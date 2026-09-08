# Cheat Engine 原生 C++ MCP 插件工程 (ce_mcp_plugin)

本项目为 Cheat Engine (CE) 原生 C++ 实现的 Model Context Protocol (MCP) 服务端插件，彻底摒弃 Python 依赖与外部中间代理。

## 1. 架构设计

采用 **双层热重载架构 (Loader + Core)**：
1. **稳定加载器 (`ce_mcp_plugin.dll`)**：
   - 导出 CE 标准插件符号 (`CEPlugin_GetVersion`, `CEPlugin_InitializePlugin`, `CEPlugin_DisablePlugin`)。
   - 注入 CE 主菜单 `插件(L) -> MCP: Reload Core DLL`。
   - 负责在 CE 启动时动态加载 `ce_mcp_plugin_core.dll`，常驻 CE 进程中，极低变更频率。
2. **核心业务与服务 (`ce_mcp_plugin_core.dll`)**：
   - 内置轻量化纯 C++ HTTP/SSE 引擎（端口 5556）。
   - 实现了 JSON-RPC 2.0 协议分发与 29 个全功能 CE 工具。
   - 支持热重载：在 CE 菜单点击热重载后，可直接释放旧 DLL 并重新载入新版本，无需重启 Cheat Engine。

## 2. 工具清册 (29 Tools)

- **进程生命周期与附加**：`ce_open_process`, `ce_get_opened_process`, `ce_pause_process`, `ce_unpause_process`, `ce_get_process_list`
- **内存读写与扫描**：`ce_read_memory`, `ce_write_memory`, `ce_aob_scan`, `ce_get_module_list`, `ce_get_symbol_address`
- **汇编与代码注入**：`ce_disassemble`, `ce_assemble`, `ce_auto_assemble`
- **地址表 (Cheat Table)**：`ce_create_record`, `ce_delete_record`, `ce_get_record_by_description`, `ce_freeze_record`, `ce_unfreeze_record`, `ce_set_record_value`
- **调试与断点**：`ce_set_breakpoint`, `ce_remove_breakpoint`, `ce_get_registers`
- **Lua 穿透引擎**：`ce_lua_eval`, `ce_lua_execute`
- **结构体解构与内存保护**：`ce_create_structure`, `ce_virtual_query`, `ce_virtual_protect`, `ce_show_message`, `ce_get_version`

## 3. 构建与部署

在项目根目录下执行 PowerShell 编译脚本：
```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1
```
产物输出至 `bin/` 目录：
- `ce_mcp_plugin.dll` -> 部署至 `<CE安装目录>\plugins\ce_mcp_plugin.dll`
- `ce_mcp_plugin_core.dll` -> 部署至 `<CE安装目录>\plugins\ce_mcp\ce_mcp_plugin_core.dll`

## 4. 全自动化测试验证

针对靶机程序运行自动化测试套件：
```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File ..\tools\test-ce-mcp.ps1
```
8 项用例覆盖协议握手、进程附加、模块遍历、内存读取、AOB 特征码扫描、Lua 穿透、AA 脚本注入、Cheat Table 项增删与冻结。
