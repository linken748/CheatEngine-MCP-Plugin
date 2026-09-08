#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <sstream>
#include <iomanip>
#include "cepluginsdk.h"

inline UINT_PTR ParseAddress(ExportedFunctions* ef, const std::string& addrStr) {
    if (addrStr.empty()) return 0;
    
    if (addrStr.rfind("0x", 0) == 0 || addrStr.rfind("0X", 0) == 0) {
        try {
            return (UINT_PTR)std::stoull(addrStr, nullptr, 16);
        } catch (...) {}
    }

    try {
        return (UINT_PTR)std::stoull(addrStr, nullptr, 16);
    } catch (...) {}

    if (ef && ef->sym_nameToAddress) {
        UINT_PTR resolved = 0;
        if (ef->sym_nameToAddress((char*)addrStr.c_str(), &resolved)) {
            return resolved;
        }
    }

    return 0;
}

inline std::string ToHex(UINT_PTR val) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::hex << val;
    return ss.str();
}
