# Windows 启动脚本

`start-windows.bat` 和 `start-windows.ps1` 用来在本机启动后端 `thin_agent.exe`，并打开前端 `zing_agent.exe`。两个文件做的是同一件事：`.bat` 只是带上执行策略去调用同目录的 `.ps1`，方便双击。

脚本按自己的位置定位仓库根目录，请保持它们位于 `script\windows`。

## 使用前

两个程序都已经用 Visual Studio 编过 Release：

- `backends\thin_agent\build-win\Release\thin_agent.exe`
- `frontends\zing_agent\build-win\Release\zing_agent.exe`

打开前端窗口还需要系统里已安装 WebView2 运行时。没有的话，后端仍可单独启动，脚本会提示安装地址。

## 启动

在仓库根目录执行，或直接双击 `start-windows.bat`。

```bat
script\windows\start-windows.bat
```

默认行为：

1. 若 `thin_agent.exe` 旁边没有 `onnxruntime.dll`，从 `backends\thin_agent\third_party\onnxruntime\lib` 复制一份。
2. 配置优先用 `%USERPROFILE%\.thin_agent\demo.model.yaml`。这个文件不存在时，用仓库里的 `backends\thin_agent\config\demo.model.yaml`。
3. 默认 profile `local_tools_mode`，监听 `127.0.0.1:8765`。会后台托管拉起本地 llama-server（无需 API key）。缺 llama-server/GGUF 或家目录旧 YAML 无此 profile 时自动回退 `offline_mode` / 仓库配置。
4. 若 `%USERPROFILE%\.thin_agent\profiles\<profile>\config\chat_policy.json` 还不存在，从仓库复制一份。已有文件不会覆盖。
5. 后端若已在该端口健康，则不再启动第二个进程。
6. 健康检查通过后打开 `zing_agent`。界面默认连接 `ws://127.0.0.1:8765/ws`。

健康检查地址是 `http://127.0.0.1:8765/health`。日志在 `%USERPROFILE%\.thin_agent\logs\agent_svc.log`。约 60 秒内没有就绪时，脚本会打印日志末尾并退出。sidecar 在后台加载模型，首轮对话可能再多等一会儿。

## 参数

`.bat` 会把参数原样交给 `.ps1`。

```bat
script\windows\start-windows.bat -AgentProfile deepseek_fast_demo
script\windows\start-windows.bat -Port 18765 -BindHost 127.0.0.1
script\windows\start-windows.bat -NoGui
script\windows\start-windows.bat -Stop
```

也可以直接调用 PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\start-windows.ps1 -NoGui
```

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `-AgentProfile` | `local_tools_mode` | `demo.model.yaml` 里的 profile。`-Profile` 是同一参数的别名。 |
| `-Port` | `8765` | 监听端口。 |
| `-BindHost` | `127.0.0.1` | 监听地址。 |
| `-NoGui` | 关 | 只启动后端，不打开 `zing_agent`。 |
| `-Stop` | 关 | 结束本仓库编出来的 `thin_agent.exe` 和 `zing_agent.exe`。 |

云端 profile（例如 `deepseek_fast_demo`）需要对应的 API key 环境变量。密钥放在配置或环境里，不要写进这个脚本。更完整的密钥/第三方清单见仓库根目录 [`THIRD_PARTY.md`](../../THIRD_PARTY.md)。

## 本地对话：访问模式

| 方式 | 何时用 | 要不要 `llama-server` | 配置 / 通道 |
| --- | --- | --- | --- |
| **A. 进程内 GGUF** | 推荐本机离线聊天 | **不需要** | 启动 `offline_mode`；`THIN_AGENT_WITH_LLAMA_CPP=ON` 编过；GGUF 放在 `models/gguf/` |
| **B. 托管 sidecar** | 需要模型自主 tools | **自动拉起** | 侧栏切 `local_tools_mode` |
| **C. 外挂 OpenAI 兼容端点** | 对齐 LM Studio / 单独调试 server | **需自行启动** | 侧栏切 `lmstudio_mode`（`http://127.0.0.1:1234/v1`） |
| **D. 云端 API** | 智谱 / DeepSeek / 自建网关 | 不需要本地权重 | `cloud_api_mode` 或 `zai_*` / `deepseek_*`，并设置 `api_key_env` |

侧栏「模型通道」热切换只改进程内路由，**不**迁移会话/记忆目录（数据目录仍跟启动时的 `-AgentProfile`）。协议：`list_profiles` / `switch_profile`。

侧栏「本地模型」可在已注册的进程内 GGUF（如 Aurum / Qwen）之间热切换：`list_local_models` / `switch_local_model`（会卸载其它 GGUF 并加载所选模型）。需 `offline_mode`（或走 HybridRouter 的离线路径）且 `THIN_AGENT_WITH_LLAMA_CPP=ON`。

