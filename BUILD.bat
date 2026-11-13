@echo off
rem Builds build\Release\DemoTools2.dll. Needs Visual Studio 2022 (Desktop C++) and CMake.
cd /d "%~dp0"
cmake -S . -B build -A x64 || goto :fail
cmake --build build --config Release || goto :fail
echo.
echo Done: %~dp0build\Release\DemoTools2.dll
pause
exit /b 0
:fail
echo Build failed.
pause
exit /b 1
