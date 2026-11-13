@echo off
rem Build everything: the Qt-free CLI (C++/CMake) and the Go GUI.
setlocal
cd /d "%~dp0"

call scripts\build_cli.bat || exit /b 1

where go >nul 2>nul
if errorlevel 1 (
    echo [build] Go toolchain not found; skipping the GUI. Install Go 1.21+ to build it.
    exit /b 0
)

pushd gui
if not exist dist mkdir dist
go build -trimpath -ldflags "-s -w" -o dist\autoremesher-gui.exe .\cmd\autoremesher-gui || (popd & exit /b 1)
popd

rem Ship the CLI next to the GUI so the default discovery finds it without flags.
if exist build\cli\dist\autoremesher.exe copy /y build\cli\dist\autoremesher.exe gui\dist\autoremesher.exe >nul

echo.
echo [build] CLI : build\cli\dist\autoremesher.exe
echo [build] GUI : gui\dist\autoremesher-gui.exe
endlocal
