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

echo Compiling overlay...
cl /EHsc /O2 /Fe:overlay.exe src\main.cpp user32.lib gdi32.lib dwmapi.lib

if not exist build\overlay mkdir build\overlay
move /y overlay.exe build\overlay\
move /y *.obj build\overlay\ 2>nul

endlocal
