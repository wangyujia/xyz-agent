@echo off
rem 双击启动本机后端和 zing_agent。参数原样传给同目录的 start-windows.ps1。
rem   script\windows\start-windows.bat
rem   script\windows\start-windows.bat -AgentProfile deepseek_fast_demo
rem   script\windows\start-windows.bat -NoGui
rem   script\windows\start-windows.bat -Stop
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0start-windows.ps1" %*
set EXITCODE=%ERRORLEVEL%
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
