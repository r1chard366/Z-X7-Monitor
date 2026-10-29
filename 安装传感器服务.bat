@echo off
REM 只需运行一次：右键 -> 以管理员身份运行
REM 安装 CPU 温度传感器服务（最高权限计划任务），之后主程序用普通权限即可。
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
cd /d "%~dp0"
schtasks /Create /TN ZX7ProSensor /TR "\"%~dp0sensors\LibreHardwareMonitor\ZX7SensorHost.exe\"" /SC ONCE /ST 00:00 /RL HIGHEST /F
if errorlevel 1 ( echo [x] create task failed & pause & exit /b 1 )
schtasks /Run /TN ZX7ProSensor >nul 2>&1
echo [ok] sensor service installed (task: ZX7ProSensor)
echo Now you can run ZX7Pro.exe without administrator rights.
pause