@echo off
setlocal
cd /d "%~dp0"
if not exist "RoachPet_DoubleClick.exe" call Build.cmd
if not exist "RoachPet_DoubleClick.exe" exit /b 1
start "" "%~dp0RoachPet_DoubleClick.exe"
