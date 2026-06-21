@echo off
REM build.bat — сборка pyro_stand и/или pyro_demo (Windows)
REM Использование:
REM   build.bat            — только pyro_stand (production)
REM   build.bat demo       — только pyro_demo (демо без железа)
REM   build.bat all        — pyro_stand + pyro_demo
REM
REM Требует: Qt6, CMake, Visual Studio (или MinGW) в PATH

setlocal
set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
set MODE=%1
if "%MODE%"=="" set MODE=stand

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

if "%MODE%"=="demo" (
    echo === Настройка: pyro_demo ^(демо-режим^) ===
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON || goto :error
    echo === Сборка: pyro_demo ===
    cmake --build . --target pyro_demo --config Release || goto :error
    echo.
    echo Готово: %BUILD_DIR%\pyro_demo.exe
    echo Запуск: %BUILD_DIR%\pyro_demo.exe
) else if "%MODE%"=="all" (
    echo === Настройка: pyro_stand + pyro_demo ===
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON || goto :error
    echo === Сборка: все цели ===
    cmake --build . --target pyro_stand --config Release || goto :error
    cmake --build . --target pyro_demo  --config Release || goto :error
    echo.
    echo Готово:
    echo   %BUILD_DIR%\Release\pyro_stand.exe
    echo   %BUILD_DIR%\Release\pyro_demo.exe
) else (
    echo === Настройка: pyro_stand ^(production^) ===
    cmake .. -DCMAKE_BUILD_TYPE=Release || goto :error
    echo === Сборка: pyro_stand ===
    cmake --build . --target pyro_stand --config Release || goto :error
    echo.
    echo Готово: %BUILD_DIR%\Release\pyro_stand.exe
    echo Запуск: %BUILD_DIR%\Release\pyro_stand.exe --port COM7
)

exit /b 0
:error
echo.
echo ОШИБКА СБОРКИ — см. вывод выше
exit /b 1
