@echo off
rem Online play: join a game someone is hosting (README.md, "Online play").
cd /d "%~dp0"
set "ADDR=%~1"
if "%ADDR%"=="" set /p "ADDR=The host's address (IP or name, :PORT if not 7777): "
if "%ADDR%"=="" exit /b 1
start "" RampageRE.exe --join %ADDR%
