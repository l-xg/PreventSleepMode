@echo off
rem Build PreventSleepMode.exe with MSVC (Visual Studio).
rem Usage: double-click build.bat, or run from cmd / Git Bash: cmd //c build.bat
setlocal
cd /d "%~dp0"

rem Locate Visual Studio via vswhere and set up the build environment
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe not found. Please install Visual Studio with C++ workload,
    echo         or run this script from the "x64 Native Tools Command Prompt".
    exit /b 1
)

rc /nologo /fo app.res resources\app.rc
if errorlevel 1 (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

cl /nologo /utf-8 /W4 /O2 /EHsc /DUNICODE /D_UNICODE src\main.cpp app.res ^
   /Fe:PreventSleepMode.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

del /q main.obj app.res >nul 2>nul
echo.
echo Build OK: PreventSleepMode.exe
endlocal
