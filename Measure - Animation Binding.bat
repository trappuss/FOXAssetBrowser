@echo off
setlocal
title FOXAssetBrowser - Animation Binding
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  THE ONE RUN THAT SETTLES "this model's animations".
::
::  The filter cannot currently separate a Walker Gear from a human, and the
::  reason is measured and written down: the score depends on an archive only
::  through its TRACK COUNT, so every archive with enough tracks scores the
::  same. The obvious fix - intersecting the archive's unit names with the
::  model's BONE hashes - was tried and scored 0.000 on all 108 archives,
::  because a gani unit name is a RIG UNIT name and an FMDL bone hash is a
::  BONE name. Different vocabularies. It was reverted.
::
::  The right comparison is the archive's rig-unit names against the model's
::  own .frig, and that cannot be validated in a container with one .frig in
::  it. This run does it on the real thing, against the model the request
::  named: /Assets/tpp/mecha/mgm/Scenes/mgm1_main0_def.fmdl, which should be
::  playing walkergear2_layers and nothing else.
::
::  READ THE HISTOGRAM in the log. Two clusters with an empty band between
::  them means a threshold exists and the number can be taken off the chart.
::  One smear means there is no threshold and the filter stays as it is - and
::  that is a real answer, not a failure.
::
::  Minutes, not seconds: it scores every motion archive in the install.
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
echo Scoring every motion archive against mgm1_main0_def (the Walker Gear)...
echo.
"%EXE%" --animbind "mgm1_main0_def=_deliveries\animbind_walker.tsv" --animscope model 2>&1 | findstr /i "animbind: animscope: bind:"
if exist "%DATA%\FOXAssetBrowser.log" copy /y "%DATA%\FOXAssetBrowser.log" "_deliveries\animbind_walker.log" >nul

echo.
echo ------------------------------------------------------------
echo Written to _deliveries\ :  animbind_walker.tsv
echo                            animbind_walker.log
echo.
echo The .tsv has two columns added for exactly this question -
echo rigNames and rigFrac. Send both files.
echo ------------------------------------------------------------
echo.
pause
