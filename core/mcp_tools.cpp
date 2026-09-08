#include "mcp_server.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <sstream>
#include <iomanip>

#include <mutex>
static std::mutex g_LuaMutex;

// 辅助：执行 CE Lua 表达式并获取结果字符串（使用锁与 pcall 保护）
static std::string ExecuteCeLua(ExportedFunctions* ef, const std::string& script) {
    if (!ef || !ef->GetLuaState) return "Error: GetLuaState not available";
    std::lock_guard<std::mutex> lock(g_LuaMutex);

    lua_State* L = ef->GetLuaState();
    if (!L) return "Error: lua_State is null";

    int top = lua_gettop(L);

    // loadstring
    if (luaL_loadstring(L, script.c_str()) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "loadstring error";
        lua_settop(L, top);
        return "Lua Error: " + err;
    }

    // pcall
    if (lua_pcall(L, 0, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "pcall error";
        lua_settop(L, top);
        return "Lua Error: " + err;
    }

    std::string result = "";
    int nres = lua_gettop(L) - top;
    if (nres > 0) {
        if (lua_isstring(L, -1) || lua_isnumber(L, -1)) {
            result = lua_tostring(L, -1);
        } else if (lua_isboolean(L, -1)) {
            result = lua_toboolean(L, -1) ? "true" : "false";
        } else if (lua_istable(L, -1)) {
            result = "[table]";
        } else {
            result = "OK";
        }
    } else {
        result = "OK";
    }
    lua_settop(L, top);
    return result;
}

// 辅助：获取当前打开的进程句柄
static HANDLE GetTargetProcessHandle(ExportedFunctions* ef) {
    if (ef && ef->OpenedProcessHandle && *(ef->OpenedProcessHandle)) {
        return *(ef->OpenedProcessHandle);
    }
    return NULL;
}

// 辅助：获取当前打开的进程 ID
static DWORD GetTargetProcessID(ExportedFunctions* ef) {
    if (ef && ef->OpenedProcessID) {
        return *(ef->OpenedProcessID);
    }
    return 0;
}

// 1. 获取进程列表
json McpServer::Tool_GetProcessList(const json& args) {
    json procList = json::array();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                char exeNameA[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exeNameA, MAX_PATH, NULL, NULL);
                procList.push_back({
                    {"pid", pe.th32ProcessID},
                    {"name", exeNameA}
                });
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    return {{"processes", procList}, {"count", procList.size()}};
}

// 2. 附加目标进程
json McpServer::Tool_AttachProcess(const json& args) {
    DWORD pid = 0;
    if (args.contains("pid")) {
        pid = args["pid"].get<DWORD>();
    } else if (args.contains("process_name")) {
        std::string procName = args["process_name"].get<std::string>();
        if (m_ef && m_ef->getProcessIDFromProcessName) {
            pid = m_ef->getProcessIDFromProcessName((char*)procName.c_str());
        }
    }

    if (pid == 0) {
        return {{"success", false}, {"error", "Process not found or PID is 0"}};
    }

    if (m_ef && m_ef->openProcessEx) {
        m_ef->openProcessEx(pid);
        return {
            {"success", true},
            {"attached_pid", pid},
            {"message", "Successfully attached to process"}
        };
    }
    return {{"success", false}, {"error", "openProcessEx not available"}};
}

// 3. 脱离进程
json McpServer::Tool_DetachProcess(const json& args) {
    if (m_ef && m_ef->openProcessEx) {
        m_ef->openProcessEx(0); // 传 0 清空
        return {{"success", true}, {"message", "Detached from target process"}};
    }
    return {{"success", false}, {"error", "Failed to detach"}};
}

// 4. 挂起与恢复进程
json McpServer::Tool_PauseProcess(const json& args) {
    if (m_ef && m_ef->pause) {
        m_ef->pause();
        return {{"success", true}, {"message", "Target process paused"}};
    }
    return {{"success", false}, {"error", "pause API not available"}};
}

json McpServer::Tool_UnpauseProcess(const json& args) {
    if (m_ef && m_ef->unpause) {
        m_ef->unpause();
        return {{"success", true}, {"message", "Target process resumed"}};
    }
    return {{"success", false}, {"error", "unpause API not available"}};
}

// 5. 模块枚举
json McpServer::Tool_ListModules(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) {
        return {{"success", false}, {"error", "No process currently attached"}};
    }

    HMODULE hMods[1024];
    DWORD cbNeeded;
    json modules = json::array();

    if (EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL)) {
        int count = cbNeeded / sizeof(HMODULE);
        for (int i = 0; i < count; i++) {
            char modName[MAX_PATH];
            char modPath[MAX_PATH];
            MODULEINFO modInfo = { 0 };

            GetModuleBaseNameA(hProcess, hMods[i], modName, sizeof(modName));
            GetModuleFileNameExA(hProcess, hMods[i], modPath, sizeof(modPath));
            GetModuleInformation(hProcess, hMods[i], &modInfo, sizeof(modInfo));

            std::stringstream ssBase;
            ssBase << "0x" << std::uppercase << std::hex << (UINT_PTR)modInfo.lpBaseOfDll;

            modules.push_back({
                {"name", modName},
                {"path", modPath},
                {"base_address", ssBase.str()},
                {"size", modInfo.SizeOfImage}
            });
        }
    }
    return {{"success", true}, {"modules", modules}, {"count", modules.size()}};
}

