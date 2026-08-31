@echo off
setlocal
title FOXAssetBrowser - Package the mod folder
cd /d "%~dp0"

:: ---------------------------------------------------------------------------
::  PACKAGE THE MOD FOLDER, BOTH WAYS, AND CHECK WINDOWS CAN READ THE RESULT.
::
::  Two different deliverables come out of this, not two formats of one thing:
::
::    mod_package.zip    the Assets\ tree plus a manifest. Anyone can unzip it
::                       over a mod folder. Always works.
::    <name>.mgsv        a real SnakeBite mod. A mod manager installs it into
::                       the game. Refused, with the files named, if the folder
::                       holds an asset the game keeps inside an .fpk - those
::                       have to be re-packed into that .fpk and this tool
::                       cannot write one yet.
::
::  Export > Package mod folder... and Export > Package as a SnakeBite mod...
::  do the same two jobs from the menu. This script exists for the part the
::  menu cannot do: the ZIP container is written by this tool's own code, so
::  the script unpacks it again with Windows' own reader and checks every file
::  that comes back against the MD5 that was recorded when it went in.
::
::  OK on the last line of step 3: the package is good, send it to anyone.
::  Anything else: send me the log and do NOT send anyone the zip.
:: ---------------------------------------------------------------------------

set "EXE=build\release\FOXAssetBrowser.exe"
if not exist "%EXE%" set "EXE=dist\FOXAssetBrowser.exe"
if not exist "%EXE%" (
    echo No build found - run clean-rebuild.bat first.
    pause & exit /b 1
)
if not exist "_deliveries" mkdir "_deliveries"

set "ZIP=%~dp0_deliveries\mod_package.zip"
set "UNPACK=%~dp0_deliveries\mod_package_check"
if exist "%ZIP%" del /q "%ZIP%"
if exist "%UNPACK%" rmdir /s /q "%UNPACK%"

echo.
echo ============================================================
echo   1 of 4   what is in the mod folder
echo ============================================================
:: One dump per run - the harness answers the FIRST one it is given and
:: ignores the rest - so this is a separate launch from the packaging below.
"%EXE%" --moddump "_deliveries\mod_contents.tsv" 2>&1 | findstr /i "mod:"

echo.
echo ============================================================
echo   2 of 4   writing the plain package
echo ============================================================
"%EXE%" --modpackage "%ZIP%" 2>&1 | findstr /i "modpackage"

if not exist "%ZIP%" (
    echo.
    echo Nothing was written. The usual reason is that no mod folder is set
    echo yet - Settings ^> Folders - or that it holds no replacements. Replace
    echo an asset from any right-click menu first.
    goto :done
)

echo.
echo ============================================================
echo   3 of 4   Windows reads it back, and every MD5 is checked
echo ============================================================
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "try { Expand-Archive -LiteralPath '%ZIP%' -DestinationPath '%UNPACK%' -Force } catch { Write-Host ('Expand-Archive FAILED: ' + $_.Exception.Message); exit 1 };" ^
  "Write-Host 'Expand-Archive: OK';" ^
  "$man = Join-Path '%UNPACK%' 'manifest.tsv';" ^
  "if (-not (Test-Path $man)) { Write-Host 'manifest.tsv is not in the archive.'; exit 1 };" ^
  "$rows = Get-Content $man ^| Select-Object -Skip 1;" ^
  "$same = 0; $diff = 0;" ^
  "foreach ($r in $rows) {" ^
  "  if ($r.Trim() -eq '') { continue };" ^
  "  $c = $r -split \"`t\";" ^
  "  $rel = $c[0].TrimStart('/') -replace '/','\';" ^
  "  $p = Join-Path '%UNPACK%' $rel;" ^
  "  if (-not (Test-Path $p)) { Write-Host ('  MISSING after unpack: ' + $c[0]); $diff++; continue };" ^
  "  $h = (Get-FileHash -LiteralPath $p -Algorithm MD5).Hash.ToLower();" ^
  "  if ($h -eq $c[2].ToLower()) { $same++ } else { Write-Host ('  DIFFERENT: ' + $c[0]); $diff++ } };" ^
  "Write-Host ('assets: ' + $same + ' identical, ' + $diff + ' different');" ^
  "if ($diff -eq 0 -and $same -gt 0) { Write-Host 'OK - the package round-trips through Windows own ZIP reader.' } else { Write-Host 'NOT OK - do not send this package.'; exit 1 }"

echo.
echo ============================================================
echo   4 of 4   the SnakeBite mod
echo ============================================================
echo A .mgsv is published under your name, so it needs two things a
echo folder does not. Press Enter at either one to skip the .mgsv.
echo.
set "MODNAME="
set "MODAUTHOR="
set /p "MODNAME=Mod name    : "
if "%MODNAME%"=="" goto :nomgsv
set /p "MODAUTHOR=Your name   : "

set "MGSV=%~dp0_deliveries\%MODNAME%.mgsv"
if exist "%MGSV%" del /q "%MGSV%"
"%EXE%" --mgsvpackage "%MGSV%" --mgsvmeta "name=%MODNAME%;author=%MODAUTHOR%" 2>&1 ^
    | findstr /i "mgsvpackage"

if exist "%MGSV%" (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
      "$z=[System.IO.Compression.ZipFile]::OpenRead('%MGSV%');" ^
      "$e=$z.Entries ^| Where-Object { $_.FullName -eq 'metadata.xml' };" ^
      "if (-not $e) { Write-Host 'metadata.xml is missing - that is wrong on its own.'; $z.Dispose(); exit 1 };" ^
      "$r=New-Object IO.StreamReader($e.Open()); $x=[xml]$r.ReadToEnd(); $r.Close(); $z.Dispose();" ^
      "Write-Host ('metadata.xml parses. Name=' + $x.ModEntry.Name + '  Version=' + $x.ModEntry.Version + '  Author=' + $x.ModEntry.Author);" ^
      "Write-Host ('QarEntries: ' + @($x.ModEntry.QarEntries.QarEntry).Count + '   SBVersion: ' + $x.ModEntry.SBVersion.Version + '   MGSVersion: ' + $x.ModEntry.MGSVersion.Version)"
) else (
    echo.
    echo No .mgsv was written - the reason is the line above. If it says
    echo files live inside a container, use mod_package.zip for those.
)
goto :done

:nomgsv
echo Skipped.

:done
echo.
echo Wrote whatever succeeded into _deliveries\ :
echo    mod_package.zip          the plain package
echo    mod_contents.tsv         what was in the folder, and whether each
echo                             replacement is the copy the index hands out
echo    mod_package_check\       the unpacked copy, safe to delete
echo    ^<name^>.mgsv              the SnakeBite mod, if you named one
echo.
pause
