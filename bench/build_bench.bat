@echo off
rem Builds the headless benchmark (no Qt needed) into build\bench\arbench.exe.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S bench -B build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build build/bench --parallel 16