// 6. 内存读取函数族
json McpServer::Tool_ReadBytes(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    std::string addrStr = args.value("address", "");
    size_t count = args.value("count", 4);
    UINT_PTR address = ParseAddress(m_ef, addrStr);
    if (!address) return {{"success", false}, {"error", "Invalid address"}};

    std::vector<BYTE> buffer(count);
    SIZE_T bytesRead = 0;
    BOOL ok = FALSE;

    if (m_ef->ReadProcessMemory && *(m_ef->ReadProcessMemory)) {
        ok = (*(m_ef->ReadProcessMemory))(hProcess, (LPCVOID)address, buffer.data(), count, &bytesRead);
    } else {
        ok = ReadProcessMemory(hProcess, (LPCVOID)address, buffer.data(), count, &bytesRead);
    }

    if (!ok || bytesRead == 0) {
        return {{"success", false}, {"error", "ReadProcessMemory failed", "win_err", GetLastError()}};
    }

    std::stringstream ssHex;
    json byteArr = json::array();
    for (size_t i = 0; i < bytesRead; ++i) {
        byteArr.push_back(buffer[i]);
        ssHex << std::uppercase << std::setfill('0') << std::setw(2) << std::hex << (int)buffer[i] << " ";
    }

    return {
        {"success", true},
        {"address", ToHex(address)},
        {"bytes_read", bytesRead},
        {"hex", ssHex.str()},
        {"bytes", byteArr}
    };
}

json McpServer::Tool_ReadInteger(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    if (!address) return {{"success", false}, {"error", "Invalid address"}};

    int32_t val = 0;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, &val, sizeof(val), &bytesRead);
    if (bytesRead == sizeof(val)) {
        return {{"success", true}, {"address", ToHex(address)}, {"value", val}};
    }
    return {{"success", false}, {"error", "Failed to read integer"}};
}

json McpServer::Tool_ReadQword(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    if (!address) return {{"success", false}, {"error", "Invalid address"}};

    uint64_t val = 0;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, &val, sizeof(val), &bytesRead);
    if (bytesRead == sizeof(val)) {
        std::stringstream ss;
        ss << "0x" << std::uppercase << std::hex << val;
        return {{"success", true}, {"address", ToHex(address)}, {"value", val}, {"value_hex", ss.str()}};
    }
    return {{"success", false}, {"error", "Failed to read qword"}};
}

json McpServer::Tool_ReadFloat(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    float val = 0.0f;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, &val, sizeof(val), &bytesRead);
    if (bytesRead == sizeof(val)) {
        return {{"success", true}, {"address", ToHex(address)}, {"value", val}};
    }
    return {{"success", false}, {"error", "Failed to read float"}};
}

