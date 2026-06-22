# pyro_stand

Qt6/C++20 десктоп-приложение для контроля пуска ракеты. Считывает поток данных с отдельного датчика срабатывания через COM-порт (1 байт/мс, 8-битная маска каналов пиросистем), сравнивает факт и время срабатывания с заранее загруженной циклограммой и формирует протокол отклонений.

Интерфейс на русском языке. Основная платформа — Windows, Linux поддерживается полностью.

---

## Аппаратная конфигурация

Система состоит из двух независимых аппаратных компонентов.

**Датчик срабатывания (COM-порт)**

Отдельное устройство, физически измеряющее ток в пиротехнических линиях. Посылает ровно 1 байт каждую 1 мс — непрерывный поток, независимый от БЦВМ и PC. Каждый байт — битовая маска активных каналов. Подключается к `COM7` (Windows) или `/dev/ttyUSB0` (Linux), скорость 115200.

**БЦВМ (бортовой компьютер, 192.168.17.246)**

Получает циклограмму от PC по сети (UDP порт 4000 или TFTP порт 69) и выполняет полётное задание, управляя пиротехникой. Данные обратно в PC через COM-порт не передаёт.

```
Датчик срабатывания → COM7/ttyUSB0 → pyro_stand (PC)
БЦВМ (192.168.17.246) ← UDP/TFTP ← pyro_stand (PC)
```

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

## Сборка

### Скрипты быстрой сборки

```bash
# Windows
build.bat           # pyro_stand (продакшн)
build.bat demo      # pyro_demo  (демо без железа)
build.bat all       # оба

# Linux
./build.sh
./build.sh demo
./build.sh all
```

### Сборка вручную

```bash
mkdir build && cd build

# Продакшн GUI (требует COM-порт и БЦВМ)
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target pyro_stand

# Демо-режим — полный GUI без железа (MockSerial со скриптованными событиями)
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=ON
cmake --build . --target pyro_demo
./pyro_demo

# Headless-сервис — JSON в stdout, без GUI (для интеграции/скриптов)
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SERVICE=ON
cmake --build . --target pyro_service
./pyro_service --log-dir /var/log/pyro

# GUI-тесты (offscreen, без железа)
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_GUI_TESTS=ON
cmake --build . --target tst_gui
QT_QPA_PLATFORM=offscreen ./tst_gui
```

### Таблица цели сборки

| Цель CMake | Флаг | Назначение |
|---|---|---|
| `pyro_stand` | — | Продакшн GUI; нужен COM-порт и БЦВМ |
| `pyro_demo` | `-DBUILD_DEMO=ON` | Полный GUI, MockSerial, без железа |
| `pyro_service` | `-DBUILD_SERVICE=ON` | Headless, JSON stdout |
| `tst_gui` | `-DBUILD_GUI_TESTS=ON` | GUI-тесты offscreen |
| `tst_analysis` `tst_cyclogram` `tst_reading` `tst_logformat` | — | Unit/integration тесты |

---

## Использование

### Шаг 1 — подготовить циклограмму

