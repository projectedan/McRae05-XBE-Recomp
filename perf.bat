@echo off
rem Starts the game with the performance counters on: each second the log (out\build\release\logs) gets frame
rem times (median, p95, p99, worst), the GPU, renderer and game time per frame, and every frame over 25 ms with
rem what it did. F3 captures as usual.
cd /d "%~dp0out\build\release"
set "XBE_CAPTURE_DIR=capture"
set "XBE_PERF=1"
rem CMR keeps its measured frame time, in seconds, as a float at 5F426C
set "XBE_PERF_DT=5F426C"
start "" CMR2005.exe
