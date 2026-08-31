@echo off
setlocal
title FOXAssetBrowser - Accessory Position Sweep
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  ANSWERS "which items are still in the wrong place", for the WHOLE wardrobe,
::  in one double-click. No typing, nothing to set.
::
::  It equips every item of every slot on the Survivor - one at a time - poses
::  each on a real clip, and measures how far the item's own root bone lands
::  from the bone it is supposed to hang on. That distance is the whole answer:
::
::      under 0.02 m   seated
::      0.2 - 1.2 m    floating, which is what "accessories fly away" IS
::
::  IT HAS TO BE POSED. At bind pose every item in the game is in the right
::  place - that is why "it's fine in T-pose" was the reported symptom for
::  eight batches. So it loads a clip first and measures partway through it.
::
::  Both genders. Seconds each once the index is warm; the FIRST run after a
::  cache clear rebuilds the index and takes about three minutes - let it.
::
::  WHAT TO SEND BACK: the two .tsv files in _deliveries\. They open in Excel;
::  sort by the last column and anything saying FLOATING is a real fault with
::  a real number and the bone it hangs from beside it.
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

:: The clip from the original bug report, so the numbers line up with the ones
:: in _9r_report.md. If this install does not have it, the second name is a
:: TPP-native clip that every copy of the game ships - the fault is not
:: specific to either, which 9r measured.
set "CLIP=mgo_pl_rcvr_gl03"
set "CLIP2=pl_rcvr_ar01"

call :sweep ssd_f "SURVIVOR - FEMALE"
call :sweep ssd_m "SURVIVOR - MALE"

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :
echo    accessory_sweep_ssd_f.tsv   accessory_sweep_ssd_f.log
echo    accessory_sweep_ssd_m.tsv   accessory_sweep_ssd_m.log
echo.
echo Send the two .tsv files back.
echo ------------------------------------------------------------
echo.
pause
exit /b 0

:sweep
set "G=%~1"
set "OUT=_deliveries\accessory_sweep_%G%.tsv"
if exist "%OUT%" del /q "%OUT%"
echo.
echo ============================================================
echo   %~2
echo ============================================================
"%EXE%" --character "%G%" --mtar "%CLIP%" --frame 94 --restalignsweep "%OUT%" 2>&1 | findstr /i "restalignsweep: devshot:"
if not exist "%OUT%" (
    echo.
    echo   ^(%CLIP% did not load here - retrying with %CLIP2%^)
    "%EXE%" --character "%G%" --mtar "%CLIP2%" --frame 94 --restalignsweep "%OUT%" 2>&1 | findstr /i "restalignsweep: devshot:"
)
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\accessory_sweep_%G%.log" >nul
if not exist "%OUT%" echo   NOTHING WAS WRITTEN - send _deliveries\accessory_sweep_%G%.log instead.
exit /b 0
