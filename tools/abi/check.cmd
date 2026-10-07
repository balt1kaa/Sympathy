@echo off
setlocal
if "%~1"=="" (
  echo usage: check.cmd "<Premiere Pro SDK>\Examples\Headers"
  exit /b 2
)
cd /d "%~dp0"
del abi_own.exe abi_adobe.exe own.txt adobe.txt 2>nul
cl /nologo /EHsc /std:c++17 abi_dump.cpp /Fe:abi_own.exe /Fo:abi_own.obj
if not exist abi_own.exe ( echo [FAIL] could not build against premiere_api.h & exit /b 1 )
cl /nologo /EHsc /std:c++17 /DWITH_ADOBE_SDK /DPRWIN_ENV /DMSWindows /I"%~1" abi_dump.cpp /Fe:abi_adobe.exe /Fo:abi_adobe.obj
if not exist abi_adobe.exe ( echo [FAIL] could not build against the SDK & exit /b 1 )
.\abi_own.exe > own.txt
.\abi_adobe.exe > adobe.txt
fc own.txt adobe.txt >nul
if errorlevel 1 (
  echo [FAIL] premiere_api.h differs from the SDK:
  fc own.txt adobe.txt
  exit /b 1
)
for /f %%n in ('find /c /v "" ^< own.txt') do echo [OK] %%n sizes, offsets and values identical
del abi_own.* abi_adobe.* own.txt adobe.txt
