@echo off
setlocal enabledelayedexpansion
title FOXAssetBrowser - one-time setup, build, and run
cd /d "%~dp0"

echo ============================================================
echo   FOXAssetBrowser - one-time setup / recovery
echo   Checks tools, sets up vcpkg, and builds. If the D4 asset
echo   browser was built on this machine, the Qt6 dependencies
echo   restore from the vcpkg cache in minutes instead of hours.
echo   ^(For day-to-day work use rebuild.bat instead.^)
echo ============================================================
echo.
set /a MISSING=0

echo --- Checking required tools --------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
if defined VSPATH (
    echo [ OK ] Visual Studio C++ tools ^> !VSPATH!
    echo [ OK ] CMake + Ninja ^(bundled with the VS C++ workload^)
) else (
    echo [MISS] Visual Studio with "Desktop development with C++".
    echo        Install: https://visualstudio.microsoft.com/downloads/  ^(tick that workload^)
    set /a MISSING+=1
)
where git >nul 2>&1
if %errorlevel%==0 ( echo [ OK ] git ) else (
    echo [MISS] git   ^(winget install --id Git.Git -e^)
    set /a MISSING+=1
)

if not %MISSING%==0 (
    echo.
    echo   %MISSING% required tool^(s^) missing. Install the [MISS] items above,
    echo   open a NEW terminal, and run setup.bat again.
    pause & exit /b 1
)

echo.
echo --- vcpkg ---------------------------------------------------
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    echo [ OK ] vcpkg ^> %VCPKG_ROOT%
    goto vcpkg_ok
)
echo Setting up a standalone vcpkg at C:\vcpkg ...
if not exist "C:\vcpkg\.git"      git clone https://github.com/microsoft/vcpkg C:\vcpkg
if not exist "C:\vcpkg\vcpkg.exe" call C:\vcpkg\bootstrap-vcpkg.bat
if not exist "C:\vcpkg\vcpkg.exe" (
    echo   ERROR: vcpkg bootstrap failed ^(check internet / antivirus^).
    pause & exit /b 1
)
setx VCPKG_ROOT C:\vcpkg >nul
set "VCPKG_ROOT=C:\vcpkg"
echo [ OK ] vcpkg ready ^(C:\vcpkg^)
:vcpkg_ok

echo.
echo --- Cleaning any stale build cache -------------------------
if exist "build\release\CMakeCache.txt"  del /q "build\release\CMakeCache.txt"
if exist "build\release\CMakeFiles"      rmdir /s /q "build\release\CMakeFiles"
if exist "build\release\vcpkg_installed" rmdir /s /q "build\release\vcpkg_installed"
echo       done.

echo.
echo --- Building ------------------------------------------------
call "%~dp0build.bat"
