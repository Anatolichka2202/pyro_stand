# pyro_stand — контекст проекта

## Что это

Qt6/C++20 десктоп-приложение для контроля пуска ракеты (тестовый стенд).
Считывает поток с COM-порта (1 байт/мс = 8-битная маска каналов), сверяет с циклограммой,
выдаёт таблицу отклонений срабатывания пиропатронов.

Основная платформа — Windows. Linux поддерживается полностью.
Интерфейс на русском языке.

---

## Состояние проекта (актуально на 2026-06)

Проект в стабильном рабочем состоянии. Все основные фичи реализованы:
- Загрузка циклограммы, отправка на БЦВМ по UDP
- Обратный отсчёт, отслеживание каналов в реальном времени
- Таблица событий с результатами (ОК / НЕ СРАБОТАЛО / откл. мс)
- Структурированный лог с временны́ми метками из циклограммы (не системных часов)
- Демо-режим без железа (`pyro_demo`)
- Headless-сервис с JSON-выводом (`pyro_service`)
- Полный тестовый стенд (unit, integration, GUI-тесты, CI на GitHub Actions)

---

## Архитектура (ключевое)

Два слоя, соединённых только через Qt signals/slots:

**`Stand`** (`stand.h/cpp`) — вся бизнес-логика.
- Constructor: `Stand(QObject*, portName, unique_ptr<ISerialPort>, SessionLogger*)`
- Пустой `portName` → mock-режим (нет serial, нет UDP, нет ping)
- `readingThread` — горячий путь: 1 байт/мс, 8-битная маска, фазовый автомат

**`MainWindow`** (`mainwindow.h/cpp`) — только UI, никакой бизнес-логики.
- Два конструктора: продакшн (создаёт Stand сам) и тест/демо (Stand инжектируется)

**`SessionLogger`** — лог в `logDir/session_YYYY-MM-DD_HH-MM-SS/pyro_stand.log`.
- Временны́е метки = `SET_UTC_TIME + absoluteIndex_ms` (НЕ системное время).
- `setTimeBase(QTime)` вызывается после разбора циклограммы.

**`ISerialPort`** — интерфейс порта. Реализации:
- `RealSerialPort` — QSerialPort (создаётся лениво внутри readingThread)
- `MockSerial` — для тестов и демо

---

## Жёсткие ограничения (не менять)

- **Формат циклограммы** (`cyclogram.ini`) — контракт с железом, заморожен.
- **Маппинг каналов** — захардкожен в конструкторе `Stand`, не трогать:
  - `IGNITE_PYRO_CANDLES_ENGINES_9_TO_12` → `0x03` → каналы 1, 2
  - `IGNITE_PYRO_CANDLES_ENGINES_1_TO_8`  → `0x1C` → каналы 3, 4, 5
  - `CLOSE_MAIN_VALVES_ENGINES_9_TO_12`   → `0x60` → каналы 6, 7
  - `LIFT_OFF_CONTACT`                    → `0x80` → канал 8 (синхро)
- Канал 8 — синхросигнал, его абсолютный индекс = T0 для расчёта отклонений.

---

## Тесты

```bash
cd build && ctest --output-on-failure -V
```

| Цель | Покрывает |
|------|-----------|
| `tst_analysis` | `Stand::analyzeEvents()` — чистая функция |
| `tst_cyclogram` | парсинг cyclogram.ini |
| `tst_reading` | readingThread + MockSerial (срабатывания, пропуски, обрыв) |
| `tst_logformat` | SessionLogger: метки времени, папки сессий |
| `tst_gui` | GUI-сценарии через инжекцию Stand (offscreen) |

CI: GitHub Actions, `ubuntu-22.04`, Qt6, каждый push/PR.

---

## Ключевые константы (stand.h private)

| Константа | Значение |
|-----------|----------|
| `BAUD_RATE` | 115200 |
| `BCVM_IP` | `192.168.17.246` |
| `UDP_PORT` | 4000 |
| `SYNC_MASK` | `0x80` |
| `FLIGHT_SAFETY_MS` | 1000 |
| `FAIL_MARGIN_MS` | 5 |

---

## Потоки

- Qt main thread — весь UI и методы Stand из UI-слотов
- `readingThread` (std::thread) — только emit сигналов, никаких виджетов
- `m_events` защищён `QMutex`
- `m_running` / `m_stopped` — `std::atomic<bool>`
- `flightComplete` подключён через `Qt::QueuedConnection` → исполняется в main thread