json McpServer::Tool_ReadDouble(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    double val = 0.0;
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, &val, sizeof(val), &bytesRead);
    if (bytesRead == sizeof(val)) {
        return {{"success", true}, {"address", ToHex(address)}, {"value", val}};
    }
    return {{"success", false}, {"error", "Failed to read double"}};
}

json McpServer::Tool_ReadString(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    size_t maxLen = args.value("max_length", 256);
    std::vector<char> buf(maxLen + 1, 0);
    SIZE_T bytesRead = 0;
    ReadProcessMemory(hProcess, (LPCVOID)address, buf.data(), maxLen, &bytesRead);
    buf[bytesRead] = '\0';
    return {{"success", true}, {"address", ToHex(address)}, {"value", std::string(buf.data())}};
}

// 7. 内存写入函数族
json McpServer::Tool_WriteBytes(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    if (!address) return {{"success", false}, {"error", "Invalid address"}};

    std::vector<BYTE> data;
    if (args.contains("bytes") && args["bytes"].is_array()) {
        for (auto& b : args["bytes"]) {
            data.push_back((BYTE)b.get<int>());
        }
    } else if (args.contains("hex")) {
        // 从十六进制字符串解析，如 "90 90 90"
        std::string hexStr = args["hex"].get<std::string>();
        std::stringstream ss(hexStr);
        std::string byteStr;
        while (ss >> byteStr) {
            data.push_back((BYTE)std::stoul(byteStr, nullptr, 16));
        }
    }

    if (data.empty()) return {{"success", false}, {"error", "No byte data provided"}};

    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, data.data(), data.size(), &bytesWritten);
    return {
        {"success", ok && (bytesWritten == data.size())},
        {"address", ToHex(address)},
        {"bytes_written", bytesWritten}
    };
}

json McpServer::Tool_WriteInteger(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    int32_t val = args.value("value", 0);
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, &val, sizeof(val), &bytesWritten);
    return {{"success", ok && bytesWritten == sizeof(val)}, {"address", ToHex(address)}, {"value", val}};
}

json McpServer::Tool_WriteQword(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    uint64_t val = args.value("value", 0ULL);
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, &val, sizeof(val), &bytesWritten);
    return {{"success", ok && bytesWritten == sizeof(val)}, {"address", ToHex(address)}, {"value", val}};
}

json McpServer::Tool_WriteFloat(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    float val = args.value("value", 0.0f);
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, &val, sizeof(val), &bytesWritten);
    return {{"success", ok && bytesWritten == sizeof(val)}, {"address", ToHex(address)}, {"value", val}};
}

json McpServer::Tool_WriteDouble(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    double val = args.value("value", 0.0);
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, &val, sizeof(val), &bytesWritten);
    return {{"success", ok && bytesWritten == sizeof(val)}, {"address", ToHex(address)}, {"value", val}};
}

json McpServer::Tool_WriteString(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    UINT_PTR address = ParseAddress(m_ef, args.value("address", ""));
    std::string text = args.value("value", "");
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)address, text.c_str(), text.length() + 1, &bytesWritten);
    return {{"success", ok}, {"address", ToHex(address)}, {"bytes_written", bytesWritten}};
}

