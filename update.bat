@echo off
rem Fetches the latest changes from GitHub, then builds. Your settings, captures and game files are left alone.
cd /d "%~dp0"
echo.
echo   Updating from GitHub ...
git pull --ff-only
if errorlevel 1 (
    echo.
    echo   UPDATE FAILED. Read the message above; nothing was built.
    echo.
    pause
    exit /b 1
)
git log -1 --format="  Now at %%h: %%s"
echo.
call build.bat
