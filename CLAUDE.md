# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`pyro_stand` is a Qt6/C++20 cross-platform (Windows primary, Linux supported) desktop application for a rocket launch test stand. It monitors pyrotechnic events during a launch sequence by reading a 1-byte-per-millisecond bitmask stream from a serial port and comparing observed channel firing times against a pre-loaded cyclogram (timeline of expected events).

The interface language is Russian.

## Build Targets

```bash
mkdir build && cd build

# Production GUI (COM port + БЦВМ required)
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .

# Demo mode — full GUI, no hardware (MockSerial with scripted events)
cmake .. -DBUILD_DEMO=ON
cmake --build . --target pyro_demo
./pyro_demo

# Headless service — JSON stdout, no GUI (for integration/scripting)
cmake .. -DBUILD_SERVICE=ON
cmake --build . --target pyro_service

# GUI simulation tests (offscreen, no hardware)
cmake .. -DBUILD_GUI_TESTS=ON
cmake --build . --target tst_gui
QT_QPA_PLATFORM=offscreen ./tst_gui

# Unit / integration tests (always built)
cmake --build . --target tst_analysis tst_cyclogram tst_reading
ctest
```

Requires Qt6 (Core, Widgets, Network, SerialPort) and CMake ≥ 3.16.

## Architecture

The application has two layers connected exclusively via Qt signals/slots:

**`Stand`** (`stand.h/cpp`) — all business logic:
- Constructor signature: `Stand(QObject*, portName, unique_ptr<ISerialPort>, SessionLogger*)`.
  - `portName` empty → mock/test mode: no serial open, no BCVM ping/UDP in `sendToBoard()`.
  - `port` non-null → injected mock (tests/demo); null → creates `RealSerialPort` lazily in `readingThread`.
  - `logger` non-null → writes structured log to file; null → silent (demo/tests).
- `loadCyclogram(filePath)` parses `cyclogram.ini`; empty `filePath` → searches in `applicationDirPath()`.
- `sendToBoard()` pings БЦВМ via ICMP, sends cyclogram as UTF-8 UDP datagram to port 4000, then spawns `readingThread`. In mock mode (empty portName), skips ping + UDP.
- `readingThread` is the hot path: reads 1 byte/ms, interprets each byte as bitmask of 8 channels, emits timer/event signals every 1000 bytes (≈1 s), drives `Phase` state machine: Idle → Loaded → Countdown → Running → Completed/Stopped.
- Channel 8 (`0x80`, "Контакт подъёма") is the sync signal. Its absolute byte-index becomes the time base for post-flight deviation analysis in `performAnalysis`.
- `stop()` transitions to Phase::Stopped and sends `0x63` via UDP to БЦВМ.
- `startReadingForTest(ms)` launches `readingThread` directly, skipping UDP/ping — used by unit tests.
- `analyzeEvents()` is a pure static function — fully testable without hardware.

**`MainWindow`** (`mainwindow.h/cpp`) — pure UI:
- Two constructors:
  - `MainWindow(QWidget*)` — production: creates `Stand` + `SessionLogger` internally using `DEFAULT_SERIAL_PORT`.
  - `MainWindow(unique_ptr<Stand>, cyclogramPath, QWidget*)` — test/demo: caller provides pre-configured `Stand`; no file logger.
- Internal structure: `setupUI()` builds widgets, `connectStand()` wires all Stand signals → slots/lambdas, `finalizeInit(cyclogramPath)` calls `loadCyclogram` and syncs initial UI state.
- `m_displayEvents` holds only events with `hasChannels == true`.
- Key widget `objectName`s for tests: `timerLabel`, `phaseLabel`, `nextEventLabel`, `summaryLabel`.
- Owns `m_logger` (destroyed after `m_stand` — reverse declaration order ensures join-before-close).

**`types.h`** — shared types: `Phase` enum, `EventRow` struct, `ChannelMapping` struct, `TimerState` struct, `NextEventInfo` struct.

**`serial_port.h`** — `ISerialPort` interface (open/close/isOpen/waitForReadyRead/read/hasError/errorString/clearBuffers).

**`real_serial_port.h`** — `RealSerialPort` wraps `QSerialPort`, created lazily in `readingThread` to satisfy Qt thread affinity.

**`mocks/mock_serial.h`** — `MockSerial` for tests/demo:
- `fireAt(tick, mask)` — schedule a byte value at a given tick.
- `dropAfter(tick)` — simulate port disconnect.
- `setRealtime(true)` — 1 ms/byte sleep for human-paced demo playback.

**`session_logger.h/cpp`** — structured append-only log. Log format: `HH:mm:ss.zzz | LEVEL | message`.
- **Timestamps are NOT wall-clock** — derived as `SET_UTC_TIME.addMSecs(absoluteIndex)`.
- `setTimeBase(QTime)` must be called after parsing `SET_UTC_TIME` from the cyclogram.
- `log(level, msg, tickMs=0)` — `tickMs` is the COM stream absolute byte index.
- Creates `logDir/session_YYYY-MM-DD_HH-MM-SS/pyro_stand.log` per session (wall clock used only for folder naming).
- `Stand` writes to the logger directly (not via `logMessage` signal → MainWindow path) to preserve tick-accurate timestamps.

**`platform.h`** — `DEFAULT_SERIAL_PORT`: `"COM7"` on Windows, `"/dev/ttyUSB0"` on Linux.