// 8. 原生高性能 C++ AOB 特征码扫描
json McpServer::Tool_AobScan(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    std::string patternStr = args.value("pattern", "");
    if (patternStr.empty()) return {{"success", false}, {"error", "Pattern is empty"}};

    // 解析 pattern 为字节和掩码（-1 表示通配符）
    std::vector<int> pattern;
    std::stringstream ss(patternStr);
    std::string token;
    while (ss >> token) {
        if (token == "?" || token == "??" || token == "*") {
            pattern.push_back(-1);
        } else {
            try {
                pattern.push_back(std::stoi(token, nullptr, 16));
            } catch (...) {
                pattern.push_back(-1);
            }
        }
    }
    if (pattern.empty()) return {{"success", false}, {"error", "Invalid pattern"}};

    std::string moduleName = args.value("module_name", "");
    UINT_PTR scanStart = 0;
    UINT_PTR scanEnd = 0;

    if (!moduleName.empty()) {
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL)) {
            int count = cbNeeded / sizeof(HMODULE);
            for (int i = 0; i < count; i++) {
                char modName[MAX_PATH];
                GetModuleBaseNameA(hProcess, hMods[i], modName, sizeof(modName));
                if (_stricmp(modName, moduleName.c_str()) == 0) {
                    MODULEINFO mi = { 0 };
                    GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi));
                    scanStart = (UINT_PTR)mi.lpBaseOfDll;
                    scanEnd = scanStart + mi.SizeOfImage;
                    break;
                }
            }
        }
    }

    if (scanStart == 0) {
        scanStart = 0x10000;
        scanEnd = 0x7FFFFFFFFFFF;
    }

    json matches = json::array();
    UINT_PTR curr = scanStart;
    const size_t chunkSize = 65536;
    std::vector<BYTE> buffer(chunkSize + pattern.size());

    MEMORY_BASIC_INFORMATION mbi;
    while (curr < scanEnd && VirtualQueryEx(hProcess, (LPCVOID)curr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT && 
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD)) {

            UINT_PTR regionEnd = (UINT_PTR)mbi.BaseAddress + mbi.RegionSize;
            if (regionEnd > scanEnd) regionEnd = scanEnd;
            UINT_PTR blockCurr = (UINT_PTR)mbi.BaseAddress;

            while (blockCurr < regionEnd) {
                SIZE_T toRead = min(chunkSize, (size_t)(regionEnd - blockCurr));
                SIZE_T bytesRead = 0;
                if (ReadProcessMemory(hProcess, (LPCVOID)blockCurr, buffer.data(), toRead, &bytesRead) && bytesRead >= pattern.size()) {
                    for (size_t i = 0; i + pattern.size() <= bytesRead; ++i) {
                        bool matched = true;
                        for (size_t p = 0; p < pattern.size(); ++p) {
                            if (pattern[p] != -1 && buffer[i + p] != (BYTE)pattern[p]) {
                                matched = false;
                                break;
                            }
                        }
                        if (matched) {
                            matches.push_back(ToHex(blockCurr + i));
                            if (matches.size() >= 100) goto done_scan;
                        }
                    }
                }
                blockCurr += toRead;
            }
        }
        curr = (UINT_PTR)mbi.BaseAddress + mbi.RegionSize;
        if (curr <= (UINT_PTR)mbi.BaseAddress) break;
    }

done_scan:
    return {{"success", true}, {"matches", matches}, {"count", matches.size()}};
}

// 9. 多级指针链解析
json McpServer::Tool_ResolvePointerChain(const json& args) {
    HANDLE hProcess = GetTargetProcessHandle(m_ef);
    if (!hProcess) return {{"success", false}, {"error", "No process attached"}};

    std::string baseStr = args.value("base_address", "");
    UINT_PTR curr = ParseAddress(m_ef, baseStr);
    if (!curr) return {{"success", false}, {"error", "Invalid base address"}};

    std::vector<int> offsets;
    if (args.contains("offsets") && args["offsets"].is_array()) {
        for (auto& off : args["offsets"]) {
            offsets.push_back(off.get<int>());
        }
    }

    bool is64 = true;
    BOOL wow64 = FALSE;
    if (IsWow64Process(hProcess, &wow64) && wow64) {
        is64 = false;
    }

    for (size_t i = 0; i < offsets.size(); i++) {
        UINT_PTR ptrVal = 0;
        SIZE_T read = 0;
        if (is64) {
            uint64_t v = 0;
            ReadProcessMemory(hProcess, (LPCVOID)curr, &v, sizeof(v), &read);
            if (read != sizeof(v)) return {{"success", false}, {"error", "Dereference failed at step " + std::to_string(i)}};
            ptrVal = (UINT_PTR)v;
        } else {
            uint32_t v = 0;
            ReadProcessMemory(hProcess, (LPCVOID)curr, &v, sizeof(v), &read);
            if (read != sizeof(v)) return {{"success", false}, {"error", "Dereference failed at step " + std::to_string(i)}};
            ptrVal = (UINT_PTR)v;
        }
        curr = ptrVal + offsets[i];
    }

    return {{"success", true}, {"final_address", ToHex(curr)}};
}

