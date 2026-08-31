@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Publish to GitHub
rem ===========================================================================
rem  publish.bat - put a project on GitHub, or update it.  MIT.
rem
rem  Double-click it from inside your project folder. It will:
rem     install Git and the GitHub CLI if they are missing
rem     sign you in to GitHub
rem     create the repository (or update an existing one)
rem     publish a release with your build files attached
rem     create and fill the wiki from a wiki\ folder
rem
rem  Safe to run again - it skips whatever is already done.
rem  Settings live in .publish.cfg, written on first run.
rem ===========================================================================

rem ---- locate the project root (this file may sit in the root or scripts\) --
set "ROOT=%~dp0"
if exist "%~dp0..\.git" set "ROOT=%~dp0..\"
if not exist "%ROOT%.git" if exist "%~dp0..\.publish.cfg" set "ROOT=%~dp0..\"
pushd "%ROOT%" & set "ROOT=%CD%\" & popd
cd /d "%ROOT%"

echo.
echo   ============================================================
echo     Publish to GitHub
echo     %ROOT%
echo   ============================================================
echo.

rem ---- 1. tools ------------------------------------------------------------
echo   [1/7] Git and GitHub CLI...
set "PATH=%PATH%;%ProgramFiles%\Git\cmd;%ProgramFiles%\GitHub CLI"
where git >nul 2>&1 || (
  echo         installing Git...
  winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements
  set "PATH=!PATH!;%ProgramFiles%\Git\cmd"
)
where gh >nul 2>&1 || (
  echo         installing GitHub CLI...
  winget install --id GitHub.cli -e --source winget --accept-package-agreements --accept-source-agreements
  set "PATH=!PATH!;%ProgramFiles%\GitHub CLI"
)
where git >nul 2>&1 || goto :needrestart
where gh  >nul 2>&1 || goto :needrestart
echo         ok
goto :config

:needrestart
echo.
echo   Git and/or the GitHub CLI were just installed, but this window cannot
echo   see them yet. Close this window and run the file again.
echo.
echo   Manual downloads if needed:
echo       https://git-scm.com/download/win
echo       https://cli.github.com
echo.
pause & exit /b 1

rem ---- 2. settings ---------------------------------------------------------
:config
set "REPO_NAME="
set "VISIBILITY=public"
set "VERSION="
set "TAG_PREFIX=v"
set "NOTES=CHANGELOG.md"
set "ASSETS="
set "WIKI=wiki"
set "BRANCH="
set "SKIP_RELEASE=0"

if exist ".publish.cfg" (
  for /f "usebackq eol=# tokens=1,* delims==" %%A in (".publish.cfg") do (
    set "K=%%A" & set "V=%%B"
    for /f "tokens=* delims= " %%X in ("!K!") do set "K=%%X"
    if /I "!K!"=="REPO"         set "REPO_NAME=!V!"
    if /I "!K!"=="VISIBILITY"   set "VISIBILITY=!V!"
    if /I "!K!"=="VERSION"      set "VERSION=!V!"
    if /I "!K!"=="TAG_PREFIX"   set "TAG_PREFIX=!V!"
    if /I "!K!"=="NOTES"        set "NOTES=!V!"
    if /I "!K!"=="ASSETS"       set "ASSETS=!V!"
    if /I "!K!"=="WIKI"         set "WIKI=!V!"
    if /I "!K!"=="BRANCH"       set "BRANCH=!V!"
    if /I "!K!"=="SKIP_RELEASE" set "SKIP_RELEASE=!V!"
  )
)
if "!REPO_NAME!"=="" for %%F in ("%ROOT:~0,-1%") do set "REPO_NAME=%%~nxF"

