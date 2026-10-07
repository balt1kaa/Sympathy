@echo off
setlocal
set MC=C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore
set DST=%MC%\sympathy
set ROOT=%~dp0
set RED=%ROOT%out\payload
set SRC=%RED%\Sympathy.prm

echo Installing: sympathy
echo.

if not exist "%SRC%" (
  echo [FAIL] Not built: %SRC%
  echo        Run ci\build.ps1 first.
  pause
  exit /b 1
)
if not exist "%RED%\avcodec-62.dll" (
  echo [FAIL] FFmpeg runtime missing: %RED%
  pause
  exit /b 1
)

if exist "%DST%" (
  echo Clearing previous install...
  rmdir /S /Q "%DST%"
)

echo Installing plugin...
mkdir "%DST%" 2>nul
copy /Y "%SRC%" "%DST%\sympathy.prm" >nul

echo Installing FFmpeg runtime (LGPL) and licences...
copy /Y "%RED%\*.dll" "%DST%\" >nul
copy /Y "%RED%\*.txt" "%DST%\" >nul

if exist "%DST%\sympathy.prm" if exist "%DST%\avcodec-62.dll" (
  echo.
  echo [OK] Installed to: %DST%
  dir /b "%DST%"
) else (
  echo.
  echo [FAIL] Copy failed - did you run as administrator?
)
echo.
pause
