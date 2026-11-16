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

echo Loading MSVC environment from "%VS_PATH%...
call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

echo Building project...
msbuild fps-overlay.sln /p:Configuration=Release /p:Platform=x64
if errorlevel 1 (
  REM Fallback if no .sln file, build via MSBuild on project file directly
  msbuild src\fps-overlay.vcxproj /p:Configuration=Release /p:Platform=x64
)

endlocal
