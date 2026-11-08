@echo off
rem Build version.dll (Windows x64) with Zig. Install it with: pip install ziglang
cd /d "%~dp0"
if not exist dist mkdir dist
python -m ziglang cc -target x86_64-windows-gnu -O2 -shared -Wall -Wextra -o dist\version.dll src\version_proxy.c src\fxcore.c src\version.def -ladvapi32 -s || exit /b 1
del /q dist\*.lib dist\*.pdb 2>nul
echo Built dist\version.dll
