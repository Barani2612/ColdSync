@echo off
setlocal
cd /d "%~dp0backend"
if not exist .venv\Scripts\python.exe (
  py -3.13 -m venv .venv 2>nul
  if not exist .venv\Scripts\python.exe py -3.12 -m venv .venv 2>nul
  if not exist .venv\Scripts\python.exe py -3.11 -m venv .venv 2>nul
)
if not exist .venv\Scripts\python.exe (
  echo Python 3.11, 3.12, or 3.13 was not found. Install Python from https://www.python.org/downloads/ and enable the py launcher.
  pause
  exit /b 1
)
call .venv\Scripts\activate.bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
if not exist .env copy .env.example .env >nul
echo.
echo IMPORTANT: Edit backend\.env and set the ESP2 IP and Firebase credentials before expecting live/cloud data.
echo ESP2 IP is printed in Arduino Serial Monitor after it connects to test123.
echo Open http://127.0.0.1:5000/login after the server starts.
echo.
python app.py
pause
