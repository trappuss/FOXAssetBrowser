@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  Make a portable, zip-and-send build of FOX Asset Browser.
REM  Produces FOXAssetBrowser_portable.zip next to itself.
REM ============================================================
cd /d "%~dp0"

set "SRC=build\release"
set "OUT=dist\FOXAssetBrowser"
set "ZIP=FOXAssetBrowser_portable.zip"

if not exist "%SRC%\FOXAssetBrowser.exe" (
  echo [X] Can't find "%SRC%\FOXAssetBrowser.exe" - build first ^(build.bat^).
  pause & exit /b 1
)

echo [1/4] Preparing "%OUT%" ...
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" 2>nul

echo [2/4] Copying app + Qt runtime DLLs + plugin folders + dictionaries ...
copy /y "%SRC%\FOXAssetBrowser.exe" "%OUT%\" >nul
copy /y "%SRC%\*.dll"               "%OUT%\" >nul
for %%D in (platforms styles imageformats iconengines) do (
  if exist "%SRC%\%%D" xcopy /e /i /y /q "%SRC%\%%D" "%OUT%\%%D" >nul
)
if exist "dict" xcopy /e /i /y /q "dict" "%OUT%\dict" >nul

echo [3/4] Bundling the Microsoft C++ runtime ...
for %%R in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
  if exist "%windir%\System32\%%R" copy /y "%windir%\System32\%%R" "%OUT%\" >nul
)

echo [4/4] Zipping ...
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\*' -DestinationPath '%ZIP%' -Force"

if not exist "%ZIP%" (
  echo [X] Zipping failed.
  pause & exit /b 1
)

echo.
echo Done: "%CD%\%ZIP%"
echo Unzip anywhere and run FOXAssetBrowser.exe - fully portable, settings
echo and caches live in data\ beside the exe. Do NOT ship game archives.
pause
