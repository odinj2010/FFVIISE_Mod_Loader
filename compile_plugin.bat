@echo off

:: Check if cl.exe is already configured in the environment and is x64
where cl.exe >nul 2>nul
if %ERRORLEVEL% equ 0 (
    cl 2>&1 | findstr /i "x64" >nul
    if %ERRORLEVEL% equ 0 (
        echo [INFO] x64 Native Tools environment is already loaded.
        goto :compile
    )
)

echo [INFO] Detecting Visual Studio installation...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio Installer vswhere.exe not found.
    echo Please make sure Visual Studio is installed.
    pause
    exit /b 1
)

:: Find the latest installation of Visual Studio with C++ tools
set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)

if "%VS_PATH%"=="" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
        set "VS_PATH=%%i"
    )
)

if "%VS_PATH%"=="" (
    echo [ERROR] No Visual Studio installation found.
    pause
    exit /b 1
)

set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
set "VCVARS_ARGS="
if not exist "%VCVARS%" (
    set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat"
    set "VCVARS_ARGS=x64"
)

if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat or vcvarsall.bat not found in "%VS_PATH%".
    pause
    exit /b 1
)

echo [INFO] Found Visual Studio at: %VS_PATH%
echo [INFO] Configuring environment using: "%VCVARS%" %VCVARS_ARGS%
call "%VCVARS%" %VCVARS_ARGS%

:compile
echo [INFO] Compiling battle_overlay.dll via Python script...
python compile_plugin.py

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Plugin compilation completed successfully!
) else (
    echo [ERROR] Plugin compilation failed.
)

pause
