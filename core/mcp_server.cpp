#include "mcp_server.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <psapi.h>
#include <tlhelp32.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

static void LogCore(const char* format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);
}

McpServer::McpServer(ExportedFunctions* ef, int port)
    : m_ef(ef), m_port(port), m_running(false), m_listenSock(INVALID_SOCKET) {
}

McpServer::~McpServer() {
    Stop();
}

bool McpServer::Start() {
    if (m_running) return true;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LogCore("[CE_MCP_CORE] WSAStartup failed.\n");
        return false;
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        LogCore("[CE_MCP_CORE] socket creation failed.\n");
        WSACleanup();
        return false;
    }

    int opt = 1;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr = { 0 };
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port = htons((u_short)m_port);

    if (bind(m_listenSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LogCore("[CE_MCP_CORE] bind failed on port %d, error=%d\n", m_port, WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        LogCore("[CE_MCP_CORE] listen failed, error=%d\n", WSAGetLastError());
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&McpServer::ServerWorker, this);
    LogCore("[CE_MCP_CORE] MCP Server listening on http://127.0.0.1:%d\n", m_port);
    return true;
}

void McpServer::Stop() {
    if (!m_running) return;
    m_running = false;

    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        for (auto& pair : m_sseClients) {
            closesocket(pair.second);
        }
        m_sseClients.clear();
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }

    WSACleanup();
    LogCore("[CE_MCP_CORE] MCP Server stopped.\n");
}

void McpServer::ServerWorker() {
    while (m_running) {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSock, (sockaddr*)&clientAddr, &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        std::thread([this, clientSock]() {
            this->HandleClient(clientSock);
        }).detach();
    }
}

void McpServer::HandleClient(SOCKET clientSock) {
    char buffer[16384];
    int bytesReceived = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSock);
        return;
    }
    buffer[bytesReceived] = '\0';

    std::string req(buffer);
    std::string method, path;
    std::istringstream iss(req);
    iss >> method >> path;

    // 处理跨域预检 OPTIONS
    if (method == "OPTIONS") {
        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                           "Content-Length: 0\r\n\r\n";
        send(clientSock, resp.c_str(), (int)resp.length(), 0);
        closesocket(clientSock);
        return;
    }

    // 处理 SSE 长连接：GET /sse
    if (method == "GET" && path.rfind("/sse", 0) == 0) {
        std::string sessionId = "ce-sess-" + std::to_string(GetTickCount64());
        HandleSseConnect(clientSock, sessionId);
        return;
    }

    // 处理 POST 请求（MCP 消息）
    if (method == "POST") {
        size_t bodyPos = req.find("\r\n\r\n");
        std::string body = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";

        // 检查 path 中的 sessionId: /message?sessionId=xxx
        size_t queryPos = path.find("sessionId=");
        if (queryPos != std::string::npos) {
            std::string sessId = path.substr(queryPos + 10);
            size_t ampPos = sessId.find('&');
            if (ampPos != std::string::npos) sessId = sessId.substr(0, ampPos);
            HandlePostMessage(clientSock, body, sessId);
        } else {
            // 普通 POST JSON-RPC 请求
            HandleRegularPost(clientSock, body);
        }
        return;
    }

    // 默认 HTTP 状态页
    std::string html = "<html><body><h1>Cheat Engine MCP Server (Online)</h1>"
                       "<p>Port: " + std::to_string(m_port) + "</p>"
                       "<p>Status: Running</p>"
                       "<p>SSE Endpoint: <code>/sse</code></p></body></html>";
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/html; charset=utf-8\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Content-Length: " + std::to_string(html.length()) + "\r\n\r\n" + html;
    send(clientSock, resp.c_str(), (int)resp.length(), 0);
    closesocket(clientSock);
}

void McpServer::HandleSseConnect(SOCKET clientSock, const std::string& sessionId) {
    // 发送 SSE 握手响应
    std::string sseHeader = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Connection: keep-alive\r\n"
                            "Access-Control-Allow-Origin: *\r\n\r\n";
    send(clientSock, sseHeader.c_str(), (int)sseHeader.length(), 0);

    // 发送 endpoint 事件，告知客户端向 /message?sessionId=xxx 发送 POST 消息
    std::string endpointMsg = "event: endpoint\r\ndata: /message?sessionId=" + sessionId + "\r\n\r\n";
    send(clientSock, endpointMsg.c_str(), (int)endpointMsg.length(), 0);

    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sseClients[sessionId] = clientSock;
    }
    LogCore("[CE_MCP_CORE] SSE client connected, sessionId=%s\n", sessionId.c_str());

    // 保持长连接，定期发送心跳包
    while (m_running) {
        Sleep(15000);
        std::string ping = ": keepalive\r\n\r\n";
        if (send(clientSock, ping.c_str(), (int)ping.length(), 0) == SOCKET_ERROR) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_sessionsMutex);
        m_sseClients.erase(sessionId);
    }
    closesocket(clientSock);
    LogCore("[CE_MCP_CORE] SSE client disconnected, sessionId=%s\n", sessionId.c_str());
}

void McpServer::HandlePostMessage(SOCKET clientSock, const std::string& body, const std::string& sessionId) {
    // 立即响应 HTTP 202 Accepted
    std::string resp = "HTTP/1.1 202 Accepted\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Content-Length: 0\r\n\r\n";
    send(clientSock, resp.c_str(), (int)resp.length(), 0);
    closesocket(clientSock);

    // 解析 JSON-RPC 请求并在 SSE 流中回传
    try {
        json reqJson = json::parse(body);
        json respJson = ProcessJsonRpc(reqJson);

        if (!respJson.is_null()) {
            std::string sseData = "event: message\r\ndata: " + respJson.dump() + "\r\n\r\n";
            std::lock_guard<std::mutex> lock(m_sessionsMutex);
            auto it = m_sseClients.find(sessionId);
            if (it != m_sseClients.end()) {
                send(it->second, sseData.c_str(), (int)sseData.length(), 0);
            }
        }
    } catch (const std::exception& e) {
        LogCore("[CE_MCP_CORE] JSON-RPC parse error: %s\n", e.what());
    }
}

