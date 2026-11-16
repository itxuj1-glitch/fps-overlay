
@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ==========================================
echo FPS OVERLAY - MSVC BUILD
echo ==========================================

echo Locating Visual Studio...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo ERROR: Visual Studio installation not found.
    exit /b 1
)

echo Loading MSVC environment...
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

if errorlevel 1 exit /b 1

echo Downloading ImGui dependencies...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0download-imgui.ps1"

if errorlevel 1 (
    echo ERROR: ImGui download failed.
    exit /b 1
)

echo Creating hardware wrapper...

powershell -NoProfile -ExecutionPolicy Bypass -Command "$c = '#pragma once' + [Environment]::NewLine + '#include <string>' + [Environment]::NewLine + '#include <map>' + [Environment]::NewLine + 'namespace LHWM {' + [Environment]::NewLine + 'inline double GetSensorValue(const std::string& name) { return 0.0; }' + [Environment]::NewLine + 'inline std::map<std::string, double> GetHardwareSensorMap() { return {}; }' + [Environment]::NewLine + '}'; Set-Content 'src/lhwm-cpp-wrapper.h' $c -Encoding ASCII"

if errorlevel 1 exit /b 1

if not exist "build\overlay" mkdir "build\overlay"

echo Compiling application...

cl.exe /nologo /std:c++17 /EHsc /O2 /MD /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /I src /I src\backends src\main.cpp src\imgui.cpp src\imgui_draw.cpp src\imgui_tables.cpp src\imgui_widgets.cpp src\backends\imgui_impl_win32.cpp src\backends\imgui_impl_dx11.cpp /link d3d11.lib dxgi.lib dwmapi.lib user32.lib gdi32.lib /OUT:build\overlay\overlay.exe

if errorlevel 1 (
    echo BUILD FAILED.
    exit /b 1
)

echo ==========================================
echo BUILD SUCCESSFUL!
echo ==========================================

exit /b 0
