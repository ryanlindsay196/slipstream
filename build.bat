@echo off
rem Usage: build.bat [release|debug] [x64|x86]
rem Output: build\<arch>-<config>\slipstream.exe
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release
set ARCH=%2
if "%ARCH%"=="" set ARCH=x64

rem No if-block here: the ")" in %ProgramFiles(x86)% would close it.
if defined VCINSTALLDIR goto :have_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul || exit /b 1
:have_vs

set OUT=build\%ARCH%-%CONFIG%
if not exist %OUT% mkdir %OUT%

rem No exceptions, no RTTI, C++20, warnings as errors. /fp:fast is safe: floats
rem only ever touch rendering, never the deterministic simulation.
set COMMON=/nologo /std:c++20 /W4 /WX /permissive- /EHs-c- /D_HAS_EXCEPTIONS=0 /D_CRT_SECURE_NO_WARNINGS /GR- /utf-8 /Isrc /Fo%OUT%\ /Fe%OUT%\slipstream.exe
if /i "%CONFIG%"=="debug" (
    set FLAGS=/Od /Zi /MTd /Fd%OUT%\
) else (
    set FLAGS=/O2 /Oi /GS- /fp:fast /MT /DNDEBUG
)

cl %COMMON% %FLAGS% src\*.cpp src\core\*.cpp src\platform\*.cpp src\render\*.cpp src\game\*.cpp src\net\*.cpp src\app\*.cpp /link /SUBSYSTEM:CONSOLE || exit /b 1
echo Built %OUT%\slipstream.exe
