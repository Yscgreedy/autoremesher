@echo off
rem Builds the same benchmark from HEAD sources, so a change can be compared
rem against the untouched pipeline on the same machine and compiler.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
if not exist build\pristine mkdir build\pristine
if not exist build\pristine\thirdparty\tbb\include git archive HEAD | tar -x -C build\pristine
xcopy /E /I /Y bench build\pristine\bench >nul
cmake -S build/pristine/bench -B build/pristine/b -G Ninja -DCMAKE_BUILD_TYPE=Release "-DTBB_LIB=%~dp0..\build\tbb\tbb_static.lib"
if errorlevel 1 exit /b 1
cmake --build build/pristine/b --parallel 16
