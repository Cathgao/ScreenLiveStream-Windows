@echo off
setlocal EnableDelayedExpansion

taskkill /F /IM ScreenLiveStreamWindows.exe >nul 2>&1

set VCVARS=
if exist "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo [ERROR] Visual Studio x64 Build Environment not found!
    exit /b 1
)

call "!VCVARS!" >nul

if not exist build\build.ninja (
    echo [CMake] Configuring Release build with Ninja...
    cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
)

echo [Build] Compiling and Linking with LTCG/LTO...
cmake --build build --config Release
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed!
    exit /b 1
)

echo.
echo ========================================================
echo [SUCCESS] Build completed successfully!
echo Binary: build\ScreenLiveStreamWindows.exe
echo ========================================================
endlocal
