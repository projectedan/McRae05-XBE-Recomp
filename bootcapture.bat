@echo off
rem Starts the game capturing every 10th frame of its first 1200 (about 20 s): the boot videos and logos, to
rem out\build\release\capture
cd /d "%~dp0out\build\release"
set "XBE_CAPTURE_DIR=capture"
set "XBE_CAPTURE_AT=0:10:120"
start "" CMR2005.exe
