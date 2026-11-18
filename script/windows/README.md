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

## 停止

```bat
script\windows\start-windows.bat -Stop
```

这只结束路径匹配本仓库 `build-win\Release` 产物的进程，不会按进程名结束其它目录里的同名程序。
