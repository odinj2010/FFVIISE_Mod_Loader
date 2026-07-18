@echo off
:main_menu
cls
echo ======================================================
echo             FFVIISE Mod Loader CLI Tool
echo ======================================================
echo.
echo  [1] Compile Components
echo  [2] Switch Git Branch
echo  [3] Push changes to GitHub
echo  [4] Exit
echo.
echo ======================================================
set /p choice="Enter your choice (1-4): "

if "%choice%"=="1" goto :compile_menu
if "%choice%"=="2" goto :switch_branch
if "%choice%"=="3" goto :git_push
if "%choice%"=="4" exit /b 0
echo [ERROR] Invalid choice.
pause
goto :main_menu


:compile_menu
cls
echo ======================================================
echo                 Compilation Options
echo ======================================================
echo.
echo  [1] Compile Mod Loader (d3d11.dll)
echo  [2] Compile Battle Overlay Plugin (battle_overlay.dll)
echo  [3] Compile Both/All
echo  [4] Back to main menu
echo.
echo ======================================================
set /p comp_choice="Enter your choice (1-4): "

if "%comp_choice%"=="1" goto :compile_loader
if "%comp_choice%"=="2" goto :compile_plugin
if "%comp_choice%"=="3" goto :compile_both
if "%comp_choice%"=="4" goto :main_menu
echo [ERROR] Invalid choice.
pause
goto :compile_menu


:compile_loader
call :configure_msvc
if %ERRORLEVEL% neq 0 goto :compile_menu

echo [INFO] Compiling resources (resources.rc)...
rc.exe resources.rc
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Resource compilation failed.
    pause
    goto :compile_menu
)

echo [INFO] Compiling d3d11.dll with MSVC...
cl.exe /LD /O2 d3d11_proxy.cpp hooks.cpp minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde32.c minhook/src/hde/hde64.c resources.res /Iminhook/include /link /out:d3d11.dll user32.lib kernel32.lib gdi32.lib d3d11.lib d2d1.lib dwrite.lib

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Mod Loader compilation completed successfully! d3d11.dll is ready.
) else (
    echo [ERROR] Compilation failed.
)
pause
goto :compile_menu


:compile_plugin
call :configure_msvc
if %ERRORLEVEL% neq 0 goto :compile_menu

echo [INFO] Compiling battle_overlay.dll via Python script...
python compile_plugin.py

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Plugin compilation completed successfully!
) else (
    echo [ERROR] Plugin compilation failed.
)
pause
goto :compile_menu


:compile_both
call :configure_msvc
if %ERRORLEVEL% neq 0 goto :compile_menu

echo [INFO] Compiling resources (resources.rc)...
rc.exe resources.rc
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Resource compilation failed.
    pause
    goto :compile_menu
)

echo [INFO] Compiling d3d11.dll with MSVC...
cl.exe /LD /O2 d3d11_proxy.cpp hooks.cpp minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde32.c minhook/src/hde/hde64.c resources.res /Iminhook/include /link /out:d3d11.dll user32.lib kernel32.lib gdi32.lib d3d11.lib d2d1.lib dwrite.lib
set loader_err=%ERRORLEVEL%

echo [INFO] Compiling battle_overlay.dll via Python script...
python compile_plugin.py
set plugin_err=%ERRORLEVEL%

echo.
echo ======================================================
if %loader_err% equ 0 (
    echo [SUCCESS] Mod Loader (d3d11.dll) compiled successfully!
) else (
    echo [ERROR] Mod Loader (d3d11.dll) compilation failed.
)
if %plugin_err% equ 0 (
    echo [SUCCESS] Battle Overlay Plugin compiled successfully!
) else (
    echo [ERROR] Battle Overlay Plugin compilation failed.
)
echo ======================================================
pause
goto :compile_menu


:switch_branch
cls
echo ======================================================
echo                 Switch Git Branch
echo ======================================================
echo.
:: Get current branch name
for /f "tokens=*" %%i in ('git rev-parse --abbrev-ref HEAD') do set "current_branch=%%i"
echo Current branch: %current_branch%
echo.
echo  [1] Switch to main branch
echo  [2] Switch to nightly branch
echo  [3] Back to main menu
echo.
echo ======================================================
set /p br_choice="Enter your choice (1-3): "
set "target_branch="
if "%br_choice%"=="1" set "target_branch=main"
if "%br_choice%"=="2" set "target_branch=nightly"
if "%br_choice%"=="3" goto :main_menu

if "%target_branch%"=="" (
    echo [ERROR] Invalid choice.
    pause
    goto :switch_branch
)

if /i "%current_branch%"=="%target_branch%" (
    echo [INFO] Already on branch %target_branch%.
    pause
    goto :switch_branch
)

echo [INFO] Switching to branch %target_branch%...
call git checkout %target_branch%
if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Switched to branch %target_branch%!
) else (
    echo [ERROR] Failed to switch branch.
)
pause
goto :switch_branch


:git_push
cls
echo ======================================================
echo               Push changes to GitHub
echo ======================================================
echo.
echo  [1] main branch
echo  [2] nightly branch
echo  [3] Back to main menu
echo.
echo ======================================================
set /p branch_choice="Select which branch to update (1-3): "
set "target_branch="
if "%branch_choice%"=="1" set "target_branch=main"
if "%branch_choice%"=="2" set "target_branch=nightly"
if "%branch_choice%"=="3" goto :main_menu
if "%target_branch%"=="" (
    echo [ERROR] Invalid branch choice.
    pause
    goto :git_push
)

:: Get current branch name
for /f "tokens=*" %%i in ('git rev-parse --abbrev-ref HEAD') do set "current_branch=%%i"

:: Ask for commit message
set "commit_msg="
set /p commit_msg="Enter commit message (or press Enter for default 'Update mod loader'): "
if "%commit_msg%"=="" set commit_msg=Update mod loader

:: If current branch is different from target branch, checkout target branch
if /i not "%current_branch%"=="%target_branch%" (
    echo [INFO] Switching from %current_branch% to %target_branch%...
    call git checkout %target_branch%
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Failed to switch to branch %target_branch%.
        pause
        goto :main_menu
    )
)

echo.
echo [INFO] Running 'git add .'
call git add .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] git add failed.
    pause
    goto :main_menu
)

echo [INFO] Running 'git commit -m "%commit_msg%"'
call git commit -m "%commit_msg%"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] git commit failed. Perhaps there are no changes to commit?
    pause
    goto :main_menu
)

echo [INFO] Running 'git push origin %target_branch%'
call git push origin %target_branch%
if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] Successfully pushed changes to origin/%target_branch%!
) else (
    echo [ERROR] git push failed.
)
pause
goto :main_menu


:configure_msvc
:: Check if cl.exe is already configured in the environment and is x64
where cl.exe >nul 2>nul
if %ERRORLEVEL% equ 0 (
    cl 2>&1 | findstr /i "x64" >nul
    if %ERRORLEVEL% equ 0 (
        exit /b 0
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
exit /b 0
