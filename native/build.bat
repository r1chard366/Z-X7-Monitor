@echo off
REM ============================================================
REM  Z-X7 PRO ??? ?? ?????MSVC??? CRT?? exe?
REM ============================================================
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo [x] ??? Visual Studio ????
    exit /b 1
)

if not exist build mkdir build

echo [1/2] ???? ...
rc /nologo /fo build\app.res res\app.rc
if errorlevel 1 exit /b 1

echo [2/2] ????? ...
cl /nologo /std:c++17 /O1 /Oi /Gy /Gw /GR- /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE ^
   src\main.cpp build\app.res ^
   /Fe:build\ZX7Pro.exe /Fo:build\ ^
   /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'"
if errorlevel 1 exit /b 1

echo [ok] build\ZX7Pro.exe