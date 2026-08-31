@echo off
setlocal
title FOXAssetBrowser - Startup Timings
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  WHERE THE MINUTES GO. Three numbers have been outstanding for many
::  batches and each one decides which of several different jobs comes next.
::
::  It launches COLD (caches cleared) and then WARM, and keeps both logs.
::  What matters is in three lines:
::
::   anims:            17.8 s cold / 7.1 s warm, now split five ways as
::                     read / parse / units / clips / rest. WHICH TERM
::                     DOMINATES decides the fix: read = parallelise it like
::                     the deep scan; clips = it is the label build over
::                     24,353 clips, so cache or defer rather than thread;
::                     units = stop calling decodeClip(0) just to learn a
::                     skeleton. Three different batches, one line.
::
::   archives:         about 11.4 s on EVERY launch, cache or no cache, now
::                     split as dictionaries / archive cache / discovery walk
::                     / entry tables / entries. The suspicion is the
::                     discovery walk - a recursive iterator that opens every
::                     file for four magic bytes before any cache is asked -
::                     but that is reasoned and NOT measured. Read it.
::
::   deep scan:        88.6 s of a 119 s cold index. Already threaded in 8w;
::                     this is the confirmation.
::
::  Your settings are never touched - only data\cache\*.bin is cleared, which
::  is the whole reason that folder was separated from the .ini.
::
::  ABOUT FOUR MINUTES. Let the cold run finish; closing it part way leaves
::  the texture sweep unwritten and the next launch pays for it again.
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

echo.
echo Clearing ONLY %DATA%\cache\*.bin  (settings are elsewhere and are safe)
taskkill /im FOXAssetBrowser.exe /f >nul 2>&1
if exist "%DATA%\cache" del /q "%DATA%\cache\*.bin" 2>nul

echo.
echo ============================================================
echo   COLD - rebuilding every index. About three minutes.
echo ============================================================
echo Started: %TIME%
"%EXE%" --filedump "_deliveries\_startup_cold_filelist.tsv" 2>&1 | findstr /i "index: anims: archives: deep players: weapons: equip:"
echo Finished: %TIME%
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\startup_cold.log" >nul

echo.
echo ============================================================
echo   WARM - everything cached. Seconds.
echo ============================================================
echo Started: %TIME%
"%EXE%" --filedump "_deliveries\_startup_warm_filelist.tsv" 2>&1 | findstr /i "index: anims: archives: deep players: weapons: equip:"
echo Finished: %TIME%
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\startup_warm.log" >nul

del /q "_deliveries\_startup_cold_filelist.tsv" "_deliveries\_startup_warm_filelist.tsv" 2>nul

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :  startup_cold.log  startup_warm.log
echo Send both. The three lines above are what they are for.
echo ------------------------------------------------------------
echo.
pause
