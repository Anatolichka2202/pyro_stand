# pyro_stand — Technical Reference

This document covers the internal design of pyro_stand: timing model, threading, protocol flow, state machine, event pipeline, and test architecture. It is written for developers maintaining or extending the codebase.

---

## 1. Timing Model

### 1.1 The hardware clock

The sensor device connected to the COM port emits **exactly 1 byte every 1 ms**, continuously, regardless of what the PC or BCVM is doing. Each byte is an 8-bit bitmask of currently-active pyrotechnic channels.

`absoluteIndex` — the byte counter in `readingThread` — is therefore a **hardware millisecond timer**, not a software or OS clock. Its accuracy is determined by the sensor hardware, not by Qt or the operating system.

### 1.2 Drain = clock reset

The sensor runs from the moment it is powered on. By the time the operator reaches the "Load to board" step, the OS receive buffer holds N accumulated bytes — N milliseconds of elapsed real time. Without draining, `absoluteIndex = 0` would refer to N ms in the past and all timing would be offset by N.

The drain step, performed at the start of `readingThread` immediately after opening the COM port, discards this accumulated data in a non-blocking read loop:

```
waitForReadyRead(50)          // one wait to let the port wake up
do { read(buf, 4096) } while read() > 0   // drain without re-waiting
```

Re-waiting in the drain loop would be wrong: since the sensor always sends 1 byte/ms, `waitForReadyRead` would always return `true` and the loop would never exit. A single wait followed by non-blocking reads is the correct pattern.

After drain, `absoluteIndex = 0` corresponds to "right now" — specifically, after `sendToBoard()` has completed the cyclogram transfer:
- **TFTP mode**: drain follows receipt of the final ACK → confirmed delivery.
- **UDP mode**: drain follows the UDP `sendDatagram` call → no delivery guarantee.

Drain is skipped for `MockSerial` (empty `portName`): the mock starts clean at tick 0.

### 1.3 T0 and deviation self-cancellation

The true time reference is **channel 8** (`SYNC_MASK = 0x80`, `LIFT_OFF_CONTACT`). Its `absoluteIndex` at the moment of firing is stored as `m_syncIndex`.

All deviations are computed as:

```
deviation = firedTick - m_syncIndex
```

Because both `firedTick` and `m_syncIndex` are measured on the same hardware clock after the same drain, any systematic offset introduced by drain timing cancels out in the subtraction. The software deliberately does not live in real time — the sensor board records the absolute wall-clock reference.

### 1.4 Summary chain

```
sendToBoard()
  ├─ ping BCVM
  ├─ send cyclogram (UDP datagram OR TFTP handshake)
  │     TFTP: wait for final ACK before proceeding
  └─ spawn readingThread

readingThread
  ├─ open COM port (up to SERIAL_RETRIES=3 attempts)
  ├─ drain buffer  ← absoluteIndex=0 set here
  └─ byte loop (1 byte = 1 ms)
       ├─ mask & SYNC_MASK → m_syncIndex = N  (actual T0)
       ├─ other bits       → firedTick   = M
       └─ deviation = (M − N) − planned_ms_for_event
```

---

## 2. Threading Model

| Thread | Owner | Rule |
|---|---|---|
| **Main (GUI)** | Qt event loop | All UI operations; Stand public methods called from UI slots |
| **readingThread** | `std::thread m_worker` | May only emit signals; never touches widgets |

Signals emitted from `readingThread` are automatically queued to the main thread via Qt's auto-connection mechanism (the receiver lives in the main thread). No explicit `QMetaObject::invokeMethod` is needed.

The `flightComplete` signal is connected with `Qt::QueuedConnection` explicitly:

```cpp
connect(this, &Stand::flightComplete, this, &Stand::completeFlight, Qt::QueuedConnection);
```

This ensures `completeFlight()` — which calls `analyzeEvents()` and emits `analysisDone` — always executes in the main thread after the worker thread has exited, preventing data races on `m_events`.

