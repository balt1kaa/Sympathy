@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\ci\build.ps1"
