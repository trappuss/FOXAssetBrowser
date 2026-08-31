@echo off
setlocal
title FOXAssetBrowser - Rebuild Index Cache
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  Forces the next launch to rebuild every index cache from the game files.
::
::  WHY THIS EXISTS RATHER THAN "just delete data\":
::  the settings file lives in data\ too, and deleting the folder took a user's
::  entire configuration with it - every game folder, gone. From the outside the
::  two failures look identical: an app with no caches and an app with no game
::  folders both start instantly and show nothing. This script empties
::  data\cache\ and cannot touch data\FOXAssetBrowser\FOXAssetBrowser.ini.
::
::  WHAT IT COSTS. Measured on a full install (TPP + MGO3 + Survive + GZ):
::
::      container deep scan   88.6 s     1,111,128 entries
::      archive entry tables  18.9 s     273,094 entries across 32 archives
::      animation catalogue   17.8 s     508 archives, 24,353 clips
::      texture->model sweep  90 s+      40,754 models
::      ------------------------------------------------------------------
::      first launch after this runs      about three minutes
::
::  Let it finish. Closing the window part way leaves the sweep unwritten and
::  the next launch pays for it again.
:: ---------------------------------------------------------------------------

set "CACHE=build\release\data\cache"
if not exist "%CACHE%" set "CACHE=dist\data\cache"
if not exist "%CACHE%" (
    echo No cache folder found - nothing to clear.
    echo   ^(looked in build\release\data\cache and dist\data\cache^)
    pause & exit /b 0
)

echo About to empty:  %CACHE%
echo.
dir /b "%CACHE%\*.bin" 2>nul || echo   ^(already empty^)
echo.
echo Your settings are NOT in that folder and will not be touched.
echo.
choice /c YN /m "Clear it"
if errorlevel 2 ( echo Cancelled. & pause & exit /b 0 )

taskkill /im FOXAssetBrowser.exe /f >nul 2>&1
del /q "%CACHE%\*.bin" 2>nul

echo.
echo Cleared. The next launch rebuilds - expect about three minutes, and
echo let it run to the end.
echo.
pause
