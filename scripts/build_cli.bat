@echo off
rem Build the Qt-free C++ core and the headless CLI into build\cli\dist\autoremesher.exe.
rem Uses CMake's default generator (Visual Studio on Windows), so no vcvars is needed.
setlocal
cd /d "%~dp0.."

if not exist build\tbb\tbb_static.lib (
    echo [build] static TBB missing, building it once...
    call build\build_tbb.bat || exit /b 1
)

cmake -S . -B build\cli -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build\cli --config Release --parallel || exit /b 1

echo.
echo [build] done: %CD%\build\cli\dist\autoremesher.exe
endlocal