// 10. Auto Assembler 注入
json McpServer::Tool_AutoAssemble(const json& args) {
    std::string script = args.value("script", "");
    if (script.empty()) return {{"success", false}, {"error", "Script is empty"}};

    if (m_ef && m_ef->AutoAssemble) {
        BOOL ok = m_ef->AutoAssemble((char*)script.c_str());
        return {{"success", ok != FALSE}, {"message", (ok != FALSE) ? "AA script executed successfully" : "AA script failed"}};
    }
    return {{"success", false}, {"error", "AutoAssemble not available"}};
}

// 11. 反汇编与汇编
json McpServer::Tool_Disassemble(const json& args) {
    UINT_PTR addr = ParseAddress(m_ef, args.value("address", ""));
    if (!addr) return {{"success", false}, {"error", "Invalid address"}};

    char outBuf[512] = { 0 };
    if (m_ef && m_ef->Disassembler) {
        BOOL ok = m_ef->Disassembler(addr, outBuf, sizeof(outBuf) - 1);
        return {{"success", ok != FALSE}, {"address", ToHex(addr)}, {"assembly", outBuf}};
    }
    return {{"success", false}, {"error", "Disassembler not available"}};
}

json McpServer::Tool_Assemble(const json& args) {
    UINT_PTR addr = ParseAddress(m_ef, args.value("address", ""));
    std::string inst = args.value("instruction", "");
    BYTE outBytes[32] = { 0 };
    int retSize = 0;

    if (m_ef && m_ef->Assembler) {
        BOOL ok = m_ef->Assembler(addr, (char*)inst.c_str(), outBytes, sizeof(outBytes), &retSize);
        std::stringstream ss;
        for (int i = 0; i < retSize; i++) {
            ss << std::uppercase << std::setfill('0') << std::setw(2) << std::hex << (int)outBytes[i] << " ";
        }
        return {{"success", ok != FALSE}, {"bytes", ss.str()}, {"size", retSize}};
    }
    return {{"success", false}, {"error", "Assembler not available"}};
}

// 12. Cheat Table 地址表管理
json McpServer::Tool_CreateRecord(const json& args) {
    if (!m_ef || !m_ef->createTableEntry) return {{"success", false}, {"error", "createTableEntry not available"}};

    PVOID memrec = m_ef->createTableEntry();
    if (!memrec) return {{"success", false}, {"error", "Failed to create table entry"}};

    std::string desc = args.value("description", "New Record");
    std::string addr = args.value("address", "");
    int valType = args.value("type", 2); // 2 = dword

    if (m_ef->memrec_setDescription) m_ef->memrec_setDescription(memrec, (char*)desc.c_str());
    if (m_ef->memrec_setAddress) m_ef->memrec_setAddress(memrec, (char*)addr.c_str(), NULL, 0);
    if (m_ef->memrec_setType) m_ef->memrec_setType(memrec, valType);

    return {{"success", true}, {"description", desc}, {"address", addr}};
}

json McpServer::Tool_FreezeRecord(const json& args) {
    std::string desc = args.value("description", "");
    if (m_ef && m_ef->getTableEntry && m_ef->memrec_freeze) {
        PVOID rec = m_ef->getTableEntry((char*)desc.c_str());
        if (rec) {
            m_ef->memrec_freeze(rec, 0);
            bool isFrozen = true;
            if (m_ef->memrec_isfrozen) {
                isFrozen = (m_ef->memrec_isfrozen(rec) != FALSE);
            }
            return {{"success", isFrozen}, {"description", desc}, {"frozen", isFrozen}};
        }
    }
    return {{"success", false}, {"error", "Record not found"}};
}

