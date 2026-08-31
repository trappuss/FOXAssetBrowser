@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
:: Snapshot the source tree into .Backups BEFORE a build so a bad edit /
:: refactor / truncation can always be recovered. Keeps the newest 20.
set "DEST=.Backups"

for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "STAMP=%%i"
if "%STAMP%"=="" set "STAMP=nostamp"
set "OUT=%DEST%\src_%STAMP%"

if not exist "%DEST%" mkdir "%DEST%" 2>nul
mkdir "%OUT%" 2>nul

robocopy "src" "%OUT%\src" /E /NFL /NDL /NJH /NJS /NP >nul
copy /y "CMakeLists.txt"    "%OUT%\" >nul 2>&1
copy /y "CMakePresets.json" "%OUT%\" >nul 2>&1
copy /y "vcpkg.json"        "%OUT%\" >nul 2>&1
copy /y "*.bat"             "%OUT%\" >nul 2>&1

echo Source snapshot: %OUT%

set /a keep=20, n=0
for /f "delims=" %%d in ('dir /b /ad /o-d "%DEST%\src_*" 2^>nul') do (
    set /a n+=1
    if !n! gtr %keep% rd /s /q "%DEST%\%%d" 2>nul
)
endlocal
