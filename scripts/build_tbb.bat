@echo off
rem Build the vendored TBB 2017 into build\tbb as a static library.
rem
rem This is the script scripts\build_cli.bat expects; it used to live at
rem build\build_tbb.bat, which never made it into the repository because the
rem whole build\ directory is ignored.  No vcvars call is needed: CMake selects
rem the installed Visual Studio generator itself, which also keeps the script
rem usable on a CI runner whose VS version is not 18.
setlocal
cd /d "%~dp0.."

if not exist build mkdir build

cmake -S thirdparty/tbb -B build\tbb ^
    -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" ^
    -DTBB_BUILD_SHARED=OFF ^
    -DTBB_BUILD_TBBMALLOC=OFF ^
    -DTBB_BUILD_TBBMALLOC_PROXY=OFF ^
    -DTBB_BUILD_TESTS=OFF || exit /b 1

cmake --build build\tbb --config Release --parallel || exit /b 1

rem A multi-config generator drops the library into build\tbb\Release.
if not exist build\tbb\tbb_static.lib (
    if exist build\tbb\Release\tbb_static.lib (
        copy /y build\tbb\Release\tbb_static.lib build\tbb\tbb_static.lib >nul
    )
)

if not exist build\tbb\tbb_static.lib (
    echo [build_tbb] failed: build\tbb\tbb_static.lib was not produced
    exit /b 1
)

echo [build_tbb] done: %CD%\build\tbb\tbb_static.lib
endlocal
