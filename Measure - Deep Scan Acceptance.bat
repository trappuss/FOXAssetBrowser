@echo off
setlocal
title FOXAssetBrowser - Deep Scan Acceptance Test
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  THE ONE TEST 8w OWES, and the only one that says whether that batch was
::  correct at all.
::
::  The container deep scan was 88.6 s of a 119 s cold index - 74 percent of
::  it - and 8w split it across every core, merging the chunks in order. If
::  the merge is NOT in chunk order, the index comes out different depending
::  on how many workers ran, and nothing else about the batch matters.
::
::  So: build the index with ONE worker, hash it. Build it again with EIGHT,
::  hash it again.
::
::      IDENTICAL HASHES + a lower time  =  8w is correct.
::      DIFFERENT HASHES                 =  the merge is out of order.
::
::  The hashes are printed at the end. They are the answer; nothing else in
::  this run needs reading.
::
::  Two cold indexes back to back - about six minutes. Let it run.
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
set "REPORT=_deliveries\deepscan_acceptance.txt"
echo FOXAssetBrowser deep-scan acceptance test> "%REPORT%"
echo.>> "%REPORT%"

call :pass 1
call :pass 8

echo.>> "%REPORT%"
echo ------------------------------------------------------------
type "%REPORT%"
echo ------------------------------------------------------------
echo.
echo The two md5 lines must MATCH. Written to %REPORT%
echo.
pause
exit /b 0

:pass
set "N=%~1"
echo.
echo ============================================================
echo   %N% worker(s) - clearing caches and rebuilding
echo ============================================================
taskkill /im FOXAssetBrowser.exe /f >nul 2>&1
if exist "%DATA%\cache" del /q "%DATA%\cache\*.bin" 2>nul
set "FOXAB_DEEPSCAN_THREADS=%N%"
echo Started: %TIME%
"%EXE%" --cachecheck 2>&1 | findstr /i "deep index:"
echo Finished: %TIME%
echo === %N% worker(s) ===>> "%REPORT%"
if exist "%DATA%\cache\fox_index_v5.bin" (
    :: certutil prints three lines and only the middle one is the hash; the
    :: other two both contain a colon, which is the whole filter.
    certutil -hashfile "%DATA%\cache\fox_index_v5.bin" MD5 | findstr /v ":" >> "%REPORT%"
) else (
    echo fox_index_v5.bin was NOT written>> "%REPORT%"
)
exit /b 0
