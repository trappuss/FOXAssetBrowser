@echo off
setlocal
title FOXAssetBrowser - Texture Users Sweep
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  THE PHASE NOBODY HAS EVER SEEN FINISH.
::
::  The cold run logged  "sweeping 40754 model(s)..."  and was still going
::  ninety-four seconds later when the app was closed, so fox_texusers_v1.bin
::  was never written and its real cost is simply unknown - a fourth
::  multi-minute startup phase that a cache hit has been hiding ever since.
::
::  This clears that ONE cache file, runs the sweep to completion, and times
::  it. Nothing can be decided about it until the number exists.
::
::  Then it runs the same sweep with 1 worker and with 8, because every
::  setting from 1 to 8 must write a BYTE-IDENTICAL file - the same acceptance
::  the deep scan has. The md5s are printed at the end and must match.
::
::  That invariant already holds on the container's own tree - 425 models,
::  md5 e16c8e814092c462b142f91951b62275 at 1, 2, 4 and 8 workers. What is
::  unknown is whether it still holds at 40,754, and what that costs.
::
::  COULD BE SEVERAL MINUTES PER PASS. Let each one finish.
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
set "REPORT=_deliveries\texusers.txt"
echo FOXAssetBrowser texture-users sweep> "%REPORT%"

call :pass 1
call :pass 8

echo.
echo ------------------------------------------------------------
type "%REPORT%"
echo ------------------------------------------------------------
echo.
echo The two md5 lines must MATCH, and the elapsed times are the
echo number this phase has never had. Written to %REPORT%
echo.
pause
exit /b 0

:pass
set "N=%~1"
echo.
echo ============================================================
echo   %N% worker(s)
echo ============================================================
taskkill /im FOXAssetBrowser.exe /f >nul 2>&1
if exist "%DATA%\cache\fox_texusers_v1.bin" del /q "%DATA%\cache\fox_texusers_v1.bin"
set "FOXAB_TEXUSERS_THREADS=%N%"
echo === %N% worker(s) ===>> "%REPORT%"
echo started %TIME%>> "%REPORT%"
echo Started: %TIME%
"%EXE%" --texusersweep 2>&1 | findstr /i "texusers:"
echo Finished: %TIME%
echo finished %TIME%>> "%REPORT%"
if exist "%DATA%\cache\fox_texusers_v1.bin" (
    certutil -hashfile "%DATA%\cache\fox_texusers_v1.bin" MD5 | findstr /v ":" >> "%REPORT%"
) else (
    echo fox_texusers_v1.bin was NOT written - the sweep did not finish>> "%REPORT%"
)
exit /b 0