void McpServer::HandleRegularPost(SOCKET clientSock, const std::string& body) {
    try {
        json reqJson = json::parse(body);
        json respJson = ProcessJsonRpc(reqJson);
        std::string respBody = respJson.dump();

        std::string resp = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json; charset=utf-8\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Content-Length: " + std::to_string(respBody.length()) + "\r\n\r\n" + respBody;
        send(clientSock, resp.c_str(), (int)resp.length(), 0);
    } catch (const std::exception& e) {
        std::string errBody = "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}";
        std::string resp = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(errBody.length()) + "\r\n\r\n" + errBody;
        send(clientSock, resp.c_str(), (int)resp.length(), 0);
    }
    closesocket(clientSock);
}

// MCP JSON-RPC 请求路由器
json McpServer::ProcessJsonRpc(const json& request) {
    if (!request.is_object()) return nullptr;

    std::string method = request.value("method", "");
    json id = request.contains("id") ? request["id"] : nullptr;

    json response;
    response["jsonrpc"] = "2.0";
    if (!id.is_null()) response["id"] = id;

    if (method == "initialize") {
        response["result"] = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {
                {"tools", json::object()}
            }},
            {"serverInfo", {
                {"name", "cheat-engine-mcp-server"},
                {"version", "1.0.0"}
            }}
        };
        return response;
    }

    if (method == "notifications/initialized") {
        // 不需要返回响应
        return nullptr;
    }

    if (method == "ping") {
        response["result"] = json::object();
        return response;
    }

    if (method == "tools/list") {
        response["result"] = {
            {"tools", BuildToolsList()}
        };
        return response;
    }

    if (method == "tools/call") {
        std::string toolName = request["params"].value("name", "");
        json args = request["params"].value("arguments", json::object());

        try {
            json toolResult;
            if (toolName == "ce_get_process_list") toolResult = Tool_GetProcessList(args);
            else if (toolName == "ce_attach_process") toolResult = Tool_AttachProcess(args);
            else if (toolName == "ce_detach_process") toolResult = Tool_DetachProcess(args);
            else if (toolName == "ce_pause_process") toolResult = Tool_PauseProcess(args);
            else if (toolName == "ce_unpause_process") toolResult = Tool_UnpauseProcess(args);
            else if (toolName == "ce_list_modules") toolResult = Tool_ListModules(args);

            else if (toolName == "ce_read_bytes") toolResult = Tool_ReadBytes(args);
            else if (toolName == "ce_read_integer") toolResult = Tool_ReadInteger(args);
            else if (toolName == "ce_read_qword") toolResult = Tool_ReadQword(args);
            else if (toolName == "ce_read_float") toolResult = Tool_ReadFloat(args);
            else if (toolName == "ce_read_double") toolResult = Tool_ReadDouble(args);
            else if (toolName == "ce_read_string") toolResult = Tool_ReadString(args);

            else if (toolName == "ce_write_bytes") toolResult = Tool_WriteBytes(args);
            else if (toolName == "ce_write_integer") toolResult = Tool_WriteInteger(args);
            else if (toolName == "ce_write_qword") toolResult = Tool_WriteQword(args);
            else if (toolName == "ce_write_float") toolResult = Tool_WriteFloat(args);
            else if (toolName == "ce_write_double") toolResult = Tool_WriteDouble(args);
            else if (toolName == "ce_write_string") toolResult = Tool_WriteString(args);

            else if (toolName == "ce_aob_scan") toolResult = Tool_AobScan(args);
            else if (toolName == "ce_resolve_pointer_chain") toolResult = Tool_ResolvePointerChain(args);
            else if (toolName == "ce_auto_assemble") toolResult = Tool_AutoAssemble(args);
            else if (toolName == "ce_disassemble") toolResult = Tool_Disassemble(args);
            else if (toolName == "ce_assemble") toolResult = Tool_Assemble(args);

            else if (toolName == "ce_create_record") toolResult = Tool_CreateRecord(args);
            else if (toolName == "ce_freeze_record") toolResult = Tool_FreezeRecord(args);
            else if (toolName == "ce_unfreeze_record") toolResult = Tool_UnfreezeRecord(args);

            else if (toolName == "ce_lua_eval") toolResult = Tool_LuaEval(args);
            else if (toolName == "ce_lua_exec") toolResult = Tool_LuaExec(args);
            else if (toolName == "ce_get_status") toolResult = Tool_GetStatus(args);
            else {
                response["error"] = {
                    {"code", -32601},
                    {"message", "Unknown tool: " + toolName}
                };
                return response;
            }

            response["result"] = {
                {"content", {
                    {
                        {"type", "text"},
                        {"text", toolResult.dump(2)}
                    }
                }}
            };
        } catch (const std::exception& ex) {
            response["result"] = {
                {"isError", true},
                {"content", {
                    {
                        {"type", "text"},
                        {"text", std::string("Error executing tool: ") + ex.what()}
                    }
                }}
            };
        }
        return response;
    }

    response["error"] = {
        {"code", -32601},
        {"message", "Method not found: " + method}
    };
    return response;
}
