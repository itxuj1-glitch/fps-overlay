@echo off
setlocal

echo Locating Visual Studio...
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.X86.X64 -property installationPath`) do (
  set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
  echo ERROR: Visual Studio C++ build tools not found!
  exit /b 1
)

echo Loading MSVC environment...
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

echo Ensuring all ImGui files and backends exist...
if not exist "imgui" mkdir imgui
if not exist "imgui\imgui.h" (
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imgui.h' -OutFile 'imgui\imgui.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imgui_internal.h' -OutFile 'imgui\imgui_internal.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imconfig.h' -OutFile 'imgui\imconfig.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imstb_textedit.h' -OutFile 'imgui\imstb_textedit.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imstb_rectpack.h' -OutFile 'imgui\imstb_rectpack.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/imstb_truetype.h' -OutFile 'imgui\imstb_truetype.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/backends/imgui_impl_win32.h' -OutFile 'imgui\imgui_impl_win32.h'"
  powershell -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/ocornut/imgui/v1.90.1/backends/imgui_impl_dx11.h' -OutFile 'imgui\imgui_impl_dx11.h'"
)

echo Creating complete hardware wrapper stub...
(
echo #pragma once
echo #include <string^>
echo #include <vector^>
echo #include <map^>
echo namespace LHWM {
echo     inline float GetSensorValue(const std::string&, const std::string^) { return 0.0f; }
echo     inline std::map<std::string, std::vector<std::string>> GetHardwareSensorMap() { return {}; }
echo }
) > src\lhwm-cpp-wrapper.h

echo Compiling overlay with C++17...
cl /std:c++17 /EHsc /O2 /I"imgui" /I"src" /Fe:overlay.exe src\main.cpp user32.lib gdi32.lib dwmapi.lib

if not exist build\overlay mkdir build\overlay
move /y overlay.exe build\overlay\
move /y *.obj build\overlay\ 2>nul

endlocal
