@echo off
rem Starts the game logging the code behind the stage's grass draws (vertex shader B016C3C1) to the log
cd /d "%~dp0out\build\release"
set "XBE_DRAW_CALLERS=B016C3C1"
start "" CMR2005.exe