json McpServer::Tool_UnfreezeRecord(const json& args) {
    std::string desc = args.value("description", "");
    if (m_ef && m_ef->getTableEntry && m_ef->memrec_unfreeze) {
        PVOID rec = m_ef->getTableEntry((char*)desc.c_str());
        if (rec) {
            m_ef->memrec_unfreeze(rec);
            bool isFrozen = false;
            if (m_ef->memrec_isfrozen) {
                isFrozen = (m_ef->memrec_isfrozen(rec) != FALSE);
            }
            return {{"success", !isFrozen}, {"description", desc}, {"frozen", isFrozen}};
        }
    }
    return {{"success", false}, {"error", "Record not found"}};
}

// 13. 原生 Lua 引擎调用
json McpServer::Tool_LuaEval(const json& args) {
    std::string expr = args.value("expression", "");
    std::string luaCode = "return (" + expr + ")";
    std::string res = ExecuteCeLua(m_ef, luaCode);
    return {{"success", true}, {"expression", expr}, {"result", res}};
}

json McpServer::Tool_LuaExec(const json& args) {
    std::string script = args.value("script", "");
    std::string res = ExecuteCeLua(m_ef, script);
    return {{"success", true}, {"result", res}};
}

// 14. 获取状态
json McpServer::Tool_GetStatus(const json& args) {
    DWORD pid = GetTargetProcessID(m_ef);
    std::string procName = "";
    if (m_ef && m_ef->GetProcessNameFromID && pid != 0) {
        char nameBuf[MAX_PATH] = { 0 };
        // 尝试从 PID 获取名
    }

    return {
        {"status", "online"},
        {"version", "1.0.0"},
        {"attached_pid", pid},
        {"is_attached", (pid != 0)},
        {"mcp_port", m_port}
    };
}

