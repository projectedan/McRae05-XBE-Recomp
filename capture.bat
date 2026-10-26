@echo off
rem Starts the game with frame capture on: press F3 in game to save the next frame to out\build\release\capture
cd /d "%~dp0out\build\release"
set "XBE_CAPTURE_DIR=capture"
start "" CMR2005.exe
