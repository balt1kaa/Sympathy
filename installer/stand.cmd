@echo off
powershell -NoProfile -Command "if (Get-NetTCPConnection -LocalPort 1420 -State Listen -ErrorAction SilentlyContinue) { exit }; Start-Process npx.cmd -ArgumentList 'vite','--port','1420','--strictPort' -WorkingDirectory '%~dp0tauri' -WindowStyle Hidden"
start "" http://localhost:1420/
