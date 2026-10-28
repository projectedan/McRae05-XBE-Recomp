@echo off
rem Starts the game capturing every 2nd frame of its first 240 (about 4 s): the boot videos and logos, to
rem out\build\release\capture
cd /d "%~dp0out\build\release"
set "XBE_CAPTURE_DIR=capture"
set "XBE_CAPTURE_AT=0:2:120"
start "" CMR2005.exe
