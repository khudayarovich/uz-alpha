@echo off
rem UzAlpha build: keyboard layout DLLs, text service DLLs, the app and the tests.
rem   build.cmd          build everything into build\
rem   build.cmd test     build, then run the automated tests
rem Needs: Visual Studio Build Tools (C++ x64/x86 + Windows SDK), Node.js.
setlocal enableextensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Build Tools not found.
  exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: no Visual C++ toolset found.
  exit /b 1
)
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
rem vcvarsall calls vswhere itself and expects it on PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"

rem --- compiler / linker settings ------------------------------------------
rem Keyboard layout: a data-only DLL, no CRT, everything merged into .data.
set "KBD_CL=/nologo /c /TC /utf-8 /W3 /Zp8 /Gy /GF /GS- /GR- /EHs-c- /Zl /O1"
set "KBD_LINK=/nologo /dll /noentry /subsystem:native /def:src\kbd\kbduzalp.def /merge:.edata=.data /merge:.rdata=.data /merge:.text=.data /merge:.bss=.data /section:.data,re /ignore:4078,4070,4254 /opt:ref /opt:icf /release /incremental:no /nodefaultlib"
rem Text service: loaded into other programs, so static CRT and no exceptions.
set "TIP_CL=/nologo /c /std:c++17 /utf-8 /W4 /O1 /MT /GR- /EHs-c- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
set "TIP_SRC=src\tip\dllmain.cpp src\tip\textservice.cpp src\tip\keys.cpp src\tip\displayattr.cpp src\engine\engine.cpp"
set "TIP_OBJ=dllmain.obj textservice.obj keys.obj displayattr.obj engine.obj"
set "TIP_LIBS=kernel32.lib user32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib"
set "APP_CL=/nologo /c /std:c++17 /utf-8 /W4 /O1 /MT /EHsc /DUNICODE /D_UNICODE /DNOMINMAX"
set "APP_SRC=src\app\main.cpp src\app\install.cpp src\app\inputctl.cpp src\app\convert.cpp src\app\update.cpp src\engine\translit.cpp"
set "APP_OBJ=main.obj install.obj inputctl.obj convert.obj update.obj translit.obj"
set "APP_LIBS=kernel32.lib user32.lib gdi32.lib gdiplus.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib comctl32.lib winhttp.lib bcrypt.lib"
set "TEST_CL=/nologo /std:c++17 /utf-8 /W3 /EHsc /MT /DUNICODE /D_UNICODE"

echo === generated sources
node tools\klc2c.mjs third_party\kiritish\new-uzb.klc src\kbd\kbduzalp.c || goto :fail
node tools\gen-mac-layouts.mjs third_party\kiritish\ozbekcha-yangi-alifbo.keylayout mac\UzAlpha.bundle\Contents\Resources || goto :fail
if not exist assets\uzalpha.ico (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1 assets\uzalpha.ico || goto :fail
)
for %%d in (kbd\x64 kbd\wow64 tip\x64 tip\x86 tip\x64-trace tests\x64 tests\x86 app) do if not exist build\%%d mkdir build\%%d

call :native x64 || goto :fail
call :native x86 || goto :fail
if exist src\app\main.cpp ( call :app || goto :fail )
if /i "%~1"=="test" ( call :runtests || goto :fail )

echo.
echo BUILD OK
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1

rem ---------------------------------------------------------------------------
rem :native x64|x86 - layout DLL, text service DLL and tests for one architecture
:native
setlocal
set "A=%~1"
if "%A%"=="x64" (
  set "VCARCH=x64"
  set "KBDDIR=build\kbd\x64"
  set "KBDEXTRA="
  set "TESTEXTRA="
) else (
  set "VCARCH=x64_x86"
  set "KBDDIR=build\kbd\wow64"
  set "KBDEXTRA=/Gz /DBUILD_WOW6432"
  set "TESTEXTRA=/DBUILD_WOW6432"
)
set "TIPDIR=build\tip\%A%"
set "TESTDIR=build\tests\%A%"
call "%VCVARS%" %VCARCH% >nul
if errorlevel 1 exit /b 1

echo === %A%: keyboard layout
rc /nologo /fo %KBDDIR%\kbduzalp.res src\kbd\kbduzalp.rc || exit /b 1
cl %KBD_CL% %KBDEXTRA% /Fo%KBDDIR%\kbduzalp.obj src\kbd\kbduzalp.c || exit /b 1
link %KBD_LINK% /machine:%A% /out:%KBDDIR%\kbduzalp.dll %KBDDIR%\kbduzalp.obj %KBDDIR%\kbduzalp.res || exit /b 1