Поместить файл `cyclogram.ini` рядом с исполняемым файлом. Формат описан в разделе [Циклограмма](#циклограмма) ниже.

### Шаг 2 — запустить приложение

```bash
# Windows
pyro_stand.exe

# Linux
./pyro_stand
```

При успешном запуске:
- В строке состояния появится зелёная метка COM-порта.
- Таблица событий заполнится строками из циклограммы.
- Фаза: **Загружено**.

### Шаг 3 — установить время старта

Ввести плановое UTC-время старта в поле «Время старта» и нажать **Установить время старта**. Это перезаписывает строку `START_UTC_TIME` в файле циклограммы.

### Шаг 4 — загрузить на борт

Нажать **Загрузить на борт**:
1. Приложение проверяет доступность БЦВМ (ICMP ping на 192.168.17.246).
2. Отправляет циклограмму на БЦВМ (UDP или TFTP — выбирается в комбобоксе).
3. Запускает поток чтения COM-порта.
4. Фаза переходит в **Обратный отсчёт**.

Выбор протокола передачи:
- **UDP** — быстрее, без гарантии доставки.
- **TFTP** — подтверждённая доставка (RFC 1350); drain буфера COM выполняется после финального ACK, что даёт лучшую точность синхронизации.

### Шаг 5 — наблюдение за полётом

- Таймер ведёт обратный отсчёт до T0, затем время полёта относительно фактического сигнала «Контакт подъёма» (канал 8).
- При срабатывании каждого отслеживаемого канала строка таблицы обновляется: показывается абсолютный тик.
- Визуальный таймлайн в нижней части окна отображает прошедшие и предстоящие события.

### Шаг 6 — анализ после полёта

После прохождения последнего события (~5 с запаса) поток автоматически завершается, запускается анализ:

- Статус каждого события: **ОК** / **НЕ СРАБОТАЛО** / **ОПОЗДАНИЕ**.
- Отклонение в мс относительно фактического T0 (канал 8 самовычитает любую погрешность drain).
- Цветовая индикация: зелёный (0 мс), жёлтый (1–5 мс), красный (> 5 мс).
- Строка LIFT_OFF_CONTACT: зелёный фон, оранжевое отклонение = опоздание/опережение планового T0.

**Кнопка «СТОП»** прерывает полёт досрочно: отправляет байт `0x63` на БЦВМ (UDP 4000), останавливает поток чтения, фиксирует все ещё не сработавшие события как «НЕ СРАБОТАЛО».

**Кнопка «Сброс»** возвращает приложение в состояние «Загружено» для повторного прогона без перезапуска.

---

## Циклограмма

Файл `cyclogram.ini` — текстовый, кодировка UTF-8.

```ini
SET_UTC_TIME   = 10:00:00      # лабораторные часы в момент начала потока (tick=0)
START_UTC_TIME = 10:00:30      # плановый T0 (время старта)

# Ключ = время в мс относительно T0 (отрицательное = до отрыва)
# Комментарий после # становится описанием события в таблице

IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -8000 # Зажигание пиросвечей двигателей НК-3А №9–12
IGNITE_PYRO_CANDLES_ENGINES_1_TO_8  = -7980 # Зажигание пиросвечей двигателей НК-3АК №1–8
LIFT_OFF_CONTACT                     = 0    # Контакт подъёма, начало движения
CLOSE_MAIN_VALVES_ENGINES_9_TO_12   = 20000 # Закрытие главных клапанов двигателей 9–12
```

Правила:
- `SET_UTC_TIME` и `START_UTC_TIME` — обязательные заголовочные строки.
- Все остальные строки — события. Значение — миллисекунды относительно T0.
- Только ключи из таблицы маппинга каналов (ниже) отслеживаются аппаратно; остальные отображаются справочно.
- Формат и имена ключей фиксированы контрактом с БЦВМ. Не изменять.

### Маппинг каналов (жёсткий, в коде)

| Ключ | Битовая маска | Каналы |
|---|---|---|
| `IGNITE_PYRO_CANDLES_ENGINES_9_TO_12` | `0x03` | 1, 2 |
| `IGNITE_PYRO_CANDLES_ENGINES_1_TO_8` | `0x1C` | 3, 4, 5 |
| `CLOSE_MAIN_VALVES_ENGINES_9_TO_12` | `0x60` | 6, 7 |
| `LIFT_OFF_CONTACT` | `0x80` | 8 (синхро-T0) |

---

## Тесты

```bash
cd build

# Unit / integration тесты (без GUI, без железа)
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target tst_analysis tst_cyclogram tst_reading tst_logformat
ctest --output-on-failure

# GUI-тесты (offscreen, без дисплея)
cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_GUI_TESTS=ON
cmake --build . --target tst_gui
QT_QPA_PLATFORM=offscreen ./tst_gui
```

| Цель | Что тестирует |
|---|---|
| `tst_analysis` | `Stand::analyzeEvents()` — чистая функция расчёта отклонений |
| `tst_cyclogram` | Парсинг `cyclogram.ini`, граничные случаи |
| `tst_reading` | `readingThread` с `MockSerial`: срабатывания, пропуски, обрыв порта |
| `tst_logformat` | `SessionLogger`: метки времени, директория сессии, заголовок |
| `tst_gui` | GUI-сценарии: успешный полёт, пропущенное событие, СТОП |

CI запускается на каждый push/PR (GitHub Actions, Ubuntu 22.04 + Qt6).

---

## Архитектура (кратко)

Два слоя, связанных только через Qt signals/slots:

**`Stand`** (`stand.h/cpp`) — вся бизнес-логика. Горячий путь — `readingThread` (`std::thread`): читает 1 байт/мс, интерпретирует как 8-битную маску каналов, ведёт машину фаз `Idle → Loaded → Countdown → Running → Completed/Stopped`, эмитирует сигналы в GUI-поток. Функция `analyzeEvents()` — статическая и чистая, тестируется без железа.

**`MainWindow`** (`mainwindow.h/cpp`) — только UI. Два конструктора: продакшн (`portOverride, logDir`) и инжекционный для тестов/демо (`unique_ptr<Stand>, cyclogramPath`). Все обновления UI — через слоты сигналов Stand.

Подробная техническая документация: [`docs/TECHNICAL.md`](docs/TECHNICAL.md).

---

## Структура проекта

```
pyro_stand/
├── stand.cpp / stand.h          # бизнес-логика
├── mainwindow.cpp / mainwindow.h# UI
├── timeline_widget.cpp/.h       # горизонтальный таймлайн событий
├── session_logger.h/.cpp        # структурированный лог с тиковыми метками
├── serial_port.h                # ISerialPort — интерфейс порта
├── real_serial_port.h           # RealSerialPort (обёртка QSerialPort)
├── mocks/mock_serial.h          # MockSerial для тестов и демо
├── types.h                      # Phase, EventRow, TimerState, NextEventInfo
├── platform.h                   # DEFAULT_SERIAL_PORT (COM7 / /dev/ttyUSB0)
├── main.cpp                     # точка входа (продакшн)
├── main_demo.cpp                # точка входа (демо)
├── main_service.cpp             # точка входа (headless-сервис)
├── cyclogram.ini                # рабочая циклограмма
├── cyclogram_full.ini           # полная циклограмма (справочная)
├── tests/                       # QtTest: tst_analysis, tst_cyclogram, tst_reading, tst_logformat, tst_gui
├── docs/                        # техническая документация (английский)
├── deploy/                      # systemd unit для Linux
├── build.sh / build.bat         # скрипты сборки
└── .github/workflows/           # CI (GitHub Actions)
```

---

## Linux: деплой как системный сервис

```bash
# Сборка headless-сервиса
cmake .. -DBUILD_SERVICE=ON && cmake --build . --target pyro_service

# Установка
sudo cp pyro_service /usr/local/bin/
sudo cp ../deploy/pyro_stand.service /etc/systemd/system/
sudo systemctl enable --now pyro_stand
```

---

## Лицензия

<!-- TODO: добавить лицензию -->
