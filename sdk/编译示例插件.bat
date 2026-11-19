@echo off
rem build a plugin dll (must use python312/mingw g++ as configured below)
cd /d "%~dp0"
set "GXX=D:\Desktop\code\mingw64\bin\g++.exe"
"%GXX%" -std=c++17 -O2 -shared -DPPT_PLUGIN_EXPORTS hello_page.cpp -o hello_page.dll -static-libgcc -static-libstdc++
if not exist hello_page.dll goto fail
copy /y hello_page.dll "..\build\plugins\hello_page.dll" >nul
echo.
echo BUILD OK - hello_page.dll copied to ..\build\plugins
pause
exit /b
:fail
echo BUILD FAILED
pause
