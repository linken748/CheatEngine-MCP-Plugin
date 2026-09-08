#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include "cepluginsdk.h"
#include "core_interface.h"

#pragma comment(lib, "shlwapi.lib")

// 全局变量保存 CE 提供的 ExportedFunctions 及 PluginID
static ExportedFunctions g_ExportedFunctions;
static int g_PluginID = -1;
static HMODULE g_hCoreModule = NULL;
static char g_PluginDir[MAX_PATH] = { 0 };

typedef BOOL (__stdcall *FnCoreInitialize)(ExportedFunctions* ef, int pluginId, const char* pluginDir);
typedef BOOL (__stdcall *FnCoreDisable)(void);
typedef const char* (__stdcall *FnCoreGetVersion)(void);

static FnCoreInitialize g_pfnCoreInit = NULL;
static FnCoreDisable g_pfnCoreDisable = NULL;
static FnCoreGetVersion g_pfnCoreGetVer = NULL;

static void LogLoader(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    OutputDebugStringA(buf);
}

// 动态载入 Core DLL
static BOOL LoadCoreDLL() {
    char coreDllPath[MAX_PATH];
    snprintf(coreDllPath, sizeof(coreDllPath), "%s\\ce_mcp_plugin_core.dll", g_PluginDir);

    LogLoader("[CE_MCP_LOADER] Loading Core DLL: %s\n", coreDllPath);
    g_hCoreModule = LoadLibraryA(coreDllPath);
    if (!g_hCoreModule) {
        LogLoader("[CE_MCP_LOADER] Failed to load Core DLL! Error: %lu\n", GetLastError());
        return FALSE;
    }

    g_pfnCoreInit = (FnCoreInitialize)GetProcAddress(g_hCoreModule, "Core_Initialize");
    g_pfnCoreDisable = (FnCoreDisable)GetProcAddress(g_hCoreModule, "Core_Disable");
    g_pfnCoreGetVer = (FnCoreGetVersion)GetProcAddress(g_hCoreModule, "Core_GetVersion");

    if (!g_pfnCoreInit || !g_pfnCoreDisable) {
        LogLoader("[CE_MCP_LOADER] Core exports not found!\n");
        FreeLibrary(g_hCoreModule);
        g_hCoreModule = NULL;
        return FALSE;
    }

    BOOL ok = g_pfnCoreInit(&g_ExportedFunctions, g_PluginID, g_PluginDir);
    LogLoader("[CE_MCP_LOADER] Core_Initialize result: %d\n", ok);
    return ok;
}

// 卸载 Core DLL
static void UnloadCoreDLL() {
    if (g_hCoreModule) {
        if (g_pfnCoreDisable) {
            g_pfnCoreDisable();
        }
        FreeLibrary(g_hCoreModule);
        g_hCoreModule = NULL;
        g_pfnCoreInit = NULL;
        g_pfnCoreDisable = NULL;
        g_pfnCoreGetVer = NULL;
        LogLoader("[CE_MCP_LOADER] Core DLL unloaded.\n");
    }
}

// 菜单点击回调：热重载 Core DLL
static void __stdcall OnMenuReloadCore(void) {
    LogLoader("[CE_MCP_LOADER] Hot-reload triggered from menu.\n");
    UnloadCoreDLL();
    Sleep(100);
    BOOL ok = LoadCoreDLL();
    if (g_ExportedFunctions.ShowMessage) {
        if (ok) {
            g_ExportedFunctions.ShowMessage((char*)"[CE-MCP] Core DLL successfully reloaded!");
        } else {
            g_ExportedFunctions.ShowMessage((char*)"[CE-MCP] Failed to reload Core DLL. Check debug log.");
        }
    }
}

// CE 插件必须导出的标准入口 1：版本信息
CE_EXPORT BOOL __stdcall CEPlugin_GetVersion(PPluginVersion pv, int sizeofpluginversion) {
    if (!pv || sizeofpluginversion < (int)sizeof(PluginVersion)) return FALSE;
    pv->version = CESDK_VERSION;
    static char pluginName[] = "Cheat Engine MCP Server Plugin (Hot-Reload Loader)";
    pv->pluginname = pluginName;
    return TRUE;
}

// CE 插件必须导出的标准入口 2：插件初始化
CE_EXPORT BOOL __stdcall CEPlugin_InitializePlugin(PExportedFunctions ef, int pluginid) {
    if (!ef) return FALSE;
    memcpy(&g_ExportedFunctions, ef, sizeof(ExportedFunctions));
    g_PluginID = pluginid;

    // 获取 Loader 所在目录
    HMODULE hLoader = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&CEPlugin_InitializePlugin, &hLoader);
    GetModuleFileNameA(hLoader, g_PluginDir, MAX_PATH);
    PathRemoveFileSpecA(g_PluginDir);

    LogLoader("[CE_MCP_LOADER] Initializing loader in %s, pluginId=%d\n", g_PluginDir, pluginid);

    // 注册主菜单热重载项（PluginType 5 为 ptMainMenu）
    if (ef->RegisterFunction) {
        PLUGINTYPE5_INIT menuInit = { 0 };
        static char menuName[] = "MCP: Reload Core DLL";
        menuInit.name = menuName;
        menuInit.callbackroutine = OnMenuReloadCore;
        menuInit.shortcut = NULL;
        ef->RegisterFunction(pluginid, ptMainMenu, &menuInit);
    }

    // 自动加载 Core DLL
    LoadCoreDLL();
    return TRUE;
}

// CE 插件必须导出的标准入口 3：插件卸载
CE_EXPORT BOOL __stdcall CEPlugin_DisablePlugin(void) {
    LogLoader("[CE_MCP_LOADER] Disabling loader plugin.\n");
    UnloadCoreDLL();
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        break;
    case DLL_PROCESS_DETACH:
        UnloadCoreDLL();
        break;
    }
    return TRUE;
}
