@echo off
setlocal enabledelayedexpansion
title FOXAssetBrowser - GitHub
cd /d "%~dp0"

:: ============================================================================
::  github.bat - commit, push and release, from a menu. Double-click it.
::  Template §13. Modelled on D4AssetBrowser's, with its hard-won details kept.
::
::  You can skip the menu:  github.bat  fixed the parts panel on shared groups
::
::  THE PAGER. git sends anything longer than a screen to "less", which takes
::  the console over and waits for keys nobody expects to press - the script
::  looks frozen. With a hundred changed files that is the normal path, not an
::  edge case, so every command that PRINTS gets --no-pager. Per command rather
::  than globally, which leaves the user's own git config alone.
:: ============================================================================

set "GIT=git --no-pager"

where git >nul 2>&1
if errorlevel 1 (
    echo ERROR: git is not on PATH. Install Git for Windows, then re-run this.
    pause & exit /b 1
)
if not exist ".git" (
    echo This folder is not a git repository yet.
    echo.
    set /p INIT="Create one here now? [y/N] "
    if /i not "!INIT!"=="y" exit /b 1
    %GIT% init
    %GIT% branch -M main
    echo.
    echo Now add a remote:  git remote add origin https^://github.com/^<you^>/^<repo^>.git
    echo Then run this again.
    pause & exit /b 0
)

if not "%*"=="" (
    set "MSG=%*"
    call :do_commit_push
    pause
    exit /b 0
)

:menu
cls
echo ============================================================
echo   FOXAssetBrowser - GitHub
echo ============================================================
%GIT% status --short --branch
echo.
echo   1  Commit and push everything changed
echo   2  Show what would be committed (status + diff --stat)
echo   3  Pull (fast-forward only)
echo   4  Cut a release - tag, and upload the built exe
echo   5  Open the repository in a browser
echo   0  Quit
echo.
set "CH="
set /p CH="Choice: "
if "%CH%"=="1" goto ask_msg
if "%CH%"=="2" goto show
if "%CH%"=="3" goto pull
if "%CH%"=="4" goto release
if "%CH%"=="5" goto openweb
if "%CH%"=="0" exit /b 0
goto menu

:ask_msg
echo.
set "MSG="
set /p MSG="Commit message: "
if not defined MSG ( echo Nothing entered. & pause & goto menu )
call :do_commit_push
pause
goto menu

:show
echo.
%GIT% status
echo.
%GIT% diff --stat
echo.
pause
goto menu

:pull
echo.
:: --ff-only on purpose: a merge commit created by a script nobody was watching
:: is how a history becomes unreadable. If it refuses, the divergence is real
:: and wants a human.
%GIT% pull --ff-only
echo.
pause
goto menu

:openweb
for /f "usebackq tokens=*" %%u in (`%GIT% config --get remote.origin.url`) do set "URL=%%u"
if not defined URL ( echo No 'origin' remote is set. & pause & goto menu )
set "URL=!URL:.git=!"
start "" "!URL!"
goto menu

:release
echo.
set "TAG="
set /p TAG="Tag (e.g. v0.2.0): "
if not defined TAG ( echo Nothing entered. & pause & goto menu )
set "EXE=build\release\FOXAssetBrowser.exe"
if not exist "%EXE%" set "EXE=dist\FOXAssetBrowser.exe"
if not exist "%EXE%" (
    echo.
    echo No built exe found - run build.bat or rebuild.bat first, so the
    echo release has something to attach.
    pause & goto menu
)
%GIT% tag -a "!TAG!" -m "!TAG!"
%GIT% push origin "!TAG!"
where gh >nul 2>&1
if errorlevel 1 (
    echo.
    echo Tag pushed. The GitHub CLI ^(gh^) is not installed, so the exe was not
    echo uploaded - attach %EXE% to the release on the website, or install gh.
) else (
    gh release create "!TAG!" "%EXE%" --title "!TAG!" --generate-notes
)
echo.
pause
goto menu

:: ---------------------------------------------------------------------------
:do_commit_push
echo.
%GIT% add -A
:: --quiet --exit-code: 1 when there IS something staged. Committing nothing
:: makes git print an error that reads like a failure, on the commonest path
:: there is - pressing the button twice.
%GIT% diff --cached --quiet --exit-code
if not errorlevel 1 (
    echo Nothing to commit - the working tree matches the last commit.
    exit /b 0
)
%GIT% commit -m "!MSG!"
if errorlevel 1 ( echo Commit failed - see above. & exit /b 1 )
:: Set the upstream on the first push, and only then: -u on every push would
:: silently repoint the branch if someone had deliberately moved it.
for /f "usebackq tokens=*" %%b in (`%GIT% rev-parse --abbrev-ref HEAD`) do set "BR=%%b"
%GIT% rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>&1
if errorlevel 1 ( %GIT% push -u origin "!BR!" ) else ( %GIT% push )
exit /b %errorlevel%
