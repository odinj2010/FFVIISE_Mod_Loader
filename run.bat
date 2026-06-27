@echo off
cls
echo ======================================================
echo             FFVIISE Mod Loader CLI Tool
echo ======================================================
echo.
echo  [1] Compile mod loader (d3d11.dll)
echo  [2] Push changes to GitHub
echo  [3] Exit
echo.
echo ======================================================
set /p choice="Enter your choice (1-3): "

if "%choice%"=="1" goto :configure_msvc
if "%choice%"=="2" goto :git_push
if "%choice%"=="3" exit /b 0
echo [ERROR] Invalid choice.
pause
exit /b 1

:configure_msvc
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
echo [INFO] Compiling d3d11.dll with MSVC...
cl.exe /LD /O2 d3d11_proxy.cpp hooks.cpp minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde32.c minhook/src/hde/hde64.c /Iminhook/include /link /out:d3d11.dll user32.lib kernel32.lib gdi32.lib d3d11.lib d2d1.lib dwrite.lib

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Compilation completed successfully! d3d11.dll is ready.
) else (
    echo [ERROR] Compilation failed.
)
pause
exit /b %ERRORLEVEL%

:git_push
echo ======================================================
echo               Push changes to GitHub
echo ======================================================
echo.
set /p commit_msg="Enter commit message (or press Enter for default 'Update mod loader'): "
if "%commit_msg%"=="" set commit_msg=Update mod loader

echo.
echo [INFO] Running 'git add .'
git add .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] git add failed.
    pause
    exit /b 1
)

echo [INFO] Running 'git commit -m "%commit_msg%"'
git commit -m "%commit_msg%"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] git commit failed (perhaps no changes to commit?).
    pause
    exit /b 1
)

echo [INFO] Running 'git push'
git push
if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Successfully pushed changes to GitHub!
) else (
    echo [ERROR] git push failed.
)
pause
exit /b %ERRORLEVEL%
