#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "cepluginsdk.h"

#ifdef CE_MCP_CORE_EXPORTS
#define CE_MCP_CORE_API __declspec(dllexport)
#else
#define CE_MCP_CORE_API __declspec(dllimport)
#endif

// Core 导出给 Loader 的接口函数
extern "C" {
    CE_MCP_CORE_API BOOL __stdcall Core_Initialize(ExportedFunctions* ef, int pluginId, const char* pluginDir);
    CE_MCP_CORE_API BOOL __stdcall Core_Disable(void);
    CE_MCP_CORE_API const char* __stdcall Core_GetVersion(void);
}
