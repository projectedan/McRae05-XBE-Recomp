@echo off
rem Starts the game recording its sound: the first 3 minutes go to out\build\release\audio_rec.wav
rem (the six speaker channels, two effect sends and the stereo output, for measuring the mix). F3 captures as usual.
cd /d "%~dp0out\build\release"
set "XBE_CAPTURE_DIR=capture"
set "XBE_AUDIO_REC=180"
start "" CMR2005.exe
