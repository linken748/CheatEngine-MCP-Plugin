#include "ce_utils.h"
#include <stdio.h>
#include "core_interface.h"
#include "mcp_server.h"

static ExportedFunctions g_EF;
static int g_PluginID = -1;
static McpServer* g_pMcpServer = nullptr;

static void LogCoreMain(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);
}

extern "C" {
    CE_MCP_CORE_API BOOL __stdcall Core_Initialize(ExportedFunctions* ef, int pluginId, const char* pluginDir) {
        if (!ef) return FALSE;
        memcpy(&g_EF, ef, sizeof(ExportedFunctions));
        g_PluginID = pluginId;

        LogCoreMain("[CE_MCP_CORE] Initializing Core DLL, pluginId=%d, dir=%s\n", pluginId, pluginDir ? pluginDir : "null");

        if (!g_pMcpServer) {
            g_pMcpServer = new McpServer(&g_EF, 5556);
            if (!g_pMcpServer->Start()) {
                LogCoreMain("[CE_MCP_CORE] Failed to start McpServer on port 5556\n");
                delete g_pMcpServer;
                g_pMcpServer = nullptr;
                return FALSE;
            }
        }

        LogCoreMain("[CE_MCP_CORE] Core initialized and MCP Server running on port 5556.\n");
        return TRUE;
    }

    CE_MCP_CORE_API BOOL __stdcall Core_Disable(void) {
        LogCoreMain("[CE_MCP_CORE] Disabling Core DLL...\n");
        if (g_pMcpServer) {
            g_pMcpServer->Stop();
            delete g_pMcpServer;
            g_pMcpServer = nullptr;
        }
        return TRUE;
    }

    CE_MCP_CORE_API const char* __stdcall Core_GetVersion(void) {
        return "1.0.0-full-mcp-cpp";
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        if (g_pMcpServer) {
            g_pMcpServer->Stop();
            delete g_pMcpServer;
            g_pMcpServer = nullptr;
        }
        break;
    }
    return TRUE;
}
