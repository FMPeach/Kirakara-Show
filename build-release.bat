@echo off
setlocal
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%
cmake --build build
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build --output-on-failure
if errorlevel 1 exit /b %errorlevel%
echo.
echo Build complete: build\bin\kirakara_show_demo.exe
endlocal
