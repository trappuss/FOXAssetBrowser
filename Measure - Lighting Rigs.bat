@echo off
setlocal
title FOXAssetBrowser - Lighting Rigs
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  THE OLDEST UNMEASURED THING IN THE PROJECT. Ten minutes, outstanding for
::  many sessions.
::
::  The four per-game lighting rigs in src/gl/ViewEnvironment.cpp are AUTHORED
::  NUMBERS - somebody's guess at key direction, gain, ambient and exposure for
::  TPP, MGO3, GZ and Survive. --lightdump has existed the whole time and has
::  never once been run, so nobody knows how far off they are.
::
::  This writes what each rig actually resolves to. One run replaces four
::  guesses with four measurements.
:: ---------------------------------------------------------------------------

set "EXE=build\release\FOXAssetBrowser.exe"
set "DATA=build\release\data"
if not exist "%EXE%" set "EXE=dist\FOXAssetBrowser.exe"
if not exist "%EXE%" (
    echo No build found - run clean-rebuild.bat first.
    pause & exit /b 1
)
if not exist "build\release\FOXAssetBrowser.exe" set "DATA=dist\data"
if not exist "_deliveries" mkdir "_deliveries"

"%EXE%" --lightdump "_deliveries\lightdump.tsv" 2>&1 | findstr /i "light: lightdump: devshot:"
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\lightdump.log" >nul

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :  lightdump.tsv   lightdump.log
echo ------------------------------------------------------------
echo.
pause
