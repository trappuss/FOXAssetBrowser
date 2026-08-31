@echo off
setlocal
title FOXAssetBrowser - Weapon Camo Census
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  SETTLES THE CAMO QUESTION, so menu rows can be deleted on evidence.
::
::  Two runs, two files:
::
::  1. --camodump    every row the Camo / Variation combo would build, for
::                   every weapon in the install, with the section it lands
::                   under. Answers: does a weapon really ship BOTH cam and
::                   clv, and is anything ELSE in MODEL VARIATION. On the
::                   reference pull the answer was seven names, not two -
::                   cam, clv, def and the four DLC colours m01/m02/m03/m68 -
::                   and only 4 stems of 35 shipped cam or clv at all.
::
::  2. --fovacensus  every texture substitution the camo tables make, with the
::                   texture's real NAME. THIS IS THE DECISIVE ONE. If those
::                   names come back as cm_camo4_cNN swatches, the camo
::                   patterns really are the same art the gear-colour picker
::                   already offers and the section goes. If they are
::                   per-weapon textures, hiding the section would lose art
::                   nothing else can reach.
::
::  On the reference pull the paths could not be resolved - its dictionaries
::  do not cover MGO weapon textures. A full install's do.
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
echo ============================================================
echo   1 of 2 - the camo list, every weapon
echo ============================================================
"%EXE%" --camodump "_deliveries\camodump.tsv" 2>&1 | findstr /i "camodump:"
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\camodump.log" >nul

echo.
echo ============================================================
echo   2 of 2 - what the camo tables actually substitute
echo ============================================================
"%EXE%" --fovacensus "camo_c=_deliveries\camo_substitutions.tsv" 2>&1 | findstr /i "fovacensus:"
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\camo_substitutions.log" >nul

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :
echo    camodump.tsv               camodump.log
echo    camo_substitutions.tsv     camo_substitutions.log
echo.
echo In camo_substitutions.tsv, look at the LAST column (filePath).
echo   names like  ...\common_source\layer\...\cm_camo4_cNN.ftex
echo        =^> the camo rows are redundant with gear colour, delete them
echo   per-weapon texture names
echo        =^> they are art nothing else reaches, keep the section
echo ------------------------------------------------------------
echo.
pause