### Shared state and protection

| Data | Protection |
|---|---|
| `m_events` | `QMutex m_eventsMutex` (locked in both readingThread and main thread) |
| `m_running` | `std::atomic<bool>` |
| `m_stopped` | `std::atomic<bool>` |
| `m_syncIndex`, `m_syncFound` | written only from readingThread; read from main thread only after join (via `completeFlight`) |

### Destruction order

`MainWindow` declares members in this order:

```cpp
std::unique_ptr<SessionLogger> m_logger;
std::unique_ptr<Stand>         m_stand;
```

C++ destroys members in reverse declaration order, so `m_stand` is destroyed first. `Stand::~Stand()` sets `m_running = false` and joins the worker thread. Only then is `m_logger` destroyed and the log file closed. This guarantees the log receives all entries before the file handle is closed.

---

## 3. TFTP Transfer Flow

The cyclogram is uploaded to BCVM via Trivial File Transfer Protocol (RFC 1350). The implementation in `Stand::sendCyclogramTftp()` follows the standard WRQ (write request) flow:

```
PC (ephemeral port)       BCVM (port 69)
      │                        │
      │─── WRQ "cyclogram.ini" ──▶│   opcode=2, mode=octet
      │                        │
      │◀─── ACK(0) ────────────│   server TID assigned here
      │                        │
      │─── DATA(1) [≤512 B] ──▶│
      │◀─── ACK(1) ────────────│
      │                        │
      │─── DATA(2) [≤512 B] ──▶│
      │◀─── ACK(2) ────────────│
      │         ...            │
      │─── DATA(N) [<512 B] ──▶│   final block signals end of file
      │◀─── ACK(N) ────────────│   ← drain happens after this ACK
```

The socket is bound to an ephemeral local port before sending WRQ (required for `readDatagram` to work on Windows). Subsequent DATA packets are sent to the server's TID port obtained from the ACK(0) source port. Each DATA block times out after 2000 ms. On error the server sends opcode 5 (ERROR) with a code and message.

The final ACK confirms that BCVM has received the complete cyclogram before the drain step executes, giving TFTP a timing advantage over UDP.

---

## 4. Phase State Machine

```
Idle ──loadCyclogram()──▶ Loaded ──sendToBoard()──▶ Countdown
                                                          │
                                               channel 8 fires
                                                          │
                                                          ▼
                                          Completed ◀── Running ──stop()──▶ Stopped
```

| Phase | Trigger | Timer display |
|---|---|---|
| `Idle` | App start | — |
| `Loaded` | `loadCyclogram()` succeeds | — |
| `Countdown` | `sendToBoard()` spawns worker | Countdown to planned T0 |
| `Running` | Channel 8 (`LIFT_OFF_CONTACT`) fires | Elapsed since actual T0 |
| `Completed` | Worker exits normally; `completeFlight()` runs | Final elapsed time |
| `Stopped` | `stop()` called by operator | "СТОП" |

**Key detail:** the transition from Countdown to Running happens only when channel 8 physically fires — not at the planned `START_UTC_TIME`. If T0 is late, the timer shows the overrun as a positive value in yellow. `analyzeEvents()` is called only after Running → Completed.

---

## 5. Event Detection Pipeline

Each byte processed by `readingThread` runs through this pipeline:

### 5.1 Real-time detection (inside readingThread)

For each tracked event (`hasChannels == true`) with status `"pending"`:

1. **Window check**: if `absoluteIndex < timeToStartMs + event.time_ms`, skip.
2. **Channel check**: if the required bitmask is fully set in `firedBits` (cumulative OR of all bytes):
   - Set `status = "ok"`, record `firedTick = absoluteIndex`.
   - Emit `eventFired(id, tick)` → MainWindow updates the table row with the raw tick.
3. **Timeout check**: if `absoluteIndex >= expectedIdx + FAIL_MARGIN_MS` and channels have not fired:
   - Set `status = "fail"`, `firedTick = -1`.
   - No signal emitted — "НЕ СРАБОТАЛО" is shown only in post-flight analysis.

