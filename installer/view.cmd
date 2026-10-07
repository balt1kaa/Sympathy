@echo off
call "%~dp0stand.cmd" >nul
set SYMPATHY_PAYLOAD_EXE=%~dp0..\out\Sympathy Setup.exe
cd /d "%~dp0tauri"
npx.cmd tauri dev
