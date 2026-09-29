# zing_agent Windows 构建指南

## 前置要求

| 组件 | 版本 | 说明 |
|---|---|---|
| Visual Studio 2022 | 含 C++ 桌面开发工作负载 | MSVC v143 |
| CMake | ≥3.16（VS 自带即可） | |
| WebView2 Loader | NuGet 自动下载（1.0.1150.38） | FetchContent 自动拉 |
| （已无） | — | Windows 零 thin_agent 仓依赖：discovery 不编，nlohmann/json vendored 于本仓 `third_party/` |

## 构建步骤

```powershell
# 1) 克隆（若未）
git clone <thin_agent_frontends> D:\code\thin_agent_frontends
# 注意：submodule（agent_tool/third_party/webview）必须初始化：
git -C D:\code\thin_agent_frontends submodule update --init

# 2) 构建（x64 Native Tools Command Prompt 或 Developer PowerShell）
cd D:\code\thin_agent_frontends\zing_agent
cmake -B build-win
cmake --build build-win --config Release

# 3) 产物
#   build-win\Release\zing_agent.exe
#（build 目录约定按平台命名：build-linux / build-mac / build-win——同机多平台不互踩）
```

## 用户数据位置（v7+）

C++ ConfigStore 落盘（不依赖 WebView2 的 localStorage——data:URL 下不可靠）：
`%APPDATA%\zing_agent\state.json`（主题/会话/消息/主机档案）。可用环境变量 `ZING_STATE_PATH` 覆盖（测试用）。

## 服务代管配置（环境变量）

| 变量 | 默认 | 说明 |
|---|---|---|
| `ZING_SERVICE_BIN` | WSL 探测路径 | thin_agent 可执行文件 |
| `ZING_SERVICE_CONFIG` | ~/code/thin_agent/config/demo.model.yaml | 模型配置 |
| `ZING_SERVICE_PROFILE` | zai_main_demo | profile |
| `ZING_SERVICE_PORT` | 8765 | 服务端口 |
| `ZING_SERVICE_ENV` | ~/.thin_agent/zai.env | API key 注入 |

## 运行时文件位置（Windows）

| 文件 | 位置 |
|---|---|
| 服务日志 | `%TEMP%\zing_thin_agent.log` |
| 服务 pid | `%TEMP%\zing_svc.pid` |
| 用户数据 | `%APPDATA%\zing_agent\state.json` |

## 连接 WSL 里的服务（地址怎么填）

WSL2 下 Windows 侧访问 Linux 服务：`localhostForwarding`（Win10/11 默认开启）时
`ws://127.0.0.1:8765/ws` 直通 WSL；若不通（旧 WSL 或关闭转发），改填 WSL IP：
WSL 内 `hostname -I | awk '{print $1}'` 得到如 `172.20.237.76`，用
`ws://172.20.237.76:8765/ws`。

## Windows 侧功能边界

- **可用**：聊天/P1 全部 UI、审批卡、斜杠命令、心跳重连、本机服务代管（svcStatus/svcStart/svcStop——thin_agent 需以 WSL 或 Windows 原生方式另行部署）
- **不可用**：UDP 组播发现（`discoverBackends` 返回空 stub——需手动填后端地址）
- **远程后端**：ssh/scp 依赖 Windows 10+ 自带 OpenSSH（`C:\Windows\System32\OpenSSH\`），非标安装路径需在 PATH

## 常见问题

- **NuGet 下载慢/失败**：WEBVIEW2_VERSION 换国内镜像或手动放置解压包至 `build-win/_deps/mswebview2-src/`
- **找不到 nlohmann/json**：确认本仓 `third_party/nlohmann/json.hpp` 存在（vendored 单头，与 thin_agent 仓同源）
- **链接错误 ws2_32**：CMake 已自动链接，若手动工程需加 `ws2_32.lib`
