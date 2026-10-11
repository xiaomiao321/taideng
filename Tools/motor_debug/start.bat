@echo off
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe (
    python -m venv .venv
    if errorlevel 1 goto fail
)
.venv\Scripts\python.exe -c "import serial, tkinter" >nul 2>&1
if errorlevel 1 (
    .venv\Scripts\python.exe -m pip install -r requirements.txt
    if errorlevel 1 goto fail
)
.venv\Scripts\python.exe app.py %*
if errorlevel 1 goto fail
exit /b 0
:fail
echo Startup failed. Check Python, tkinter and network availability.
pause
exit /b 1
