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
3. 使用 profile `offline_demo`，监听 `127.0.0.1:8765`。这个 profile 不需要 API key。
4. 若 `%USERPROFILE%\.thin_agent\profiles\<profile>\config\chat_policy.json` 还不存在，从仓库复制一份。已有文件不会覆盖。
5. 后端若已在该端口健康，则不再启动第二个进程。
6. 健康检查通过后打开 `zing_agent`。界面默认连接 `ws://127.0.0.1:8765/ws`。

健康检查地址是 `http://127.0.0.1:8765/health`。日志在 `%USERPROFILE%\.thin_agent\logs\agent_svc.log`。15 秒内没有就绪时，脚本会打印日志末尾并退出。

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
| `-AgentProfile` | `offline_demo` | `demo.model.yaml` 里的 profile。`-Profile` 是同一参数的别名。 |
| `-Port` | `8765` | 监听端口。 |
| `-BindHost` | `127.0.0.1` | 监听地址。 |
| `-NoGui` | 关 | 只启动后端，不打开 `zing_agent`。 |
| `-Stop` | 关 | 结束本仓库编出来的 `thin_agent.exe` 和 `zing_agent.exe`。 |

云端 profile（例如 `deepseek_fast_demo`）需要对应的 API key 环境变量。密钥放在配置或环境里，不要写进这个脚本。

## 进程内 GGUF（可选）

Windows 可把 `THIN_AGENT_WITH_LLAMA_CPP=ON` 编进 `thin_agent`，用 `GgufModel` 直接加载 `models/gguf/*.gguf`，不必外挂 `llama-server`（`lmstudio_demo` 通道仍可走外挂）。

依赖（本机已有时可直接编）：

- 预编译 DLL：`C:\deps-thin-agent\llama.cpp\`（`bin\*.dll` + `lib\llama.lib` / `ggml.lib`）
- 匹配版本头文件：`C:\deps-thin-agent\llama.cpp-src\`（commit 与 DLL 一致）

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S backends\thin_agent -B backends\thin_agent\build-win -DTHIN_AGENT_WITH_LLAMA_CPP=ON
& $cmake --build backends\thin_agent\build-win --config Release --target thin_agent
```

离线 profile 下 HybridRouter 会懒加载已注册的 GGUF（优先 Aurum，其次 Qwen0.5B）。导入库若缺失，可在 VS 开发者命令行对 `bin\llama.dll` / `ggml.dll` 用 `dumpbin /exports` + `lib /def` 生成到 `lib\`。

## 模型通道热切换

启动后不必重启 `thin_agent` 也能换路由：`zing_agent` 侧栏「模型通道」下拉会发 `list_profiles` / `switch_profile`。只热替换进程内模型配置，**不**迁移会话/记忆目录（数据目录仍跟启动时的 `-AgentProfile`）。

常见选项：

| 展示名（约） | profile | 说明 |
| --- | --- | --- |
| 离线规则 | `offline_demo` | 无外网也能答，默认启动档 |
| 本地 Aurum（lmstudio） | `lmstudio_demo` | 走本机 OpenAI 兼容端点 `http://127.0.0.1:1234/v1` |
| 智谱 / DeepSeek 等 | `zai_*` / `deepseek_*` | 需对应 `api_key_env`（如 `GLM_API_KEY`、`DEEPSEEK_API_KEY`） |

切到本地通道时若端点不通，UI 会提示先启动 `llama-server`。

### 本地 llama-server（Aurum）

默认模型路径：`backends\thin_agent\models\gguf\aurum-brain-q4_k_m.gguf`（大文件通常不进 git）。二进制优先找 `C:\deps-thin-agent\llama.cpp\bin\llama-server.exe`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1
```

常用参数：`-Model <gguf路径>`、`-Port 1234`、`-GpuLayers 99`、`-LlamaServer <exe>`、`-Stop`。

手动等价命令示例：

```powershell
& C:\deps-thin-agent\llama.cpp\bin\llama-server.exe `
  -m backends\thin_agent\models\gguf\aurum-brain-q4_k_m.gguf `
  --host 127.0.0.1 --port 1234 -c 4096 -ngl 99
```

就绪后在侧栏选「本地 Aurum」即可；健康检查：`http://127.0.0.1:1234/v1/models`。

## 停止

```bat
script\windows\start-windows.bat -Stop
```

这只结束路径匹配本仓库 `build-win\Release` 产物的进程，不会按进程名结束其它目录里的同名程序。停止本地推理用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File script\windows\start-llama-server.ps1 -Stop
```
