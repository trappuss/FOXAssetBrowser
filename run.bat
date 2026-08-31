@echo off
setlocal
title FOXAssetBrowser
cd /d "%~dp0"

set "DESTDIR=build\release"
set "EXE=%DESTDIR%\FOXAssetBrowser.exe"
if not exist "%EXE%" (
    echo FOXAssetBrowser.exe not found - build it first:  build.bat
    pause & exit /b 1
)

set "VINST=%DESTDIR%\vcpkg_installed\x64-windows"

:: Deploy the Qt runtime + plugins beside the exe so it runs stand-alone
:: (vcpkg's Qt does not ship windeployqt; copying by hand is the working path).
echo Deploying runtime DLLs and Qt plugins...
copy /y "%VINST%\bin\*.dll" "%DESTDIR%\" >nul 2>&1
for %%P in (platforms imageformats styles iconengines) do (
    if not exist "%DESTDIR%\%%P" mkdir "%DESTDIR%\%%P"
    copy /y "%VINST%\Qt6\plugins\%%P\*.dll" "%DESTDIR%\%%P\" >nul 2>&1
)

:: Ship the dictionaries beside the exe (the app reads dict\ next to itself).
if exist "dict" (
    if not exist "%DESTDIR%\dict" mkdir "%DESTDIR%\dict"
    copy /y "dict\*.txt" "%DESTDIR%\dict\" >nul 2>&1
)

echo Launching...
start "" "%EXE%"
