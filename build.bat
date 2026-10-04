@echo off
REM Qt 6 и Ninja должны быть доступны через PATH/CMAKE_PREFIX_PATH.
setlocal
set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build\stand
cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%BUILD_DIR%" --target pyro_stand || exit /b 1
where windeployqt >nul 2>nul
if %ERRORLEVEL%==0 windeployqt --release --no-translations "%BUILD_DIR%\pyro_stand.exe" || exit /b 1
echo Готово: %BUILD_DIR%\pyro_stand.exe
echo Запуск: %BUILD_DIR%\pyro_stand.exe --debug-socket kasupp-debug-events-v1
