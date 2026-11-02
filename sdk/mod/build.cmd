@echo off
rem ithaca mod one-shot build: build.cmd ^<source.cpp^> [output.dll]
rem No cmake needed - locates Visual Studio, then a single cl command emits the DLL
rem (nostos plugin side is header-only, zero link libs).
rem NOTE: this script is ASCII-only on purpose - cmd.exe parses batch files in the
rem legacy codepage, and UTF-8 Chinese bytes get corrupted there (product docs live
rem in README.txt / example sources, both read as UTF-8).
setlocal enabledelayedexpansion
if "%~1"=="" (
    echo Usage: build.cmd ^<source.cpp^> [output.dll]
    exit /b 1
)

rem Locate latest Visual Studio via vswhere (ships with the VS installer)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere not found - install Visual Studio with the C++ workload
    exit /b 1
)
set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if "!VSROOT!"=="" (
    echo No Visual Studio with C++ tools found
    exit /b 1
)
call "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo failed to load vcvars64
    exit /b 1
)

set "OUT=%~2"
if "!OUT!"=="" set "OUT=%~n1_plugin.dll"

cl /nologo /LD /std:c++20 /EHsc /utf-8 /permissive- /DNOSTOS_BUILDING_PLUGIN=1 /I"%~dp0include" "%~1" /Fe"!OUT!"
if errorlevel 1 (
    echo build failed
    exit /b 1
)
echo Build OK: !OUT!
echo Next: copy it into the mods\ folder next to ithaca_host.exe, then restart
echo the host (in-app Settings - Restart backend) to load it.
endlocal
