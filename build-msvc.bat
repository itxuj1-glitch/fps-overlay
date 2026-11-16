
@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo.
echo ==========================================
echo       FPS OVERLAY - MSVC BUILD
echo ==========================================
echo.

echo Locating Visual Studio...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio Installer not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo ERROR: Visual Studio installation not found.
    exit /b 1
)

echo Loading MSVC environment...

call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

if errorlevel 1 (
    echo ERROR: MSVC environment initialization failed.
    exit /b 1
)

echo.
echo Checking ImGui dependencies...

if not exist "src" mkdir "src"

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
"$ErrorActionPreference = 'Stop';" ^
"$base = 'https://raw.githubusercontent.com/ocornut/imgui/master';" ^
"$files = @('imgui.h','imgui_internal.h','imconfig.h','imstb_textedit.h','imstb_rectpack.h','imstb_truetype.h','imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp','backends/imgui_impl_win32.h','backends/imgui_impl_win32.cpp','backends/imgui_impl_dx11.h','backends/imgui_impl_dx11.cpp');" ^
"foreach ($f in $files) {" ^
"  $dest = Join-Path 'src' ($f -replace '/', '\');" ^
"  $dir = Split-Path $dest;" ^
"  if (!(Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null };" ^
"  if (!(Test-Path $dest)) { Write-Host ('Downloading ' + $f); Invoke-WebRequest ($base + '/' + $f) -OutFile $dest }" ^
"}"

if errorlevel 1 (
    echo ERROR: ImGui dependency download failed.
    exit /b 1
)

echo.
echo Creating hardware wrapper...

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
"$content = @'
#pragma once
#include <string>
#include <map>

namespace LHWM {
    inline double GetSensorValue(const std::string& name) {
        return 0.0;
    }

    inline std::map<std::string, double> GetHardwareSensorMap() {
        return {};
    }
}
'@; Set-Content -Path 'src/lhwm-cpp-wrapper.h' -Value $content -Encoding ASCII"

if errorlevel 1 (
    echo ERROR: Hardware wrapper creation failed.
    exit /b 1
)

echo.
echo Starting C++ compilation...

if not exist "build\overlay" mkdir "build\overlay"

cl.exe /nologo /std:c++17 /EHsc /O2 /MD /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /I src /I src\backends src\main.cpp src\imgui.cpp src\imgui_draw.cpp src\imgui_tables.cpp src\imgui_widgets.cpp src\backends\imgui_impl_win32.cpp src\backends\imgui_impl_dx11.cpp /link d3d11.lib dxgi.lib dwmapi.lib user32.lib gdi32.lib /OUT:build\overlay\overlay.exe

if errorlevel 1 (
    echo.
    echo ERROR: Compilation failed.
    exit /b 1
)

echo.
echo ==========================================
echo BUILD SUCCESSFUL!
echo Output: build\overlay\overlay.exe
echo ==========================================

exit /b 0