echo === %A%: text service
rc /nologo /fo %TIPDIR%\tip.res src\tip\tip.rc || exit /b 1
cl %TIP_CL% /Fo%TIPDIR%\ %TIP_SRC% || exit /b 1
pushd %TIPDIR%
link /nologo /dll /def:..\..\..\src\tip\tip.def /opt:ref /opt:icf /incremental:no /out:UzAlphaTip.dll %TIP_OBJ% tip.res %TIP_LIBS% || ( popd & exit /b 1 )
popd

if "%A%"=="x64" (
  echo === x64: text service with tracing ^(tests only^)
  cl %TIP_CL% /DUZA_TRACE /Fobuild\tip\x64-trace\ %TIP_SRC% || exit /b 1
  pushd build\tip\x64-trace
  link /nologo /dll /def:..\..\..\src\tip\tip.def /opt:ref /opt:icf /incremental:no /out:UzAlphaTip.dll %TIP_OBJ% ..\x64\tip.res %TIP_LIBS% || ( popd & exit /b 1 )
  popd
)

echo === %A%: tests
if "%A%"=="x64" (
  cl %TEST_CL% /Fo%TESTDIR%\ /Fe%TESTDIR%\engine_test.exe tests\engine_test.cpp src\engine\engine.cpp src\engine\translit.cpp || exit /b 1
  cl %TEST_CL% /Fo%TESTDIR%\ /Fe%TESTDIR%\tsf_diag.exe tests\tsf_diag.cpp /link /subsystem:windows user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib || exit /b 1
)
cl %TEST_CL% %TESTEXTRA% /Fo%TESTDIR%\ /Fe%TESTDIR%\kbd_test.exe tests\kbd_test.cpp || exit /b 1
cl %TEST_CL% /Fo%TESTDIR%\ /Fe%TESTDIR%\tip_harness.exe tests\tip_harness.cpp /link /subsystem:windows user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib || exit /b 1
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem :app - UzAlpha.exe (x64) with the four DLLs embedded as resources
:app
setlocal
call "%VCVARS%" x64 >nul
if errorlevel 1 exit /b 1
echo === app
rc /nologo /fo build\app\app.res src\app\app.rc || exit /b 1
cl %APP_CL% /Fobuild\app\ %APP_SRC% || exit /b 1
pushd build\app
link /nologo /subsystem:windows /opt:ref /opt:icf /incremental:no /manifest:no /out:UzAlpha.exe %APP_OBJ% app.res %APP_LIBS% || ( popd & exit /b 1 )
popd
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
:runtests
echo.
echo === tests
build\tests\x64\engine_test.exe || exit /b 1
if exist "%SystemRoot%\System32\new-uzb.dll" (
  echo --- layout vs kiritish.uz ^(x64^)
  build\tests\x64\kbd_test.exe build\kbd\x64\kbduzalp.dll "%SystemRoot%\System32\new-uzb.dll" || exit /b 1
  echo --- layout vs kiritish.uz ^(32-bit^)
  build\tests\x86\kbd_test.exe build\kbd\wow64\kbduzalp.dll "%SystemRoot%\SysWOW64\new-uzb.dll" || exit /b 1
) else (
  echo --- kiritish.uz layout is not installed here: layout comparison skipped
)
echo --- text service, x64
call :harness x64 us tip_harness.log || exit /b 1
echo --- text service, x64, on top of the kiritish.uz layout
call :harness x64 uz tip_harness_uz.log || exit /b 1
echo --- text service, x64, on top of a Russian layout
call :harness x64 ru tip_harness_ru.log || exit /b 1
echo --- text service, 32-bit
call :harness x86 us tip_harness.log || exit /b 1
exit /b 0

rem :harness x64|x86 <base layout> <log name> - one run of the text-service harness.
rem Exit code 1 = a typing check failed; 2 = the harness could not start TSF,
rem which is tolerated on CI machines that have no interactive desktop.
:harness
start "" /wait build\tests\%1\tip_harness.exe build\tip\%1\UzAlphaTip.dll build\tests\%1\%3 %2
set "RC=%errorlevel%"
if "%RC%"=="0" (
  findstr /c:"checks," /c:"skipped" build\tests\%1\%3
  exit /b 0
)
type build\tests\%1\%3
if "%RC%"=="2" if defined CI (
  echo SKIPPED: text input services are not available in this session
  exit /b 0
)
exit /b 1