### 5.2 Late-firing detection (inside readingThread)

Events with `status == "fail"` and `firedTick == -1` are re-checked every byte. If the required mask appears later:
- Record `firedTick = absoluteIndex`.
- Emit `eventFired(id, tick)` — the table shows the tick.
- `analyzeEvents()` will reclassify this as `"late"`.

### 5.3 Post-flight analysis (Stand::analyzeEvents, static pure function)

Called once from `completeFlight()` in the main thread after the worker exits. Takes a snapshot of `m_events` and `m_syncIndex`.

| Input status | `firedTick` | Output status | `calculatedMs` | `deviationMs` |
|---|---|---|---|---|
| `"ok"` | ≥ 0 | `"ok"` | `firedTick − syncIndex` | `|calculatedMs − planned_ms|` |
| `"fail"` | ≥ 0 (late fire) | `"late"` | `firedTick − syncIndex` | `|calculatedMs − planned_ms|` |
| `"fail"` | −1 (no fire) | `"fail"` | −1 | −1 |

**LIFT_OFF_CONTACT special case:** since channel 8 defines `syncIndex`, its `calculatedMs` is always 0. Its `deviationMs` instead reports `|firedTick − timeToStartMs|` — the actual T0 offset from planned T0. A late LIFT_OFF_CONTACT (status `"fail"` + `firedTick ≥ 0`) is reclassified as `"late"`.

If channel 8 never fires but the flight otherwise completes (started flag set), `m_syncIndex` is set to `timeToStartMs` as a fallback, and analysis proceeds with this synthetic reference.

### 5.4 UI colour coding

After `analysisDone` is emitted, `MainWindow::updateTable()` colours each row:

| Condition | Colour |
|---|---|
| `status == "ok"` and `deviationMs == 0` | Green |
| `status == "ok"` and `deviationMs` 1–5 ms | Yellow |
| `status == "ok"` and `deviationMs` > 5 ms | Red |
| `status == "late"` | Yellow/red (same thresholds) |
| `status == "fail"` | Red |
| `key == "LIFT_OFF_CONTACT"` | Green background; orange deviation text |

---

## 6. Key Constants

Defined as `private static constexpr` in `Stand`:

| Constant | Value | Purpose |
|---|---|---|
| `DEFAULT_PORT` | `COM7` / `/dev/ttyUSB0` | From `platform.h` |
| `BAUD_RATE` | `115200` | Serial port baud rate |
| `BCVM_IP` | `"192.168.17.246"` | Onboard computer IP |
| `UDP_PORT` | `4000` | Cyclogram (UDP) + STOP command |
| `TFTP_PORT` | `69` | Cyclogram (TFTP) |
| `TFTP_PATH` | `"/cyclogram.ini"` | Remote path for TFTP WRQ |
| `SYNC_MASK` | `0x80` | Channel 8 bitmask (T0 sync) |
| `FLIGHT_SAFETY_MS` | `5000` | Extra ms after last event before thread exits |
| `FAIL_MARGIN_MS` | `5` | ms grace period before marking event as fail |
| `SERIAL_RETRIES` | `3` | COM port open attempts at startup |

`FLIGHT_SAFETY_MS = 5000` gives 5 s of slack after the last planned event. The thread exits when `absoluteIndex > t0Ref + m_flightDurationMs + 100`, where `t0Ref` is the actual `syncIndex` (or `timeToStartMs` if channel 8 never fired).

`FAIL_MARGIN_MS = 5` is intentionally small: 5 ms covers timing jitter from the sensor while still catching genuine misfires promptly. The event is not permanently failed — late-firing detection continues until the thread exits.

---

## 7. Testing Architecture

Three levels, each independent of hardware.

### Level 1 — Unit / integration tests

No `QApplication`, no display. MockSerial is injected into Stand via the constructor:

