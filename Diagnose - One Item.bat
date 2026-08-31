@echo off
setlocal
title FOXAssetBrowser - One Item, Full Detail
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  THE FULL PER-PART TABLE for ONE item, rather than one line each for ninety.
::
::  Use this when the sweep has named a culprit and the question is WHY: this
::  prints every part in the scene, which of four regimes the pose put it in,
::  which bone on which part it was measured against, the correction that was
::  applied, and the residual - plus the same table on every rebuild during
::  assembly, so a value that changes as parts load is visible as it happens.
::
::  Defaults to hat15. To look at something else, drop its name on this file
::  or run:   "Diagnose - One Item.bat" hat21
:: ---------------------------------------------------------------------------

set "ITEM=%~1"
if "%ITEM%"=="" set "ITEM=hat15"

set "EXE=build\release\FOXAssetBrowser.exe"
set "DATA=build\release\data"
if not exist "%EXE%" ( set "EXE=dist\FOXAssetBrowser.exe" & set "DATA=dist\data" )
if not exist "%EXE%" (
    echo No build found - run clean-rebuild.bat first.
    pause & exit /b 1
)
if not exist "_deliveries" mkdir "_deliveries"

set "FOXAB_DUMP_RESTALIGN=1"
set "CLIP=mgo_pl_rcvr_gl03"

for %%G in (ssd_f ssd_m) do (
    echo.
    echo ============================================================
    echo   %ITEM%  on  %%G
    echo ============================================================
    "%EXE%" --character "%%G,accessory=%ITEM%" --mtar "%CLIP%" --frame 94 ^
            --shot "_deliveries\%ITEM%_%%G.png" 2>&1 ^
        | findstr /i "restalign: devshot: character anim:"
    if exist "%DATA%\FOXAssetBrowser.log" (
        copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\%ITEM%_%%G.log" >nul
    )
)

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :  %ITEM%_ssd_f.log  %ITEM%_ssd_m.log
echo                            %ITEM%_ssd_f.png  %ITEM%_ssd_m.png
echo.
echo If the item is in the ACCESSORY slot this is the right script.
echo If it lives in Head Equipment or Glasses, the sweep covers it
echo and "Diagnose - Accessory Positions.bat" is the one to run.
echo ------------------------------------------------------------
echo.
pause
