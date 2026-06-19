# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`pyro_stand` is a Qt6/C++20 Windows desktop application for a rocket launch test stand. It monitors pyrotechnic events during a launch sequence by reading a 1-byte-per-millisecond bitmask stream from a serial port (COM7, 115200 baud) and comparing observed channel firing times against a pre-loaded cyclogram (timeline of expected events).

The interface language is Russian.

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Requires Qt6 (Core, Widgets, Network, SerialPort) and CMake ≥ 3.16. The primary target is Windows; `precise_time.h` uses `GetSystemTimePreciseAsFileTime` on Windows and `clock_gettime` on Linux.

There are no automated tests.

## Architecture

The application has two layers connected exclusively via Qt signals/slots:

**`Stand`** (`stand.h/cpp`) — all business logic:
- Opens COM7 read-only at construction; emits `portError` if unavailable (disables the load button in UI).
- `loadCyclogram()` parses `cyclogram.ini` from the executable's directory (not the working directory) into `m_events` and `m_mappings`.
- `sendToBoard()` pings the onboard computer (БЦВМ) at `192.168.17.246` via ICMP, sends the cyclogram as a UTF-8 UDP datagram to port 4000, clears the serial buffer, then spawns `readingThread` as a `std::thread`.
- `readingThread` runs the hot path: reads 1 byte at a time, interprets each byte as a bitmask of 8 channels, emits timer/event signals every 1000 bytes (≈1 second), and drives the `Phase` state machine through Countdown → Running → Completed.
- Channel 8 (bitmask `0x80`) is the sync signal ("Контакт подъёма"). Its absolute byte-index becomes the time base for post-flight deviation analysis in `performAnalysis`.
- `stop()` sends a single byte `0x63` (99) via UDP to БЦВМ as the STOP command.

**`MainWindow`** (`mainwindow.h/cpp`) — pure UI:
- Owns `Stand*` and wires all its signals to local slots/lambdas in the constructor.
- `m_displayEvents` holds only the events with `hasChannels == true` (the ones with channel mappings).
- The `Phase` enum drives button enable/disable and the blinking phase indicator.

**`types.h`** — shared types: `Phase` enum, `EventRow` struct (event state including `status`, `firedTick`, `calculatedMs`, `deviationMs`), `ChannelMapping` struct.

## Cyclogram Format (`cyclogram.ini`)

```ini
SET_UTC_TIME = 10:00:00      # laboratory clock reference
START_UTC_TIME = 10:00:30    # target launch time (UTC)
EVENT_KEY = -8000            # milliseconds relative to lift-off (T0)
EVENT_KEY = 0                # T0 = lift-off contact
EVENT_KEY = 20000 # Optional inline comment becomes EventRow::description
```

- `SET_UTC_TIME` and `START_UTC_TIME` are mandatory control lines; all other lines are events.
- Time values are milliseconds relative to lift-off (T0 = 0); negative values are pre-launch.
- The inline comment after `#` becomes the event description shown in the UI table.
- The file is read from `QCoreApplication::applicationDirPath()`, so place it alongside the built executable during development.
- `Stand::setStartTimeFromUI` rewrites only the `START_UTC_TIME` line in-place.

## Channel Mapping

The mapping from cyclogram event keys to serial port bitmasks is **hardcoded** in `Stand`'s constructor (`stand.cpp:29-34`), not in the INI file:

| Key | Bitmask | Channels |
|-----|---------|----------|
| `IGNITE_PYRO_CANDLES_ENGINES_9_TO_12` | `0x03` | 1, 2 |
| `IGNITE_PYRO_CANDLES_ENGINES_1_TO_8` | `0x1C` | 3, 4, 5 |
| `CLOSE_MAIN_VALVES_ENGINES_9_TO_12` | `0x60` | 6, 7 |
| `LIFT_OFF_CONTACT` | `0x80` | 8 (sync) |

Only events whose key appears in this mapping have `hasChannels = true` and are tracked/displayed.

## Hardcoded Network and Serial Constants (`stand.h`)

- `SERIAL_PORT`: `"COM7"`
- `SERIAL_BAUDRATE`: `115200`
- `BCVM_IP`: `"192.168.17.246"`
- `UDP_PORT`: `4000`
- `SYNC_MASK`: `0x80` (channel 8, lift-off contact)
- `FLIGHT_SAFETY_MARGIN_MS`: `1000` (added to last tracked event time to determine thread stop)
- `RETRY_COUNT`: `3` (COM port open retries)

## Unused / Orphaned Files

Several files exist on disk but are **not included in `CMakeLists.txt`** and are not part of the compiled application:

- `launcher.cpp/h/launcher.ui` — unused UI component
- `com_class.cpp/h` — alternative serial wrapper (superseded by `Stand`'s direct `QSerialPort` usage)
- `cycleparser.cpp/h` — alternative INI parser using `QSettings` (superseded by `Stand::loadCyclogram`)
- `cyclogram_data.h` — static C++ array of the full mission cyclogram (not loaded at runtime)
- `precise_time.h` — nanosecond-precision timestamp utility (imported but not currently wired up)
- `crc8.cpp/h` — CRC-8 implementation (not currently used)

If expanding the application, consider whether to integrate these or remove them.

## Threading Model

- The Qt main thread owns all UI and `Stand` methods called from UI slots.
- `readingThread` is a raw `std::thread` that may only emit signals (never touch Qt widgets directly). Signals connected to main-thread slots use Qt's default auto-connection, which queues them correctly.
- `m_events` is protected by `m_eventsMutex` (`QMutex`) for access from both threads.
- `m_running` is `std::atomic<bool>` — the thread-stop flag.
- `flightComplete` is connected with `Qt::QueuedConnection` so `completeFlight()` executes in the main thread after the worker exits.