// 构建 Tools 列表规范（定义所有 25+ 个工具的 JSON Schema）
json McpServer::BuildToolsList() {
    return json::array({
        {
            {"name", "ce_get_status"},
            {"description", "Get Cheat Engine MCP Server status, version, and attached target information."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_get_process_list"},
            {"description", "Enumerate all running processes in the system."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_attach_process"},
            {"description", "Attach Cheat Engine to a target process by PID or process name."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"pid", {{"type", "integer"}, {"description", "Target Process ID"}}},
                    {"process_name", {{"type", "string"}, {"description", "Target process executable name (e.g. Tutorial-x86_64.exe)"}}}
                }}
            }}
        },
        {
            {"name", "ce_detach_process"},
            {"description", "Detach Cheat Engine from the current target process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_pause_process"},
            {"description", "Pause/freeze the currently attached process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_unpause_process"},
            {"description", "Resume the currently paused target process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_list_modules"},
            {"description", "List all loaded modules (DLLs and EXE) of the attached target process."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        {
            {"name", "ce_read_bytes"},
            {"description", "Read raw bytes from the target memory at a specified address."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address (e.g. 0x140000000 or module.dll+0x1000)"}}},
                    {"count", {{"type", "integer"}, {"description", "Number of bytes to read, default 4"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_read_integer"},
            {"description", "Read a 32-bit signed integer from the target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_read_qword"},
            {"description", "Read a 64-bit integer/pointer from target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_read_float"},
            {"description", "Read a 32-bit single-precision float from target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_read_double"},
            {"description", "Read a 64-bit double-precision float from target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_read_string"},
            {"description", "Read a null-terminated string from target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"max_length", {{"type", "integer"}, {"description", "Max length to read, default 256"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_write_bytes"},
            {"description", "Write raw bytes to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"hex", {{"type", "string"}, {"description", "Hex string (e.g. '90 90 90') or bytes array"}}},
                    {"bytes", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Array of byte values"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_write_integer"},
            {"description", "Write a 32-bit integer to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"value", {{"type", "integer"}, {"description", "Integer value to write"}}}
                },
                {"required", {"address", "value"}}}
            }}
        },
        {
            {"name", "ce_write_qword"},
            {"description", "Write a 64-bit integer to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"value", {{"type", "integer"}, {"description", "64-bit value to write"}}}
                },
                {"required", {"address", "value"}}}
            }}
        },
        {
            {"name", "ce_write_float"},
            {"description", "Write a float value to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"value", {{"type", "number"}, {"description", "Float value to write"}}}
                },
                {"required", {"address", "value"}}}
            }}
        },
        {
            {"name", "ce_write_double"},
            {"description", "Write a double value to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"value", {{"type", "number"}, {"description", "Double value to write"}}}
                },
                {"required", {"address", "value"}}}
            }}
        },
        {
            {"name", "ce_write_string"},
            {"description", "Write a text string to target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"value", {{"type", "string"}, {"description", "String text to write"}}}
                },
                {"required", {"address", "value"}}}
            }}
        },
        {
            {"name", "ce_aob_scan"},
            {"description", "Perform an AOB pattern scan on the attached target memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"}, {"description", "AOB pattern with wildcards, e.g. '48 89 5C 24 ?? 48 89 74 24'"}}},
                    {"module_name", {{"type", "string"}, {"description", "Optional module scope, e.g. 'Tutorial-x86_64.exe'"}}}
                },
                {"required", {"pattern"}}}
            }}
        },
        {
            {"name", "ce_resolve_pointer_chain"},
            {"description", "Resolve a multi-level pointer chain and calculate final effective address."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"base_address", {{"type", "string"}, {"description", "Base address or symbol"}}},
                    {"offsets", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "List of integer offsets"}}}
                },
                {"required", {"base_address", "offsets"}}}
            }}
        },
        {
            {"name", "ce_auto_assemble"},
            {"description", "Execute Cheat Engine Auto Assembler (AA) script to inject code or allocate memory."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"script", {{"type", "string"}, {"description", "Auto Assembler script content"}}}
                },
                {"required", {"script"}}}
            }}
        },
        {
            {"name", "ce_disassemble"},
            {"description", "Disassemble assembly instruction at given memory address."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Memory address"}}}
                },
                {"required", {"address"}}}
            }}
        },
        {
            {"name", "ce_assemble"},
            {"description", "Assemble instruction mnemonic into machine code bytes."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"address", {{"type", "string"}, {"description", "Target address context"}}},
                    {"instruction", {{"type", "string"}, {"description", "Mnemonic, e.g. 'mov eax, 1'"}}}
                },
                {"required", {"address", "instruction"}}}
            }}
        },
        {
            {"name", "ce_create_record"},
            {"description", "Create a new record in Cheat Engine's address list table."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"description", {{"type", "string"}, {"description", "Record description"}}},
                    {"address", {{"type", "string"}, {"description", "Memory address"}}},
                    {"type", {{"type", "integer"}, {"description", "Value type: 0=byte, 1=word, 2=dword, 3=int64, 4=float, 5=double"}}}
                },
                {"required", {"description", "address"}}}
            }}
        },
        {
            {"name", "ce_freeze_record"},
            {"description", "Freeze/lock a record in Cheat Engine address list."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"description", {{"type", "string"}, {"description", "Record description"}}}
                },
                {"required", {"description"}}}
            }}
        },
        {
            {"name", "ce_unfreeze_record"},
            {"description", "Unfreeze/unlock a record in Cheat Engine address list."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"description", {{"type", "string"}, {"description", "Record description"}}}
                },
                {"required", {"description"}}}
            }}
        },
        {
            {"name", "ce_lua_eval"},
            {"description", "Evaluate a Lua expression inside Cheat Engine's native Lua 5.3 engine and return value."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"expression", {{"type", "string"}, {"description", "Lua expression, e.g. 'getOpenedProcessID()'"}}}
                },
                {"required", {"expression"}}}
            }}
        },
        {
            {"name", "ce_lua_exec"},
            {"description", "Execute arbitrary Lua code block inside Cheat Engine."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"script", {{"type", "string"}, {"description", "Lua script code"}}}
                },
                {"required", {"script"}}}
            }}
        }
    });
}
