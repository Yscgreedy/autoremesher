@echo off
rem Builds the vendored TBB 2017 as a static library into build\tbb.
rem Run from anywhere: the script locates the repository root itself.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S thirdparty/tbb -B build/tbb -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" -DTBB_BUILD_SHARED=OFF -DTBB_BUILD_TBBMALLOC=OFF -DTBB_BUILD_TBBMALLOC_PROXY=OFF -DTBB_BUILD_TESTS=OFF
if errorlevel 1 exit /b 1
cmake --build build/tbb --parallel 16