```cpp
Stand stand(nullptr, "", std::make_unique<MockSerial>(), nullptr);
```

Empty `portName` suppresses ping + UDP; the injected `MockSerial` drives the byte stream.

| Test | Coverage |
|---|---|
| `tst_analysis` | `Stand::analyzeEvents()`: ok/fail/late status, deviation arithmetic, LIFT_OFF_CONTACT special case, syncIndex fallback |
| `tst_cyclogram` | `Stand::loadCyclogram()`: valid file, missing headers, bad values, comment stripping, `hasChannels` assignment |
| `tst_reading` | `Stand::readingThread` via `startReadingForTest()`: channel firing, missed events, late events, port drop, STOP command |
| `tst_logformat` | `SessionLogger`: tick-derived timestamps, session subdirectory naming, header format |

`MockSerial` API:

```cpp
mock->fireAt(tick, mask);   // schedule mask byte at given tick
mock->dropAfter(tick);      // simulate port disconnect
mock->setRealtime(true);    // 1 ms/byte sleep (demo mode)
mock->clearBuffers();       // reset tick counter (for re-runs)
```

### Level 2 — GUI simulation tests (tst_gui)

Requires `QApplication` and `Widgets`. Runs headless via:

```
QT_QPA_PLATFORM=offscreen ./tst_gui
```

Uses the injection constructor:

```cpp
MainWindow mw(std::make_unique<Stand>(...), cyclogramPath);
```

Three scenarios:
1. Full flight success → all tracked rows show `"ok"`, summary shows count of green items.
2. Missed event → one row shows `"fail"` / `"НЕ СРАБОТАЛО"`.
3. STOP mid-flight → phase becomes `Stopped`, timer label shows `"СТОП"`.

Widget objectName keys for test selectors: `timerLabel`, `phaseLabel`, `nextEventLabel`, `summaryLabel`.

### Level 3 — Demo mode (pyro_demo)

Interactive GUI with scripted MockSerial events. `setRealtime(true)` gives human-paced 1 ms/byte playback. Demonstrates the full operator workflow without hardware. No SessionLogger is attached (second MainWindow constructor form).

---

## 8. SessionLogger

Append-only structured log. Thread-safe via `QMutex`. Not a QObject.

**Log path:** `logDir/session_YYYY-MM-DD_HH-MM-SS/pyro_stand.log`

Wall clock is used only for the session folder name. All entry timestamps use tick-derived time:

```
timestamp = SET_UTC_TIME.addMSecs(tickMs)
```

`setTimeBase(QTime)` must be called after parsing `SET_UTC_TIME` from the cyclogram (done in `Stand::loadCyclogram`). Before `setTimeBase`, timestamps render as `"??:??:??.???"`.

Log entry format:

```
HH:mm:ss.zzz | LEVEL | message
```

Levels: `INFO`, `WARN`, `ERROR`, `EVENT`.

Stand writes to the logger directly (not via the `logMessage` signal path to MainWindow) to preserve tick-accurate timestamps. The signal path is for real-time UI display only.

---

## 9. ISerialPort Interface

```cpp
class ISerialPort {
public:
    virtual bool    open(const QString &portName, int baudRate) = 0;
    virtual void    close() = 0;
    virtual bool    isOpen() const = 0;
    virtual bool    waitForReadyRead(int msecs) = 0;
    virtual qint64  read(char *data, qint64 maxSize) = 0;
    virtual bool    hasError() const = 0;
    virtual QString errorString() const = 0;
    virtual void    clearBuffers() = 0;
};
```

`RealSerialPort` wraps `QSerialPort` and is constructed inside `readingThread` to satisfy Qt's thread affinity requirement — `QSerialPort` must be created in the thread that uses it.

`MockSerial` is created in the main thread (or test body) and injected via the Stand constructor. It uses a `QMap<int64_t, uint8_t>` lookup and returns 1 byte per `read()` call regardless of `maxSize`.