rem ---- 3. version ----------------------------------------------------------
echo   [2/7] version...
if "!VERSION!"=="" (
  for /f "delims=" %%V in ('powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$v=$null;" ^
    "foreach($f in @(Get-ChildItem -Recurse -Filter *.props -EA 0)+@(Get-ChildItem -Recurse -Filter *.csproj -EA 0)){ if(-not $v){ $m=[regex]::Match((Get-Content $f.FullName -Raw),'<Version>([^<]+)</Version>'); if($m.Success){$v=$m.Groups[1].Value} } }" ^
    "if(-not $v -and (Test-Path 'package.json')){ $v=(Get-Content package.json -Raw ^| ConvertFrom-Json).version }" ^
    "if(-not $v -and (Test-Path 'pyproject.toml')){ $m=[regex]::Match((Get-Content pyproject.toml -Raw),'(?m)^\s*version\s*=\s*[\"'']([^\"'']+)'); if($m.Success){$v=$m.Groups[1].Value} }" ^
    "if(-not $v -and (Test-Path 'Cargo.toml')){ $m=[regex]::Match((Get-Content Cargo.toml -Raw),'(?m)^\s*version\s*=\s*[\"'']([^\"'']+)'); if($m.Success){$v=$m.Groups[1].Value} }" ^
    "foreach($f in 'VERSION','VERSION.txt'){ if(-not $v -and (Test-Path $f)){ $v=(Get-Content $f -Raw).Trim() } }" ^
    "if(-not $v){ $v=Get-Date -Format 'yyyy.MM.dd' }" ^
    "$v.Trim()"') do set "VERSION=%%V"
)
set "TAG=!TAG_PREFIX!!VERSION!"
echo         !VERSION!   (tag !TAG!)

rem write the config back so it is easy to adjust next time
if not exist ".publish.cfg" (
  > ".publish.cfg" echo # Settings for publish.bat. Blank means "work it out automatically".
  >>".publish.cfg" echo REPO=!REPO_NAME!
  >>".publish.cfg" echo VISIBILITY=!VISIBILITY!
  >>".publish.cfg" echo # VERSION= leave blank to read it from the project files
  >>".publish.cfg" echo VERSION=
  >>".publish.cfg" echo TAG_PREFIX=!TAG_PREFIX!
  >>".publish.cfg" echo NOTES=!NOTES!
  >>".publish.cfg" echo # ASSETS: files to attach to the release, separated by spaces.
  >>".publish.cfg" echo #         Blank attaches everything in dist\ plus a source zip.
  >>".publish.cfg" echo ASSETS=
  >>".publish.cfg" echo # WIKI: folder of .md pages to publish as the wiki. Blank disables it.
  >>".publish.cfg" echo WIKI=!WIKI!
  >>".publish.cfg" echo BRANCH=
  >>".publish.cfg" echo SKIP_RELEASE=0
  echo         wrote .publish.cfg - edit it to change any of this
)

rem ---- 4. identity and sign-in --------------------------------------------
echo   [3/7] GitHub sign-in...
gh auth status >nul 2>&1
if errorlevel 1 (
  echo.
  echo         A browser will open. Choose:  GitHub.com  /  HTTPS  /  Yes  /  browser
  echo.
  gh auth login || (echo         sign-in failed & pause & exit /b 1)
)
for /f "delims=" %%U in ('gh api user -q .login 2^>nul') do set "GHUSER=%%U"
for /f "delims=" %%I in ('gh api user -q .id 2^>nul') do set "GHID=%%I"
set "NOREPLY=!GHID!+!GHUSER!@users.noreply.github.com"
echo         signed in as !GHUSER!

rem ---- 5. repository -------------------------------------------------------
echo   [4/7] repository...
if not exist ".git" (
  git init -q || (pause & exit /b 1)
  git branch -M main
  echo         created a local repository
)
rem GitHub refuses pushes that would expose a private email (error GH007),
rem so commit as the noreply address it issues for this purpose.
git config user.name "!GHUSER!"
git config user.email "!NOREPLY!"
git rev-parse HEAD >nul 2>&1 && git commit -q --amend --reset-author --no-edit 2>nul

echo.
set "ANSWER=!REPO_NAME!"
set /p ANSWER="        Publish to which repository? [!REPO_NAME!]: "
if not "!ANSWER!"=="" set "REPO_NAME=!ANSWER!"

set "EXISTS=0"
gh repo view "!GHUSER!/!REPO_NAME!" >nul 2>&1 && set "EXISTS=1"

if "!EXISTS!"=="1" (
  if "!BRANCH!"=="" for /f "delims=" %%B in ('gh repo view "!GHUSER!/!REPO_NAME!" --json defaultBranchRef -q .defaultBranchRef.name 2^>nul') do set "BRANCH=%%B"
  if "!BRANCH!"=="" set "BRANCH=main"
  git remote get-url origin >nul 2>&1
  if errorlevel 1 (
    git remote add origin "https://github.com/!GHUSER!/!REPO_NAME!.git"
  ) else (
    git remote set-url origin "https://github.com/!GHUSER!/!REPO_NAME!.git"
  )
  git add -A
  git diff --cached --quiet || git commit -q -m "!VERSION!"
  git branch -M !BRANCH!
  git push -u origin !BRANCH! 2>nul
  if errorlevel 1 (
    echo.
    echo         The remote has commits this folder does not share.
    echo         Replacing it means the repository will match THIS folder exactly.
    choice /C YN /N /M "        Replace the remote contents? (Y/N) "
    if errorlevel 2 (echo         cancelled - nothing was changed & pause & exit /b 0)
    git push -u origin !BRANCH! --force || (echo         push failed & pause & exit /b 1)
  )
) else (
  set "VIS=--public"
  if /I "!VISIBILITY!"=="private" set "VIS=--private"
  set "BRANCH=main"
  git add -A
  git diff --cached --quiet || git commit -q -m "!VERSION!"
  echo         creating github.com/!GHUSER!/!REPO_NAME! ...
  gh repo create "!REPO_NAME!" !VIS! --source . --remote origin --push || (
     echo         could not create the repository & pause & exit /b 1)
)
set "REPO=!GHUSER!/!REPO_NAME!"
echo         !REPO! ^(!BRANCH!^)

rem ---- 6. release ----------------------------------------------------------
if "!SKIP_RELEASE!"=="1" (echo   [5/7] release skipped by .publish.cfg & goto :wiki)
echo   [5/7] release assets...

set "ASSETLIST="
if not "!ASSETS!"=="" (
  for %%A in (!ASSETS!) do if exist "%%A" set "ASSETLIST=!ASSETLIST! "%%A""
) else (
  if exist "dist" (
    for %%A in (dist\*.zip dist\*.exe dist\*.msi dist\*.7z dist\*.tar.gz) do set "ASSETLIST=!ASSETLIST! "%%A""
  )
)
if "!ASSETLIST!"=="" (
  echo         no build files found - attaching a source zip instead
  if not exist "dist" mkdir "dist"
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$n='dist\!REPO_NAME!-!VERSION!-source.zip';" ^
    "Remove-Item -Force $n -EA SilentlyContinue;" ^
    "$items=Get-ChildItem -Force ^| Where-Object { $_.Name -notin @('.git','dist','.publish.cfg') };" ^
    "Compress-Archive -Path $items.FullName -DestinationPath $n" || (pause & exit /b 1)
  set "ASSETLIST= "dist\!REPO_NAME!-!VERSION!-source.zip""
)

set "NOTESARG=--generate-notes"
if exist "!NOTES!" set "NOTESARG=--notes-file "!NOTES!""

echo   [6/7] release !TAG!...
gh release view !TAG! >nul 2>&1
if errorlevel 1 (
  gh release create !TAG! !ASSETLIST! --title "!REPO_NAME! !VERSION!" !NOTESARG! || (pause & exit /b 1)
  echo         created
) else (
  gh release upload !TAG! !ASSETLIST! --clobber >nul || (pause & exit /b 1)
  if exist "!NOTES!" gh release edit !TAG! --notes-file "!NOTES!" >nul
  echo         updated
)

rem ---- 7. wiki -------------------------------------------------------------
:wiki
echo   [7/7] wiki...
if "!WIKI!"=="" (echo         disabled in .publish.cfg & goto :done)
if not exist "!WIKI!" (echo         no !WIKI!\ folder - skipping & goto :done)
gh api -X PATCH "repos/!REPO!" -f has_wiki=true >nul 2>&1

set "W=%TEMP%\ghwiki_!REPO_NAME!"
if exist "%W%" rmdir /s /q "%W%"
git clone -q "https://github.com/!REPO!.wiki.git" "%W%" 2>nul
if exist "%W%\.git" goto :wikipush

echo         wiki does not exist yet - trying to create it...
mkdir "%W%" 2>nul
pushd "%W%"
git init -q
copy /y "!ROOT!!WIKI!\*.md" . >nul
git add -A
git -c user.name="!GHUSER!" -c user.email="!NOREPLY!" commit -q -m "Wiki" 2>nul
git remote add origin "https://github.com/!REPO!.wiki.git"
git branch -M master
git push -q -u origin master 2>nul
if not errorlevel 1 (popd & echo         wiki created and pushed & goto :done)
git branch -M main
git push -q -u origin main 2>nul
if not errorlevel 1 (popd & echo         wiki created and pushed & goto :done)
popd

echo.
echo         GitHub only creates a wiki after its first page is saved on the
echo         website. Opening it now - type any character, click "Save page",
echo         then come back here and press a key.
echo.
start "" "https://github.com/!REPO!/wiki"
pause
if exist "%W%" rmdir /s /q "%W%"
git clone -q "https://github.com/!REPO!.wiki.git" "%W%" 2>nul
if not exist "%W%\.git" (echo         still cannot reach the wiki - run this again later & goto :done)

:wikipush
copy /y "!ROOT!!WIKI!\*.md" "%W%\" >nul
pushd "%W%"
git add -A
git diff --cached --quiet && (
  echo         already up to date
) || (
  git -c user.name="!GHUSER!" -c user.email="!NOREPLY!" commit -q -m "Wiki update for !VERSION!"
  git push -q && echo         pushed
)
popd

:done
echo.
echo   ============================================================
echo     Done.
echo.
echo     Code     https://github.com/!REPO!
if not "!SKIP_RELEASE!"=="1" echo     Release  https://github.com/!REPO!/releases/tag/!TAG!
if not "!WIKI!"=="" echo     Wiki     https://github.com/!REPO!/wiki
echo   ============================================================
echo.
choice /C YN /N /M "   Open in your browser? (Y/N) "
if errorlevel 2 goto :end
start "" "https://github.com/!REPO!"
:end
echo.
pause
exit /b 0
