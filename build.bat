@echo off
setlocal
rem Cross-platform build (Windows): CMake + zig c++ (clang) as the compiler.
rem Requires cmake and ninja on PATH (e.g. "pip install cmake ninja").
rem Zig is located inside the uv toolchain; fall back to a plain "zig" on PATH.
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake not found on PATH. Install with: pip install cmake ninja
    exit /b 1
)
where ninja >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Ninja not found on PATH. Install with: pip install cmake ninja
    exit /b 1
)

rem Generate the compiler wrapper the CMake cache points at. The file itself
rem stays pure ASCII (uses %APPDATA% at runtime), which matters when the user
rem profile name contains non-ASCII characters.
if not exist build mkdir build
> "%~dp0build\zig_c++.bat" echo @echo off
>>"%~dp0build\zig_c++.bat" echo "%%APPDATA%%\uv\tools\ziglang\Lib\site-packages\ziglang\zig.exe" c++ %%

cmake -B build -G Ninja -DCMAKE_CXX_COMPILER="%~dp0build\zig_c++.bat"
cmake --build build
