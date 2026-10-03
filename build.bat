@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"
if not exist "bin" mkdir "bin"

echo [*] build douyin_lite.dll ...
g++ -shared -O2 -s -I "minhook/include" "src/douyin_lite.cpp" "minhook/src/buffer.c" "minhook/src/hook.c" "minhook/src/trampoline.c" "minhook/src/hde/hde64.c" "minhook/src/hde/hde32.c" -o "bin/douyin_lite.dll" -lpsapi -lshlwapi -static

if %errorlevel% neq 0 (
    echo [!] failed to build douyin_lite.dll
    pause
    exit /b %errorlevel%
)

echo [*] build douyin_lite.exe ...
g++ -mwindows -O2 -s -municode "src/launcher.cpp" -o "bin/douyin_lite.exe" -lshlwapi -lshell32 -static

if %errorlevel% neq 0 (
    echo [!] failed to build douyin_lite.exe
    pause
    exit /b %errorlevel%
)

echo [+] build succeeded, output in bin\
pause
