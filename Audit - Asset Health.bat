@echo off
setlocal enabledelayedexpansion
title FOXAssetBrowser - Asset Health Audit
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  Walks every .fmdl in the configured game folders and says whether this tool
::  can actually draw it, then DIFFS against the previous run (template §13).
::
::  The diff is the point. A game patch, a mod install or a re-extract quietly
::  changes what resolves, and without a before-and-after that surfaces weeks
::  later as "this used to work". Run it once to lay down a baseline, then
::  again after anything that touches the install.
::
::  The report is _audit\asset_health.tsv - path, state, detail - which opens
::  in Excel and diffs cleanly in git. Every state, and what it means:
::
::    ok                every texture the model asks for is in this install
::    textures-partial  some resolve; the model draws with holes
::    textures-missing  none resolve; the model draws flat grey
::    no-textures       the model declares none at all (some props genuinely do)
::    no-geometry       parses, but has no triangles - usually a bone-only rig
::    unparsed          the FMDL parser refused it; the reason is in `detail`
::    no-data           the archive entry read back empty
::
::  It parses each model and RESOLVES its texture references. It does not
::  decode a texture or draw anything: a full install is tens of thousands of
::  models, and an audit that rendered each one would be an overnight job and
::  would never get run.
:: ---------------------------------------------------------------------------

set "EXE=build\release\FOXAssetBrowser.exe"
if not exist "%EXE%" set "EXE=dist\FOXAssetBrowser.exe"
if not exist "%EXE%" (
    echo No build found - run build.bat or rebuild.bat first.
    pause & exit /b 1
)

if not exist "_audit" mkdir "_audit"
set "REPORT=_audit\asset_health.tsv"

:: Keep the PREVIOUS report beside the new one. The tool overwrites the file it
:: diffs against, so without this copy there is no way to look at what the run
:: was comparing to once it has finished.
if exist "%REPORT%" copy /y "%REPORT%" "_audit\asset_health.previous.tsv" >nul

echo Auditing every model in the configured game folders...
echo (this reads every .fmdl once - minutes on a full install)
echo.
"%EXE%" --healthaudit "%REPORT%" 2>&1 | findstr /i "audit:"
set "RC=%errorlevel%"

echo.
if exist "%REPORT%" (
    echo Report:   %REPORT%
    if exist "_audit\asset_health.previous.tsv" echo Previous: _audit\asset_health.previous.tsv
) else (
    echo No report was written - see the lines above.
)
echo.
pause
exit /b %RC%