| 展示名（约） | profile | 说明 |
| --- | --- | --- |
| 离线 / 进程内 GGUF | `offline_mode` | 有 GGUF 且 ON 编译时走 HybridRouter→Aurum |
| 本地 Tools（sidecar） | `local_tools_mode` | **start-windows 默认**；thin_agent 托管拉起 llama-server |
| 外接 LM Studio | `lmstudio_mode` | 外挂 `127.0.0.1:1234`；端点不通时 UI 会提示 |
| 云 API（OpenAI 兼容） | `cloud_api_mode` | 自建/代理网关 |
| 智谱 / DeepSeek 等 | `zai_*` / `deepseek_*` | 需 `GLM_API_KEY`、`DEEPSEEK_API_KEY` 等 |

profile 定义在 `backends\thin_agent\config\demo.model.yaml`（或 `%USERPROFILE%\.thin_agent\demo.model.yaml`）。

## 下载 GGUF 模型

大文件不进 git。放到：

`backends\thin_agent\models\gguf\`

Windows 当前验证过的权重（下载后请用 sha256 核对，详见 `THIRD_PARTY.md` §2.5）：

```powershell
$dir = "backends\thin_agent\models\gguf"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

# 主用：Aurum ~3B Q4_K_M（进程内离线优先加载）
curl.exe --ssl-no-revoke -L --fail -o "$dir\aurum-brain-q4_k_m.gguf" `
  "https://github.com/aurum-lab/aurum-brain-ai/releases/download/daily-2026.11.18-07/aurum-brain-q4_k_m.gguf"

# 备用：Qwen2.5-0.5B（翻译/级联回落；上游 release 文件名不同，本地按此名保存）
curl.exe --ssl-no-revoke -L --fail -o "$dir\qwen2.5-0.5b-instruct-q2_k.gguf" `
  "https://github.com/ozuakutku/qwen2.5-0.5b-gguf/releases/download/v1.0.0/qwen2.5-0.5b.gguf"

Get-FileHash "$dir\aurum-brain-q4_k_m.gguf","$dir\qwen2.5-0.5b-instruct-q2_k.gguf" -Algorithm SHA256
```

| 本地文件名 | 约体积 | sha256（本机已核） |
| --- | --- | --- |
| `aurum-brain-q4_k_m.gguf` | 1.93 GB | `EF68A4D9C31F9B5C1186979416AA5856557FA9821917D3FDA8D5F36C8624786D` |
| `qwen2.5-0.5b-instruct-q2_k.gguf` | 379 MB | `C5396E06AF294BD101B30DCE59131A76D2B773E76950ACC870EDA801D3AB0515` |

## 进程内 GGUF（方式 A）

把 `THIN_AGENT_WITH_LLAMA_CPP=ON` 编进 `thin_agent` 后，`GgufModel` 直接加载上面的 GGUF，**不必**外挂 `llama-server`。

依赖（本机约定路径）：

- 预编译 DLL：`C:\deps-thin-agent\llama.cpp\`（`bin\*.dll` + `lib\llama.lib` / `ggml.lib`）
- 匹配版本头文件：`C:\deps-thin-agent\llama.cpp-src\`（commit 与 DLL 一致，例如 build 12503 / `4577932fa`）

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S backends\thin_agent -B backends\thin_agent\build-win -DTHIN_AGENT_WITH_LLAMA_CPP=ON
& $cmake --build backends\thin_agent\build-win --config Release --target thin_agent
```

离线 profile 下 HybridRouter 懒加载（优先 Aurum，其次 Qwen0.5B）。导入库若缺失，在 VS 开发者命令行对 `bin\llama.dll` / `ggml.dll` 用 `dumpbin /exports` + `lib /def` 生成到 `lib\`。

## 外挂 llama-server（方式 B）

默认模型路径同上 `aurum-brain-q4_k_m.gguf`。未指定 `sidecar_server_path` 时，用最后一个可写固定盘上的 `deps-thin-agent\llama.cpp\bin\llama-server.exe`；该盘没有再往前的盘找。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1
```

常用参数：`-Port 1234`、`-GpuLayers 99`、`-LlamaServer <exe>`、`-Stop`。脚本以 router 启动，`--models-max 1`，目录里的模型先不载入。

手动等价：

```powershell
& <最后一个可写盘>:\deps-thin-agent\llama.cpp\bin\llama-server.exe `
  --models-dir backends\thin_agent\models\gguf `
  --models-preset backends\thin_agent\models\gguf\router-presets.ini `
  --models-max 1 --host 127.0.0.1 --port 1234 --jinja
```

预设里 Aurum 是 32768，Qwen2.5-Coder-14B 是 16384。就绪后侧栏选「本地 Aurum（lmstudio）」；健康检查：`http://127.0.0.1:1234/v1/models`。列表里的模型此时是 unloaded。

## 云端 Key（方式 C）

在系统或当前 shell 设置环境变量（名称以 `demo.model.yaml` 里 `api_key_env` 为准），例如：

```powershell
$env:GLM_API_KEY = "..."
$env:DEEPSEEK_API_KEY = "..."
```

不要把真实 Key 写进仓库或本脚本。缺 Key 时仍可热切换到该通道，但调用可能失败（UI/WS 会带 warning）。

## 停止

```bat
script\windows\start-windows.bat -Stop
```

这只结束路径匹配本仓库 `build-win\Release` 产物的进程，不会按进程名结束其它目录里的同名程序。停止本地推理用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1 -Stop
```
