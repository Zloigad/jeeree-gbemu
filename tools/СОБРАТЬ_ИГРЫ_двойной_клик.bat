@echo off
rem ============================================================
rem  СОБРАТЬ ИГРЫ - запуск в один двойной клик (Windows)
rem  Открывает окно "Собери свои игры". Python ставится один раз:
rem  https://www.python.org/downloads/ (все галочки по умолчанию)
rem ============================================================
chcp 65001 >nul
cd /d "%~dp0"

where pyw >nul 2>nul
if %errorlevel%==0 (
  start "" pyw -3 "Собрать_игры.py"
  exit /b 0
)
where py >nul 2>nul
if %errorlevel%==0 (
  start "" py -3 "Собрать_игры.py"
  exit /b 0
)
where pythonw >nul 2>nul
if %errorlevel%==0 (
  start "" pythonw "Собрать_игры.py"
  exit /b 0
)

echo ============================================================
echo  Python не найден :(
echo.
echo  Это быстро (2 минуты, один раз):
echo   1) открой https://www.python.org/downloads/
echo   2) нажми большую жёлтую кнопку Download, установи
echo      со всеми галочками по умолчанию
echo   3) запусти этот файл ещё раз двойным кликом
echo ============================================================
start https://www.python.org/downloads/
pause
