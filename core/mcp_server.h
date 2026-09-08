#pragma once
#include "ce_utils.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include "json.hpp"
#include "cepluginsdk.h"

using json = nlohmann::json;

class McpServer {
public:
    McpServer(ExportedFunctions* ef, int port = 5556);
    ~McpServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }

    // 主线程调用接口包装
    std::string DispatchToolCall(const std::string& toolName, const json& args);

private:
    void ServerWorker();
    void HandleClient(SOCKET clientSock);
    
    // HTTP/SSE 协议处理
    void HandleSseConnect(SOCKET clientSock, const std::string& sessionId);
    void HandlePostMessage(SOCKET clientSock, const std::string& body, const std::string& sessionId);
    void HandleRegularPost(SOCKET clientSock, const std::string& body);

    // MCP JSON-RPC 消息调度
    json ProcessJsonRpc(const json& request);
    json BuildToolsList();

    // 核心 CE 工具实现
    json Tool_GetProcessList(const json& args);
    json Tool_AttachProcess(const json& args);
    json Tool_DetachProcess(const json& args);
    json Tool_PauseProcess(const json& args);
    json Tool_UnpauseProcess(const json& args);
    json Tool_ListModules(const json& args);

    json Tool_ReadBytes(const json& args);
    json Tool_ReadInteger(const json& args);
    json Tool_ReadQword(const json& args);
    json Tool_ReadFloat(const json& args);
    json Tool_ReadDouble(const json& args);
    json Tool_ReadString(const json& args);

    json Tool_WriteBytes(const json& args);
    json Tool_WriteInteger(const json& args);
    json Tool_WriteQword(const json& args);
    json Tool_WriteFloat(const json& args);
    json Tool_WriteDouble(const json& args);
    json Tool_WriteString(const json& args);

    json Tool_AobScan(const json& args);
    json Tool_ResolvePointerChain(const json& args);
    json Tool_AutoAssemble(const json& args);
    json Tool_Disassemble(const json& args);
    json Tool_Assemble(const json& args);

    json Tool_CreateRecord(const json& args);
    json Tool_FreezeRecord(const json& args);
    json Tool_UnfreezeRecord(const json& args);

    json Tool_LuaEval(const json& args);
    json Tool_LuaExec(const json& args);
    json Tool_GetStatus(const json& args);

private:
    ExportedFunctions* m_ef;
    int m_port;
    std::atomic<bool> m_running;
    SOCKET m_listenSock;
    std::thread m_serverThread;

    std::mutex m_sessionsMutex;
    std::map<std::string, SOCKET> m_sseClients; // sessionId -> socket
};
