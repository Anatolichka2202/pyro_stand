# pyro_stand

Qt6/C++20 десктоп-приложение для контроля пуска ракеты. Считывает поток данных с COM-порта (1 байт/мс, 8-битная маска каналов), сверяет факт срабатывания пиропатронов с циклограммой и формирует протокол отклонений.

Целевая платформа — Windows. Linux поддерживается полностью.

---

## Требования

- Qt 6 (Core, Widgets, Network, SerialPort)
- CMake ≥ 3.16
- C++20-совместимый компилятор (MSVC 2019+ / GCC 10+ / Clang 12+)
- На Linux: пользователь должен быть в группе `dialout`

```bash
sudo usermod -aG dialout $USER
```

---

## Быстрый старт

```bash
# Клонировать
git clone https://github.com/Anatolichka2202/pyro_stand.git
cd pyro_stand

# Сборка (Windows)
build.bat

# Сборка (Linux/macOS)
./build.sh
```

### Варианты сборки

| Команда | Результат |
|---------|-----------|
| `build.sh` / `build.bat` | `pyro_stand` — продакшн GUI, требует COM-порт и БЦВМ |
| `build.sh demo` | `pyro_demo` — полный GUI без железа (MockSerial) |
| `build.sh all` | оба варианта |

Или вручную:

```bash
mkdir build && cd build

# Продакшн
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target pyro_stand

# Демо-режим (без железа)
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON
cmake --build . --target pyro_demo

# Headless-сервис (JSON в stdout)
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVICE=ON
cmake --build . --target pyro_service
```

---

## Запуск

```bash
# Windows (продакшн)
pyro_stand.exe --port COM7 --log-dir C:\logs

# Linux (продакшн)
./pyro_stand --port /dev/ttyUSB0 --log-dir /var/log/pyro

# Демо (без железа, любая платформа)
./pyro_demo
```

---

## Циклограмма (`cyclogram.ini`)

```ini
SET_UTC_TIME   = 10:00:00      # лабораторное время отсчёта (tick=0)
START_UTC_TIME = 10:00:30      # плановое время старта
IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -8000  # мс относительно T0 (отрыва)
LIFT_OFF_CONTACT                     = 0
CLOSE_MAIN_VALVES_ENGINES_9_TO_12   = 20000
```

- `SET_UTC_TIME` — лабораторные часы в момент начала потока (абсолютный индекс 0).
- `START_UTC_TIME` — плановый T0; задаётся через UI кнопкой «Установить время старта».
- Остальные строки — события в миллисекундах относительно отрыва (T0 = 0, до отрыва < 0).
- Комментарий после `#` становится описанием события в таблице.

Формат и маппинг каналов — жёсткий контракт с БЦВМ, не менять.

---

## Архитектура

```
main.cpp / main_demo.cpp / main_service.cpp
    └── MainWindow (mainwindow.h/cpp)      ← только UI, Qt widgets
            └── Stand (stand.h/cpp)        ← вся бизнес-логика
                    ├── ISerialPort        ← интерфейс порта
                    │     ├── RealSerialPort   (QSerialPort)
                    │     └── MockSerial       (mocks/mock_serial.h)
                    └── SessionLogger      ← структурированный лог
```

### Stand

Конструктор: `Stand(QObject*, portName, unique_ptr<ISerialPort>, SessionLogger*)`.

- `portName` пустой → mock-режим: порт не открывается, UDP/ping не делаются.
- `port` не-null → инжектируемый порт (тесты/демо).
- `logger` не-null → пишет лог-файл; null → тихий режим.

Фазы: `Idle → Loaded → Countdown → Running → Completed / Stopped`

**Горячий путь** — `readingThread` (std::thread): читает 1 байт/мс, интерпретирует как 8-битную маску, ведёт `Phase`-машину состояний, эмитит сигналы в GUI-тред через автосоединение Qt.

Канал 8 (`0x80`, `LIFT_OFF_CONTACT`) — сигнал синхронизации. Его абсолютный индекс становится T0 для расчёта отклонений.

### MainWindow

Два конструктора:
- `MainWindow(portOverride, logDir, parent)` — продакшн: создаёт Stand + SessionLogger.
- `MainWindow(unique_ptr<Stand>, cyclogramPath, parent)` — тесты/демо: Stand инжектируется.

### SessionLogger

Лог: `logDir/session_YYYY-MM-DD_HH-MM-SS/pyro_stand.log`

Метки времени — НЕ системные часы. Формула: `SET_UTC_TIME + absoluteIndex_ms`.
Вызвать `setTimeBase(QTime)` после разбора циклограммы.

### Маппинг каналов (жёсткий, в Stand::constructor)

| Ключ | Маска | Каналы |
|------|-------|--------|
| `IGNITE_PYRO_CANDLES_ENGINES_9_TO_12` | `0x03` | 1, 2 |
| `IGNITE_PYRO_CANDLES_ENGINES_1_TO_8` | `0x1C` | 3, 4, 5 |
| `CLOSE_MAIN_VALVES_ENGINES_9_TO_12` | `0x60` | 6, 7 |
| `LIFT_OFF_CONTACT` | `0x80` | 8 (sync) |

---

## Тесты

```bash
cd build

# Unit / integration тесты (без GUI, без железа)
ctest --output-on-failure

# GUI-тесты (offscreen)
cmake .. -DBUILD_GUI_TESTS=ON
cmake --build . --target tst_gui
QT_QPA_PLATFORM=offscreen ./tst_gui
```

| Цель | Что тестирует |
|------|---------------|
| `tst_analysis` | `Stand::analyzeEvents()` — чистая функция, отклонения |
| `tst_cyclogram` | парсинг `cyclogram.ini` |
| `tst_reading` | `readingThread` с `MockSerial`: срабатывания, пропуски, обрыв порта |
| `tst_logformat` | `SessionLogger`: метки времени, папка сессии, заголовок |
| `tst_gui` | GUI-сценарии через `MainWindow(Stand)` в offscreen-режиме |

CI запускается автоматически на каждый push/PR (GitHub Actions, Ubuntu 22.04 + Qt6).

---

## Linux: деплой как сервис

```bash
# Сборка headless-сервиса
cmake .. -DBUILD_SERVICE=ON && cmake --build . --target pyro_service

# Установка
sudo cp pyro_service /usr/local/bin/
sudo cp ../deploy/pyro_stand.service /etc/systemd/system/
sudo systemctl enable --now pyro_stand
```

Подробнее — [`deploy/README.md`](deploy/README.md).

---

## Структура проекта

```
pyro_stand/
├── stand.cpp / stand.h          # бизнес-логика
├── mainwindow.cpp / mainwindow.h# UI
├── timeline_widget.cpp/.h       # горизонтальный таймлайн
├── session_logger.h/.cpp        # структурированный лог
├── serial_port.h                # ISerialPort интерфейс
├── real_serial_port.h           # QSerialPort обёртка
├── mocks/mock_serial.h          # MockSerial для тестов/демо
├── types.h                      # Phase, EventRow, TimerState, ...
├── platform.h                   # DEFAULT_SERIAL_PORT
├── main.cpp                     # точка входа (продакшн)
├── main_demo.cpp                # точка входа (демо)
├── main_service.cpp             # точка входа (headless)
├── cyclogram.ini                # пример циклограммы
├── tests/                       # тесты (QtTest)
├── deploy/                      # systemd unit для Linux
├── build.sh / build.bat         # скрипты сборки
└── .github/workflows/ci.yml     # CI (GitHub Actions)
```
