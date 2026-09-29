# zing_agent — 正式客户端桌面端

面向最终用户的 agent 客户端（区别于 agent_tool 调试工具）：用户感知不到
thin_agent 服务，客户端代管安装/连接/会话。

## 定位与演进（Phase）

- **P1 聊天客户端**（当前）：纯对话 UI——会话侧栏+流式对话+工具调用折叠+审批卡片+命令按钮化。连接现有 thin_agent WS 服务（ws://host:port/ws）开发验证
- **P2 本机服务代管**：检测 WSL/本机 → 一键安装 thin_agent（内置二进制投放+配置+systemd 起停）→ /stats 健康检查 → 断线自动拉起
- **P3 远程后端**：ssh 主机管理+远程安装+多后端切换（libssh2）
- P4 体验项：托盘/系统通知/自动升级/Artifacts 查看器

## 技术栈（方案 A：webview 模式扩展）

C++ 宿主 + 系统 webview（WebKitGTK/WKWebView/WebView2）+ 单文件 HTML——
与 agent_tool 同构，零框架依赖，与"纯 C++ 零三方依赖"哲学一致。

## 构建（三平台）

| 系统 | WebView | 组播发现 | 用户数据落盘 |
|------|---------|----------|--------------|
| Linux | WebKitGTK | ✅ UDP | `~/.config/zing_agent/state.json` |
| macOS | WKWebView | ✅ UDP | `~/.config/zing_agent/state.json`（沿用 XDG 惯例） |
| Windows | Edge WebView2 | 🚧 手动填地址 | `%APPDATA%\zing_agent\state.json` |

> 前置：submodule 初始化（`git submodule update --init`——webview 头在里面）。
> Linux/macOS 另需 thin_agent 仓库同级（`-DTHIN_AGENT_ROOT`，discovery+nlohmann 来源）；
> **Windows 零外部仓依赖**（discovery 不编，json 已 vendored 本仓）。
> Windows 构建细节（NuGet 镜像/故障排查/环境变量）见 [BUILD-WINDOWS.md](BUILD-WINDOWS.md)。

> 构建目录按平台命名（`build-linux`/`build-mac`/`build-win`）——同机多平台
> 切换不互踩缓存，产物路径一目了然。

### Linux

```bash
sudo apt install libwebkit2gtk-4.1-dev cmake g++
cmake -B build-linux -DTHIN_AGENT_ROOT=/path/to/thin_agent
cmake --build build-linux -j$(nproc)
# 产物：build-linux/zing_agent
```

### macOS

```bash
cmake -B build-mac -DTHIN_AGENT_ROOT=/path/to/thin_agent
cmake --build build-mac -j$(sysctl -n hw.logicalcpu)
# 产物：build-mac/zing_agent
```

### Windows（VS2022 + WebView2）

```powershell
# Developer PowerShell for VS 2022
cd zing_agent
cmake -B build-win
cmake --build build-win --config Release
# 产物：build-win\Release\zing_agent.exe
```

与 agent_tool 完全一致——Windows 构建零 thin_agent 仓库依赖
（nlohmann/json 已 vendored 到本仓 `third_party/`，与 agent_tool 同构）。

- WebView2 SDK 由 CMake 从 NuGet 自动下载（FetchContent，无需手装）
- 测试（ctest）仅 Linux：`cd build-linux && ctest`（含 node 逻辑测试+ssh e2e——无 sshd 自动跳过）

## 目录

```
zing_agent/
├── CMakeLists.txt      # webview 宿主+HTML 嵌入（同 agent_tool 模式）
├── src/
│   ├── zing_agent.cpp      # 主程序（webview 壳+桥：cfg 三桥/svc 代管/discovery/远程）
│   ├── ConfigStore.{h,cpp} # 用户数据落盘（state.json 原子写，v7+）
│   ├── ServiceManager.{h,cpp} # P2 本机服务代管（拉起/探活/watch 守护）
│   ├── RemoteBackend.{h,cpp}  # P3 远程 ssh 安装/启停（start.sh 投放）
│   └── DebugLogger.{h,cpp}
├── web/
│   └── chat.html       # 纯聊天 UI（会话/流式/审批/主题三态/KV 持久层）
├── tests/              # ctest 7 件套（config_store/service_manager/remote
│                       #   e2e/remote_logic/chat_logic/theme/render——后三者 node）
└── assets/             # 图标等
```

## 移动端

zing_app（以后同仓库目录）。
