# Cheat Engine 原生 C++ MCP 插件 (CheatEngine-MCP-Plugin)

本项目为 Cheat Engine (CE) 原生 C++ 实现的 Model Context Protocol (MCP) 服务端插件，为 LLM / AI 智能体（如 Antigravity、Claude 等）提供直接与 Cheat Engine 交互的标准化接口。

## 1. 核心功能

插件通过内置的轻量化 HTTP/SSE 服务（默认监听端口 `5556`）暴露 29 个全功能 CE 工具：

- **进程生命周期与附加**：
  - `ce_open_process`: 附加目标进程（支持进程名与 PID）。
  - `ce_get_opened_process`: 获取当前已附加的进程信息。
  - `ce_pause_process` / `ce_unpause_process`: 挂起或恢复目标进程。
  - `ce_get_process_list`: 枚举系统中正在运行的进程列表。
- **内存读写与特征码扫描**：
  - `ce_read_memory`: 读取指定地址的内存数据（Hex 及数值格式）。
  - `ce_write_memory`: 向指定内存地址写入数据。
  - `ce_aob_scan`: 内存特征码（AOB Pattern）搜索，支持通配符 `??`。
  - `ce_get_module_list`: 枚举目标进程已加载的所有模块及其基地址与大小。
  - `ce_get_symbol_address`: 解析符号或模块导出函数地址。
- **汇编与代码注入**：
  - `ce_disassemble` / `ce_assemble`: 实时反汇编与机器码组装。
  - `ce_auto_assemble`: 执行 Auto Assembler (AA) 脚本，支持 Code Cave 与内存 Hook。
- **地址表 (Cheat Table)**：
  - `ce_create_record`: 新增地址表项（支持自定义名称、地址偏移与类型）。
  - `ce_delete_record`: 删除指定地址表项。
  - `ce_get_record_by_description`: 根据描述检索地址表项。
  - `ce_freeze_record` / `ce_unfreeze_record`: 锁定/解除锁定数值。
  - `ce_set_record_value`: 修改地址表项的值。
- **调试与断点**：
  - `ce_set_breakpoint` / `ce_remove_breakpoint`: 硬件断点与内存断点设置与移除。
  - `ce_get_registers`: 读取断点命中时的寄存器上下文快照。
- **CE 原生 Lua 5.3 引擎穿透**：
  - `ce_lua_eval`: 双向穿透执行 Lua 表达式并获取求值结果。
  - `ce_lua_execute`: 执行多行 CE 原生 Lua 脚本。
- **结构体与内存保护**：
  - `ce_virtual_query` / `ce_virtual_protect`: 查询与修改内存页保护属性。
  - `ce_create_structure`: 定义和解析内存结构体。
  - `ce_show_message` / `ce_get_version`: 弹出 CE 消息框与获取插件版本。

---

## 2. 架构设计与热重载

采用 **双层热重载架构 (Loader + Core)**：
1. **稳定加载器 (`ce_mcp_plugin.dll`)**：
   - 导出 CE 标准插件接口，常驻 CE 进程。
   - 注入 CE 主菜单：`插件(L) -> MCP: Reload Core DLL`。
2. **业务核心 (`ce_mcp_plugin_core.dll`)**：
   - 承载 MCP 服务端与全部 29 项逆向工具。
   - 支持热重载：更新 DLL 后，在 CE 菜单点击 `Reload Core DLL` 即可毫秒级无缝重载，无需重启 Cheat Engine。

---

## 3. 安装与使用方法

### 方式一：直接使用 Release 预编译文件（推荐）
1. 从 [Releases 页面](https://github.com/linken748/CheatEngine-MCP-Plugin/releases) 下载最新版本的压缩包（如 `ce_mcp_plugin_v1.0.0.zip`）。
2. 解压后将文件放入 Cheat Engine 目录：
   - `ce_mcp_plugin.dll` 放入 `<CE安装根目录>\plugins\`
   - `ce_mcp_plugin_core.dll` 放入 `<CE安装根目录>\plugins\ce_mcp\`
3. 启动 Cheat Engine，进入菜单：**设置 (Settings) -> 插件 (Plugins)**。
4. 勾选 **Cheat Engine MCP Server Plugin (Hot-Reload Loader)** 并保存。
5. 插件启动后将自动在 `http://127.0.0.1:5556/sse` 监听 MCP 请求。

### 方式二：源码编译
在具备 Visual Studio (MSVC x64) 的环境下执行：
```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File build.ps1
```
编译产物将输出至 `bin/` 目录。

---

## 4. 客户端配置 (MCP Client Configuration)

在支持 MCP 的客户端（如 Claude Desktop 或 Antigravity 的 `mcp_config.json`）中添加如下配置：

```json
{
  "mcpServers": {
    "cheat-engine": {
      "serverUrl": "http://127.0.0.1:5556/sse"
    }
  }
}
```
配置完成后即可通过 AI 提示词直接控制 Cheat Engine 进行动态逆向与分析。
