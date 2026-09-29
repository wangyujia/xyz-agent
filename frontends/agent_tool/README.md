# thin_agent Desktop

webview 桌面客户端 — 原生窗口加载 `ws_agent.html`，UDP 组播自动发现局域网后端。

## 平台

| 系统 | WebView | 需要安装 | 组播发现 |
|------|---------|----------|----------|
| Linux | WebKitGTK | `libwebkit2gtk-4.1-dev` | ✅ UDP |
| macOS | WKWebView | 系统内置 | ✅ UDP |
| Windows | Edge WebView2 | Win10+ 内置 | 🚧 手动填地址（discovery 未适配 Winsock） |

> 前置：thin_agent 仓库需与本仓库同级（`-DTHIN_AGENT_ROOT` 指向它），
> 且本仓库 submodule 必须初始化（webview 头文件在里面）：
> `git submodule update --init`

## 构建

> 构建目录按平台命名（`build-linux`/`build-mac`/`build-win`）——同机多平台
> 切换不互踩缓存，产物路径一目了然。

### Linux

```bash
sudo apt install libwebkit2gtk-4.1-dev cmake g++
cmake -B build-linux -DTHIN_AGENT_ROOT=/path/to/thin_agent
cmake --build build-linux -j$(nproc)
# 产物：build-linux/agent_tool
```

### macOS

```bash
cmake -B build-mac -DTHIN_AGENT_ROOT=/path/to/thin_agent
cmake --build build-mac -j$(sysctl -n hw.logicalcpu)
# 产物：build-mac/agent_tool
```

### Windows

#### 环境要求

- **Windows 10+**（WebView2 Runtime 已内置）
- **Visual Studio 2022**（Community 版即可），安装时勾选「使用 C++ 的桌面开发」
- **CMake 3.16+**（[下载](https://cmake.org/download/)），安装时勾选「Add CMake to system PATH」

#### 构建步骤

从开始菜单打开 **「Developer Command Prompt for VS 2022」**：

```cmd
cd agent_tool
cmake -B build-win
cmake --build build-win --config Release
```

产物：`build-win\Release\agent_tool.exe`

也可用 CMake GUI：Generator 选 **Visual Studio 17 2022**，Platform 选 **x64**，build 目录填 `build-win`。

#### 首次构建注意

- CMake 通过 NuGet 自动下载 WebView2 SDK（约 15MB），需要网络
- 编译时自动将 `ws_agent.html` 嵌入二进制，无需外部文件
- 产物双击即可运行（`WIN32` 子系统，无命令行窗口）
- 日志输出到可执行文件同目录下的 `log/debug.log`（最大 10MB，自动轮转）

#### 故障排查

| 现象 | 原因 | 解决 |
|---|---|---|
| `cmake` 不是内部命令 | CMake 未加入 PATH | 重装 CMake 勾选 PATH，或从 VS 开发者命令行运行 |
| WebView2 SDK 下载失败 | 网络 / 代理 | 设置 `HTTPS_PROXY` 后重试 |
| 找不到 `rc.exe` | 未安装 Windows SDK | VS Installer → 修改 → 勾选「Windows 10 SDK」 |
| 启动无窗口 | WebView2 Runtime 缺失 | [下载 WebView2 Evergreen](https://developer.microsoft.com/microsoft-edge/webview2/) |
| 中文乱码 | 编码问题 | CMakeLists 已设 `/utf-8`，检查源文件编码为 UTF-8 |

## 功能

- **自动发现**：启动时 UDP 组播探头，收集局域网内 `thin_agentd` 后端
- **C++ → JS 桥接**：`discoverBackends()` 原生函数暴露给页面，浏览器下不可用
- **HTML 嵌入**：`ws_agent.html` 编译期嵌入二进制，单文件分发
- **已保存后端降级**：无原生发现时回退到 localStorage 保存的后端列表
- **调试日志**：`DebugLogger` 写入 `log/debug.log`，轮转保留一份备份

## 目录

```
agent_tool/
├── CMakeLists.txt
├── src/
│   ├── agent_tool.cpp         # 主程序（webview 壳 + 组播发现）
│   ├── agent_tool.rc.in       # Windows 资源模板（图标/版本信息）
│   ├── DebugLogger.h          # 日志轮转（10MB 限制）
│   └── DebugLogger.cpp
├── web/
│   └── ws_agent.html          # 前端界面（嵌入到可执行文件）
└── build-linux|mac|win/   # 按平台命名的构建目录（gitignore 覆盖 build*/）
```
