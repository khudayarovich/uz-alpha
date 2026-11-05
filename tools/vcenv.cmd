@echo off
rem Run one command inside a Visual C++ build environment (developer helper;
rem build.cmd does the real build).
rem   tools\vcenv.cmd x64     cl /nologo ...
rem   tools\vcenv.cmd x64_x86 cl /nologo ...
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found - install Visual Studio Build Tools with the C++ workload. 1>&2
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo No Visual C++ toolset found. 1>&2
  exit /b 1
)
rem vcvarsall calls vswhere itself and expects it on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
rem Everything after the first argument, untouched ("=" and "," would split %2...).
set "REST=%*"
call set "REST=%%REST:*%~1=%%"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %~1 >nul
if errorlevel 1 exit /b 1
%REST%
exit /b %errorlevel%
