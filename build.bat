@echo off
setlocal enabledelayedexpansion
title Build FOXAssetBrowser (full: configure + deps + build)
cd /d "%~dp0"

:: One-time / full build: configures CMake (which lets vcpkg restore or build
:: Qt6 + zlib) and compiles everything. Day-to-day, use rebuild.bat instead.

where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    set "VSPATH="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH ( echo ERROR: Visual Studio 2022 C++ tools not found. & pause & exit /b 1 )
    call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not defined VCPKG_ROOT (
    if exist "C:\vcpkg\scripts\buildsystems\vcpkg.cmake" set "VCPKG_ROOT=C:\vcpkg"
)
if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT is not set and C:\vcpkg was not found. Run setup.bat first.
    pause & exit /b 1
)

echo Configuring (vcpkg restores cached Qt6 if the D4 browser was built on this machine)...
cmake --preset windows-msvc-release
if errorlevel 1 ( echo CONFIGURE FAILED & pause & exit /b 1 )

echo Building...
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmake --build --preset release 2>&1 | Tee-Object -FilePath '%~dp0build_log.txt'; exit $LASTEXITCODE"
set "RC=%errorlevel%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$e = Select-String -Path '%~dp0build_log.txt' -Pattern 'error C',': error','error LNK','fatal error','FAILED','ninja: build stopped','BUILD FAILED' | ForEach-Object { $_.Line } | Select-Object -First 40 ; $e; $e | Out-File -FilePath '%~dp0build_errors.txt' -Encoding utf8"
if not "%RC%"=="0" (
    echo BUILD FAILED - see build_log.txt / build_errors.txt
    pause & exit /b 1
)

echo Done. Launching...
call "%~dp0run.bat"
