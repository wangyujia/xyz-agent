@echo off
rem 构建本机 thin_agent 与 zing_agent。参数原样传给同目录的 build-windows.ps1。
rem   script\windows\build-windows.bat
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*
set EXITCODE=%ERRORLEVEL%
if not "%EXITCODE%"=="0" pause
exit /b %EXITCODE%
