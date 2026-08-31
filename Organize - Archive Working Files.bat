@echo off
setlocal EnableExtensions EnableDelayedExpansion
title FOXAssetBrowser - Organize the folder
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  TIDY THE PROJECT FOLDER. NOTHING IS DELETED.
::
::  Everything that is not the project itself moves under _archive\ , which is
::  in .gitignore, so the root becomes just the tool and GitHub never sees the
::  working history. You delete _archive\ yourself whenever you are ready - it
::  is the only folder this touches and it is the only thing left to clean up.
::
::  What moves:
::    _archive\snapshots\   the foxab-*.zip batch snapshots
::    _archive\reports\     _8*_report.md, _9*_report.md and the audits
::    _archive\notes\       MORNING_REPORT, NEW_SESSION_PROMPT, MGO_FACTS and
::                          the rest of the session documents
::    _archive\work\        _deliveries, _incoming, _probe, _Backups, .Backups,
::                          _to_delete, _skills, .Resources
::    _archive\logs\        build_log.txt, build_errors.txt
::
::  What stays: src res dict tools, the CMake files, verify-src.py, every .bat,
::  README.md, LICENSE, CHANGELOG.md, VERSION, .gitignore and wiki\.
::
::  Run it as many times as you like. It only ever moves things one way.
:: ---------------------------------------------------------------------------

echo.
echo   ============================================================
echo     Organizing  %CD%
echo   ============================================================
echo.

set "A=%CD%\_archive"
for %%D in (snapshots reports notes work logs) do if not exist "%A%\%%D" mkdir "%A%\%%D" >nul 2>&1

set /a MOVED=0

:: ---- batch snapshots ------------------------------------------------------
echo   [1/5] batch snapshots ...
for %%F in (foxab-*.zip) do (
    move /y "%%F" "%A%\snapshots\" >nul 2>&1 && set /a MOVED+=1
)

:: ---- batch reports --------------------------------------------------------
echo   [2/5] batch reports ...
for %%F in (_8*_report.md _9*_report.md _8*_verify.md) do (
    if exist "%%F" ( move /y "%%F" "%A%\reports\" >nul 2>&1 && set /a MOVED+=1 )
)
for %%F in (_SESSION_AUDIT.md CUSTOMIZE_AUDIT.md) do (
    if exist "%%F" ( move /y "%%F" "%A%\reports\" >nul 2>&1 && set /a MOVED+=1 )
)

:: ---- session documents ----------------------------------------------------
:: MORNING_REPORT.md is the running log and MGO_FACTS.md is the measured
:: ground truth. NEITHER IS DELETED - the wiki was written from them and they
:: stay readable in _archive\notes\ .
echo   [3/5] session documents ...
for %%F in (MORNING_REPORT.md NEW_SESSION_PROMPT.md MGO_FACTS.md FABLE_PROMPT.md FOLDER_MAP.md) do (
    if exist "%%F" ( move /y "%%F" "%A%\notes\" >nul 2>&1 && set /a MOVED+=1 )
)
for %%F in (foxab-codebase.skill) do (
    if exist "%%F" ( move /y "%%F" "%A%\notes\" >nul 2>&1 && set /a MOVED+=1 )
)
for %%F in (_9z_accessory_evidence.png _9z_accessory_geometry.png) do (
    if exist "%%F" ( move /y "%%F" "%A%\notes\" >nul 2>&1 && set /a MOVED+=1 )
)

:: ---- working folders ------------------------------------------------------
:: _probe holds several hundred MB of pulls. Moving a folder on the same drive
:: is a rename, so this is instant however large it is.
echo   [4/5] working folders ...
for %%D in (_deliveries _incoming _probe _Backups _to_delete _skills) do (
    if exist "%%D\" (
        if exist "%A%\work\%%D\" (
            echo         %%D already archived - leaving the current one in place
        ) else (
            move /y "%%D" "%A%\work\" >nul 2>&1 && set /a MOVED+=1
        )
    )
)
:: the dot-prefixed pair, which the loop above will not match
for %%D in (".Backups" ".Resources") do (
    if exist %%D\ (
        if not exist "%A%\work\%%~nxD\" ( move /y %%D "%A%\work\" >nul 2>&1 && set /a MOVED+=1 )
    )
)

:: ---- build logs -----------------------------------------------------------
echo   [5/5] build logs ...
for %%F in (build_log.txt build_errors.txt) do (
    if exist "%%F" ( move /y "%%F" "%A%\logs\" >nul 2>&1 && set /a MOVED+=1 )
)

echo.
echo   ------------------------------------------------------------
echo     Moved !MOVED! item(s) into _archive\
echo   ------------------------------------------------------------
echo.
echo   The root now holds the project only. What is left:
echo.
dir /b /a-d 2>nul | findstr /v /i "^_archive$"
echo.
echo   Folders:
dir /b /ad 2>nul
echo.
echo   NOTHING WAS DELETED. _archive\ still has all of it, including
echo   MGO_FACTS.md and MORNING_REPORT.md, and .gitignore keeps it out of git.
echo   Delete _archive\ yourself when you are happy.
echo.
echo   Next: run  publish.bat  to put this on GitHub.
echo.
pause