**`timeline_widget.h/cpp`** — horizontal timeline: event dots, playhead, T0 marker. Methods: `setEvents()`, `markEventFired(id, status)`, `setPlayheadMs()`, `reset()`.

## Cyclogram Format (`cyclogram.ini`)

```ini
SET_UTC_TIME = 10:00:00      # laboratory clock reference
START_UTC_TIME = 10:00:30    # target launch time (UTC)
EVENT_KEY = -8000            # milliseconds relative to lift-off (T0)
EVENT_KEY = 0                # T0 = lift-off contact
EVENT_KEY = 20000 # Optional inline comment becomes EventRow::description
```

- `SET_UTC_TIME` and `START_UTC_TIME` are mandatory control lines; all other lines are events.
- Time values are ms relative to lift-off (T0 = 0); negative = pre-launch.
- The inline comment after `#` becomes the event description in the UI table.
- `Stand::setStartTimeFromUI` rewrites only the `START_UTC_TIME` line in-place.
- **The cyclogram format and channel mapping are OFF-LIMITS for modification** — the mapping is hardcoded in Stand's constructor and the cyclogram file format is fixed by the hardware interface contract.

## Channel Mapping

Hardcoded in `Stand`'s constructor (`stand.cpp`), not in the INI file:

| Key | Bitmask | Channels |
|-----|---------|----------|
| `IGNITE_PYRO_CANDLES_ENGINES_9_TO_12` | `0x03` | 1, 2 |
| `IGNITE_PYRO_CANDLES_ENGINES_1_TO_8` | `0x1C` | 3, 4, 5 |
| `CLOSE_MAIN_VALVES_ENGINES_9_TO_12` | `0x60` | 6, 7 |
| `LIFT_OFF_CONTACT` | `0x80` | 8 (sync) |

Only events whose key appears in this mapping have `hasChannels = true` and are tracked/displayed.

## Constants (`stand.h` private)

| Constant | Value | Purpose |
|----------|-------|---------|
| `DEFAULT_PORT` | `DEFAULT_SERIAL_PORT` from `platform.h` | COM7 / /dev/ttyUSB0 |
| `BAUD_RATE` | `115200` | Serial baud rate |
| `BCVM_IP` | `"192.168.17.246"` | Onboard computer IP |
| `UDP_PORT` | `4000` | UDP port for cyclogram + STOP command |
| `SYNC_MASK` | `0x80` | Channel 8 bitmask (lift-off contact) |
| `FLIGHT_SAFETY_MS` | `1000` | Extra ms after last cyclogram event before thread stops |
| `FAIL_MARGIN_MS` | `5` | Ms after expected event time before marking as failed |
| `SERIAL_RETRIES` | `3` | Attempts to open COM port at startup |

## Threading Model

- Qt main thread owns all UI and `Stand` methods called from UI slots.
- `readingThread` is `std::thread` — may only emit signals (never touch Qt widgets). Auto-connection queues signals to main thread.
- `m_events` protected by `m_eventsMutex` (QMutex).
- `m_running` / `m_stopped` are `std::atomic<bool>`.
- `flightComplete` is connected with `Qt::QueuedConnection` so `completeFlight()` executes in main thread after worker exits.
- `RealSerialPort` (wraps QSerialPort) is created inside `readingThread` — satisfies Qt's thread affinity requirement.
- `MainWindow` destructor order: `m_stand` declared after `m_logger` → destroyed first → joins worker thread → then `m_logger` closes file.

## Cross-Platform Notes

- **Windows**: primary target. `PYRO_WINDOWS` compile def. Serial port: `COM7`.
- **Linux**: fully supported. `PYRO_LINUX` compile def. Serial port: `/dev/ttyUSB0`. User needs `dialout` group (`sudo usermod -aG dialout $USER`). Systemd unit in `deploy/pyro_stand.service`.
- `real_serial_port.h` wraps `QSerialPort` identically on both platforms.
- Demo mode (`pyro_demo`) runs identically on Linux/Windows — no hardware needed.
- GUI tests run headless via `QT_QPA_PLATFORM=offscreen` on both platforms / CI.

## Testing Without Hardware

Three levels:

1. **Unit/integration tests** (`tst_analysis`, `tst_cyclogram`, `tst_reading`) — use `MockSerial` injection, no QApplication, no display needed.

2. **GUI simulation tests** (`tst_gui`, `BUILD_GUI_TESTS=ON`) — three scenarios via `MainWindow(unique_ptr<Stand>, path)` injection constructor:
   - Full flight success → all events `✓ ОК`, summary `4 ✓`
   - Missed event → row shows `✗ НЕ СРАБОТАЛО`
   - СТОП mid-flight → phase Stopped, timer shows `СТОП`
   Run with: `QT_QPA_PLATFORM=offscreen ./tst_gui`

3. **Demo mode** (`pyro_demo`, `BUILD_DEMO=ON`) — interactive GUI with scripted 5 s countdown + 4 pyro events. `MockSerial::setRealtime(true)` gives 1 ms/byte pacing. No hardware, no BCVM.

## Headless Service (`pyro_service`, `BUILD_SERVICE=ON`)

`main_service.cpp` — no GUI, outputs JSON lines to stdout. CLI: `--log-dir <path>`. First output line reports the log file path. Intended for integration with external monitoring systems.
