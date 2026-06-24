#include "stand.h"
#include "real_serial_port.h"
#include "session_logger.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QUdpSocket>
#include <QThread>
#include <QProcess>
#include <QDateTime>
#include <QDataStream>
#include <QtEndian>

Stand::Stand(QObject *parent, const QString &portName, std::unique_ptr<ISerialPort> port,
             SessionLogger *logger)
    : QObject(parent), m_portName(portName), m_logger(logger)
{
    m_mappings = {
        {"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12", 0x03, "1, 2"},
        {"FIRE_STARTING_CHAMBER_ENGINES_9_TO_12", 0x1C, "3, 4, 5"},
        {"CLOSE_MAIN_VALVES_ENGINES_9_TO_12",   0x60, "6, 7"},
        {"LIFT_OFF_CONTACT",                    0x80, "8"}
    };

    if (port) {
        // Инжекция (тесты): не пробуем открывать, доверяем инжектируемому порту
        m_port = std::move(port);
        m_portAvailable = true;
    } else if (!portName.isEmpty()) {
        m_port = std::make_unique<RealSerialPort>();
        // Пробная проверка доступности порта
        m_portAvailable = m_port->open(portName, BAUD_RATE);
        if (m_portAvailable) {
            m_port->close();
            if (m_logger) m_logger->log("INFO", "COM-порт " + portName + " доступен");
            emit logMessage("COM-порт " + portName + " доступен", "system");
        } else {
            if (m_logger) m_logger->log("ERROR", "Не удалось открыть COM-порт. Тест невозможен.");
            emit logMessage("Не удалось открыть COM-порт. Тест невозможен.", "system");
            emit portError("Ошибка COM-порта: " + m_port->errorString());
        }
    }

    // completeFlight всегда исполняется в GUI-треде
    connect(this, &Stand::flightComplete, this, &Stand::completeFlight, Qt::QueuedConnection);
}

Stand::~Stand()
{
    m_running = false;
    if (m_worker.joinable()) m_worker.join();
}

// ─── helpers ──────────────────────────────────────────────────────────────────

QString Stand::cyclogramFilePath() const
{
    return QCoreApplication::applicationDirPath() + QDir::separator() + CYCLOGRAM_FILE;
}

// ─── loadCyclogram ────────────────────────────────────────────────────────────

bool Stand::loadCyclogram(const QString &filePath)
{
    const QString path = filePath.isEmpty() ? cyclogramFilePath() : filePath;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit logMessage("Файл циклограммы не найден: " + path, "system");
        return false;
    }

    QVector<EventRow> events;
    QTime setTime, startTime;
    bool setTimeFound = false, startTimeFound = false;
    int lineNum = 0;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        ++lineNum;
        if (line.isEmpty()) continue;

        const int commentPos = line.indexOf('#');
        const QString comment = (commentPos != -1) ? line.mid(commentPos + 1).trimmed() : QString();
        if (commentPos != -1) line = line.left(commentPos).trimmed();
        if (line.isEmpty()) continue;

        const int eqPos = line.indexOf('=');
        if (eqPos == -1) { emit logMessage(QString("Строка %1: нет '='").arg(lineNum), "system"); continue; }

        const QString key      = line.left(eqPos).trimmed();
        const QString valueStr = line.mid(eqPos + 1).trimmed();

        if (key == "SET_UTC_TIME") {
            setTime = QTime::fromString(valueStr, "hh:mm:ss");
            if (!setTime.isValid()) { emit logMessage("Неверный SET_UTC_TIME: " + valueStr, "system"); return false; }
            setTimeFound = true; continue;
        }
        if (key == "START_UTC_TIME") {
            startTime = QTime::fromString(valueStr, "hh:mm:ss");
            if (!startTime.isValid()) { emit logMessage("Неверный START_UTC_TIME: " + valueStr, "system"); return false; }
            startTimeFound = true; continue;
        }

        bool ok;
        const int timeMs = valueStr.toInt(&ok);
        if (!ok) { emit logMessage(QString("Строка %1: не число '%2'").arg(lineNum).arg(valueStr), "system"); continue; }

        EventRow ev;
        ev.id          = events.size() + 1;
        ev.key         = key;
        ev.description = comment.isEmpty() ? key : comment;
        ev.time_ms     = timeMs;
        ev.firedTick   = -1;
        ev.calculatedMs= -1;
        ev.status      = "pending";
        ev.deviationMs = 0;
        ev.hasChannels = false;
        ev.channels    = "—";

        for (const auto &m : m_mappings) {
            if (m.key == key) { ev.channels = m.channelsStr; ev.hasChannels = true; break; }
        }
        events.append(ev);
    }

    if (!setTimeFound || !startTimeFound) {
        emit logMessage("В файле не найдены SET_UTC_TIME и/или START_UTC_TIME", "system");
        return false;
    }
    if (events.isEmpty()) {
        emit logMessage("Циклограмма не содержит событий", "system");
        return false;
    }

    int maxEventTime = 0;
    int trackedCount = 0;
    for (const auto &ev : events) {
        if (ev.hasChannels) ++trackedCount;
        if (ev.time_ms > maxEventTime) maxEventTime = ev.time_ms;
    }
    m_flightDurationMs = maxEventTime + FLIGHT_SAFETY_MS;

    {
        QMutexLocker l(&m_eventsMutex);
        m_events   = events;
        m_setTime  = setTime;
        m_startTime = startTime;
    }

    // Устанавливаем базу времени и записываем заголовок
    if (m_logger) {
        m_logger->setTimeBase(setTime);
        m_logger->writeHeader(path,
                              setTime,  setTime.toString("hh:mm:ss"),
                              startTime, startTime.toString("hh:mm:ss"),
                              trackedCount);
        m_logger->log("INFO", QString("Загружена циклограмма: %1 событий, из них отслеживаемых: %2")
                                   .arg(events.size()).arg(trackedCount));
    }

    emit logMessage(QString("Загружено событий: %1").arg(events.size()), "system");
    emit logMessage(QString("SET_UTC_TIME: %1  |  START_UTC_TIME: %2")
                        .arg(setTime.toString("hh:mm:ss"))
                        .arg(startTime.toString("hh:mm:ss")), "system");
    emit analysisDone(events);
    updatePhase(Phase::Loaded);
    return true;
}

// ─── setStartTimeFromUI ───────────────────────────────────────────────────────

bool Stand::setStartTimeFromUI(const QTime &time, const QString &filePath)
{
    if (!time.isValid()) { emit logMessage("Неверное время старта", "system"); return false; }
    const QString path = filePath.isEmpty() ? cyclogramFilePath() : filePath;
    if (!writeStartTimeToFile(time, path)) {
        emit logMessage("Не удалось записать время старта в файл", "system");
        return false;
    }
    { QMutexLocker l(&m_eventsMutex); m_startTime = time; }
    emit logMessage("Время старта установлено: " + time.toString("hh:mm:ss"), "system");
    return true;
}

bool Stand::writeStartTimeToFile(const QTime &time, const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) return false;

    QStringList lines;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().startsWith("START_UTC_TIME", Qt::CaseInsensitive))
            line = "START_UTC_TIME = " + time.toString("hh:mm:ss");
        lines.append(line);
    }
    file.resize(0);
    QTextStream out(&file);
    for (const QString &line : lines) out << line << "\n";
    return true;
}

// ─── sendCyclogramTftp ───────────────────────────────────────────────────────
// Trivial File Transfer Protocol (RFC 1350): UDP, порт 69, без аутентификации.
// Протокол: WRQ → ACK(0) → DATA(1) → ACK(1) → … → DATA(N<512) → ACK(N)

bool Stand::sendCyclogramTftp(const QByteArray &data)
{
    constexpr int    BLOCK_SIZE  = 512;
    constexpr int    TIMEOUT_MS  = 2000;
    constexpr quint16 OP_WRQ    = 2;
    constexpr quint16 OP_DATA   = 3;
    constexpr quint16 OP_ACK    = 4;
    constexpr quint16 OP_ERROR  = 5;

    QUdpSocket sock;
    const QHostAddress host(BCVM_IP);

    // Bind к эфемерному порту — обязательно, иначе readDatagram вернёт 0 байт
    if (!sock.bind(QHostAddress::AnyIPv4, 0)) {
        const QString err = "TFTP: не удалось привязать сокет: " + sock.errorString();
        if (m_logger) m_logger->log("ERROR", err);
        emit logMessage(err, "system");
        return false;
    }

    // --- 1. WRQ (Write Request) ---
    QByteArray wrq;
    {
        QDataStream ds(&wrq, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << OP_WRQ;
    }
    wrq.append(TFTP_PATH);   // имя файла (null-terminated)
    wrq.append('\0');
    wrq.append("octet");     // режим передачи
    wrq.append('\0');

    sock.writeDatagram(wrq, host, TFTP_PORT);

    // --- 2. Ждём ACK(0) от сервера (сервер отвечает со своего TID-порта) ---
    if (!sock.waitForReadyRead(TIMEOUT_MS)) {
        const QString err = "TFTP: таймаут ожидания ACK от сервера";
        if (m_logger) m_logger->log("ERROR", err);
        emit logMessage(err, "system");
        return false;
    }

    char ackBuf[516] = {};   // max TFTP packet: 4 header + 512 data
    quint16 serverTid = 0;
    QHostAddress serverAddr;
    const qint64 n0 = sock.readDatagram(ackBuf, sizeof(ackBuf), &serverAddr, &serverTid);
    if (n0 < 4) {
        const QString err = QString("TFTP: пустой пакет от сервера (%1 байт)").arg(n0);
        if (m_logger) m_logger->log("ERROR", err);
        emit logMessage(err, "system");
        return false;
    }

    const quint16 opAck   = qFromBigEndian<quint16>(ackBuf);
    const quint16 blkAck0 = qFromBigEndian<quint16>(ackBuf + 2);
    if (opAck == OP_ERROR) {
        const QString errMsg = QString::fromUtf8(ackBuf + 4, static_cast<int>(n0) - 4);
        const QString err = QString("TFTP: ошибка сервера (код %1): %2").arg(blkAck0).arg(errMsg);
        if (m_logger) m_logger->log("ERROR", err);
        emit logMessage(err, "system");
        return false;
    }
    if (opAck != OP_ACK || blkAck0 != 0) {
        const QString err = QString("TFTP: неожиданный пакет op=%1 block=%2").arg(opAck).arg(blkAck0);
        if (m_logger) m_logger->log("ERROR", err);
        emit logMessage(err, "system");
        return false;
    }

    // --- 3. Отправляем блоки данных ---
    // RFC 1350: последний блок < 512 байт завершает передачу.
    // Если размер кратен 512 — финальный пустой блок всё равно нужен.
    int offset = 0;
    quint16 blockNum = 1;
    bool lastBlock = false;
    do {
        const QByteArray chunk = data.mid(offset, BLOCK_SIZE);
        lastBlock = (chunk.size() < BLOCK_SIZE);  // короткий или пустой = конец
        offset += chunk.size();

        QByteArray pkt;
        {
            QDataStream ds(&pkt, QIODevice::WriteOnly);
            ds.setByteOrder(QDataStream::BigEndian);
            ds << OP_DATA << blockNum;
        }
        pkt.append(chunk);
        sock.writeDatagram(pkt, serverAddr, serverTid);

        // Ждём ACK(blockNum)
        if (!sock.waitForReadyRead(TIMEOUT_MS)) {
            const QString err = QString("TFTP: таймаут ACK блока %1").arg(blockNum);
            if (m_logger) m_logger->log("ERROR", err);
            emit logMessage(err, "system");
            return false;
        }

        char ackN[4] = {};
        sock.readDatagram(ackN, sizeof(ackN), nullptr, nullptr);
        const quint16 ackBlock = qFromBigEndian<quint16>(ackN + 2);
        if (ackBlock != blockNum) {
            const QString err = QString("TFTP: ACK %1 ≠ ожидаемый %2").arg(ackBlock).arg(blockNum);
            if (m_logger) m_logger->log("ERROR", err);
            emit logMessage(err, "system");
            return false;
        }

        ++blockNum;
    } while (!lastBlock);

    if (m_logger) m_logger->log("INFO", QString("Циклограмма отправлена на БЦВМ по TFTP (%1 байт)").arg(data.size()));
    emit logMessage(QString("Циклограмма отправлена на БЦВМ по TFTP (%1 байт)").arg(data.size()), "system");
    return true;
}

// ─── pingBcvm ────────────────────────────────────────────────────────────────

bool Stand::pingBcvm() const
{
#ifdef Q_OS_WIN
    QStringList args = {"-n", "1", BCVM_IP};
#else
    QStringList args = {"-c", "1", BCVM_IP};
#endif
    QProcess ping;
    ping.start("ping", args);
    if (!ping.waitForFinished(2000)) { ping.kill(); ping.waitForFinished(500); return false; }
    if (ping.exitCode() != 0) return false;
    const QString out = ping.readAllStandardOutput();
    return out.contains("TTL=") || out.contains("ttl=") || out.contains("time=");
}

// ─── sendToBoard ─────────────────────────────────────────────────────────────

void Stand::sendToBoard()
{
    if (m_running) {
        m_running = false;
        if (m_worker.joinable()) m_worker.join();
    }

    if (!m_portAvailable) {
        emit logMessage("COM-порт не доступен. Невозможно отправить циклограмму.", "system");
        return;
    }

    // Mock/test mode: portName is empty when ISerialPort was injected externally.
    // Skip network operations (BCVM ping, UDP send) — hardware not present.
    const bool mockMode = m_portName.isEmpty();

    if (!mockMode && !pingBcvm()) {
        emit logMessage("ОШИБКА: БЦВМ недоступна. Проверьте подключение.", "system");
        return;
    }

    QTime setTime, startTime;
    { QMutexLocker l(&m_eventsMutex); setTime = m_setTime; startTime = m_startTime; }

    int secsToStart = setTime.secsTo(startTime);
    if (secsToStart < 0) secsToStart += 24 * 3600;
    m_timeToStartMs = static_cast<int64_t>(secsToStart) * 1000;

    if (m_timeToStartMs < 1000) {
        if (m_logger) m_logger->log("ERROR", "Время до старта менее 1 с. Проверьте SET/START_UTC_TIME.");
        emit logMessage("Время до старта менее 1 с. Проверьте SET/START_UTC_TIME.", "system");
        return;
    }
    if (m_logger) m_logger->log("INFO", QString("Время до старта: %1 с (%2 мс)").arg(secsToStart).arg(m_timeToStartMs));
    emit logMessage(QString("Время до старта: %1 с (%2 мс)").arg(secsToStart).arg(m_timeToStartMs), "system");

    if (!mockMode) {
        const QString path = cyclogramFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            if (m_logger) m_logger->log("ERROR", "Не удалось открыть файл для отправки");
            emit logMessage("Не удалось открыть файл для отправки", "system");
            return;
        }
        QStringList lines;
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            QString line = stream.readLine();
            if (line.trimmed().startsWith("START_UTC_TIME", Qt::CaseInsensitive))
                line = "START_UTC_TIME = " + startTime.toString("hh:mm:ss");
            lines.append(line);
        }
        file.close();

        const QByteArray payload = lines.join("\n").toUtf8();

        if (m_transferMode == TransferMode::UDP) {
            QUdpSocket udp;
            const qint64 sent = udp.writeDatagram(payload, QHostAddress(BCVM_IP), UDP_PORT);
            if (sent == -1) {
                if (m_logger) m_logger->log("ERROR", "Ошибка UDP: " + udp.errorString());
                emit logMessage("Ошибка UDP: " + udp.errorString(), "system"); return;
            }
            if (m_logger) m_logger->log("INFO", QString("Циклограмма отправлена на БЦВМ по UDP (%1 байт)").arg(sent));
            emit logMessage(QString("Циклограмма отправлена на БЦВМ по UDP (%1 байт)").arg(sent), "system");
        } else {
            if (!sendCyclogramTftp(payload)) return; // ошибка уже залогирована внутри
        }
    } else {
        m_port->clearBuffers();  // mock: сбрасывает m_tick для повторного прогона
    }

    m_masks.clear();
    m_syncFound   = false;
    m_syncIndex   = -1;
    m_finalIndex  = 0;
    m_analysisDone= false;
    m_stopped     = false;

    m_running = true;
    m_worker  = std::thread(&Stand::readingThread, this, m_timeToStartMs);
    updatePhase(Phase::Countdown);
}

// ─── startReadingForTest ──────────────────────────────────────────────────────

void Stand::startReadingForTest(int64_t timeToStartMs)
{
    m_timeToStartMs = timeToStartMs;
    m_masks.clear();
    m_syncFound    = false;
    m_syncIndex    = -1;
    m_finalIndex   = 0;
    m_analysisDone = false;
    m_stopped      = false;
    m_running      = true;
    m_worker       = std::thread(&Stand::readingThread, this, timeToStartMs);
}

// ─── readingThread ────────────────────────────────────────────────────────────

void Stand::readingThread(int64_t timeToStartMs)
{
    if (!m_port || !m_port->open(m_portName, BAUD_RATE)) {
        const QString errMsg = "Не удалось открыть COM-порт в потоке чтения";
        if (m_logger) m_logger->log("ERROR", errMsg);
        emit portError(errMsg);
        return;
    }


    if (m_logger) m_logger->log("INFO", QString("Поток чтения запущен, порт открыт: %1").arg(m_portName));
    emit logMessage(QString("Поток чтения: %1 открыт").arg(m_portName), "system");

    // СБРОС БУФЕРА COM-ПОРТА — две цели:
    //
    // 1. Мусор инициализации RS-232: при открытии порта линии могут быть в
    //    произвольном состоянии. Первые байты содержат ложные биты каналов.
    //
    // 2. Сброс накопленного времени (КЛЮЧЕВОЕ): датчик срабатывания непрерывно
    //    шлёт 1 байт/мс с момента включения. Пока оператор готовился, в буфере
    //    накопилось N байт = N мс «прошедшего» времени. Без drain absoluteIndex=0
    //    соответствовал бы не «сейчас», а «N мс назад» — все времена съехали бы.
    //    Drain обнуляет часы: absoluteIndex=0 = момент после подтверждения приёма
    //    циклограммы бортом (для TFTP — после последнего ACK; для UDP — после
    //    отправки, без гарантии приёма).
    //
    // Не делаем для mock-режима (portName пустой) — MockSerial чистый с тика 0.
    if (!m_portName.isEmpty()) {
        int64_t totalDrained = 0;
        char tmp[4096];
        // Один waitForReadyRead — даём порту "проснуться" и буферу стать доступным.
        // Затем читаем БЕЗ повторного ожидания: датчик шлёт 1 байт/мс, поэтому
        // повторный waitForReadyRead(50) всегда вернул бы true — цикл стал бы бесконечным.
        // read() без ожидания возвращает 0 когда буфер моментально пуст — это и есть выход.
        if (m_port->waitForReadyRead(50)) {
            qint64 n;
            do {
                n = m_port->read(tmp, sizeof(tmp));
                if (n > 0) totalDrained += n;
            } while (n > 0);
        }
        if (m_logger && totalDrained > 0)
            m_logger->log("INFO", QString("Сброшено %1 байт(а) из буфера COM-порта (обнуление часов)").arg(totalDrained), 0);
    }

    int64_t absoluteIndex = 0;
    int64_t noDataCount = 0;   // диагностика: сколько раз waitForReadyRead вернул false
    int64_t spuriousCount = 0; // диагностика: read()=0 при waitForReadyRead=true
    uint8_t firedBits = 0;
    bool started = false;

    // Тик первого появления каждого бита (0-7) в потоке.
    // Используется для firedTick = реальный аппаратный момент,
    // а не момент детектирования в цикле событий.
    int64_t bitFirstSeen[8];
    std::fill(std::begin(bitFirstSeen), std::end(bitFirstSeen), int64_t(-1));
    // Возвращает тик, когда сработал последний из нужных каналов маски.
    auto firedAtFor = [&](uint8_t mask) -> int64_t {
        int64_t t = 0;
        for (int b = 0; b < 8; ++b)
            if ((mask & (1 << b)) && bitFirstSeen[b] > t)
                t = bitFirstSeen[b];
        return t > 0 ? t : absoluteIndex;  // fallback: не должно случиться
    };

    while (m_running) {
        if (!m_port->waitForReadyRead(100)) {
            if (!m_port->isOpen() || m_port->hasError()) {
                const QString errMsg = QString("COM-порт отключён: %1").arg(m_port->errorString());
                if (m_logger) m_logger->log("ERROR", errMsg, absoluteIndex);
                emit portError(errMsg);
                m_running = false;
                break;
            }
            ++noDataCount;
            // Диагностика: логируем каждые 3 секунды пока нет данных
            if (noDataCount % 30 == 0) {
                const QString msg = QString("ДИАГН: данных нет %1 с (байт получено: %2)")
                    .arg(noDataCount / 10).arg(absoluteIndex);
                if (m_logger) m_logger->log("WARN", msg, absoluteIndex);
                emit logMessage(msg, "system");
            }
            continue;
        }
        noDataCount = 0;

        // Читаем все доступные байты за один вызов.
        // RealSerial: возвращает всё, что накопилось в буфере OS.
        // MockSerial: всегда возвращает ровно 1 байт.
        char buf[4096] = {};
        const qint64 nRead = m_port->read(buf, sizeof(buf));
        if (nRead <= 0) {
            // Spurious wakeup — waitForReadyRead вернул true, но данных нет.
            ++spuriousCount;
            if (m_logger && spuriousCount <= 5) {
                m_logger->log("WARN",
                    QString("ДИАГН spurious#%1 read()=%2 at idx=%3 err=%4")
                        .arg(spuriousCount).arg(nRead).arg(absoluteIndex)
                        .arg(m_port->errorString()),
                    absoluteIndex);
            }
            continue;
        }
        spuriousCount = 0;

        for (qint64 bi = 0; bi < nRead && m_running; ++bi) {
            const uint8_t mask = static_cast<uint8_t>(buf[bi]);
            m_masks.push_back({absoluteIndex, mask});

            // Таймер (раз в секунду)
            if (absoluteIndex % 1000 == 0) {
                if (m_logger) m_logger->log("INFO",
                    QString("ДИАГН tick[%1] mask=0x%2").arg(absoluteIndex).arg(mask, 2, 16, QChar('0')),
                    absoluteIndex);
                if (m_syncFound) {
                    // После фактического КП — время в полёте относительно реального T0
                    emit timerTick(TimerState{absoluteIndex - m_syncIndex, m_phase});
                    updateNextEvent(absoluteIndex, m_syncIndex);
                } else if (!started) {
                    // Обратный отсчёт до планового T0
                    emit timerTick(TimerState{-(timeToStartMs - absoluteIndex), m_phase});
                    updateNextEvent(absoluteIndex, timeToStartMs);
                } else {
                    // Плановый T0 прошёл, КП ещё не было.
                    // Отправляем отрицательное значение (жёлтый цвет = "ждём старт")
                    // |msToStart| = перерасход планового T0 в мс.
                    emit timerTick(TimerState{timeToStartMs - absoluteIndex, m_phase});
                    updateNextEvent(absoluteIndex, timeToStartMs);
                }
            }

            if (!started && absoluteIndex >= timeToStartMs) {
                started = true;
                if (m_logger) m_logger->log("INFO", "─── T0 по плану ───", absoluteIndex);
                emit logMessage("─── T0 по плану ───", "system");
                // Фаза Running запускается только по фактическому КП (канал 8),
                // не по плановому времени — ждём аппаратного сигнала.
            }

            // Обработка входящих бит-масок
            uint8_t newBits = mask & ~firedBits;
            if (newBits & SYNC_MASK) {
                const int64_t t0Delay = static_cast<int64_t>(absoluteIndex) - timeToStartMs;
                QString kpMsg;
                if (t0Delay == 0) {
                    kpMsg = QString("[%1] Контакт подъёма (КП, канал 8) — T0 ТОЧНО").arg(absoluteIndex);
                } else if (t0Delay > 0) {
                    kpMsg = QString("[%1] Контакт подъёма (КП, канал 8) — опоздание на %2 мс (ожидалось на %2 мс раньше)")
                        .arg(absoluteIndex).arg(t0Delay);
                } else {
                    kpMsg = QString("[%1] Контакт подъёма (КП, канал 8) — опережение на %2 мс (ожидалось на %2 мс позже)")
                        .arg(absoluteIndex).arg(-t0Delay);
                }
                if (m_logger) m_logger->log("EVENT", kpMsg.mid(kpMsg.indexOf(']') + 2), absoluteIndex);
                emit logMessage(kpMsg, "event");

                if (!started) started = true;  // ранний старт (до планового T0)
                bitFirstSeen[7] = absoluteIndex; // фиксируем реальный тик КП
                m_syncIndex = absoluteIndex;
                m_syncFound = true;
                newBits    &= ~SYNC_MASK;
                firedBits  |= SYNC_MASK;

                // В полёте — только после фактического КП
                updatePhase(Phase::Running);
            }
            if (newBits != 0) {
                // Фиксируем реальный аппаратный тик первого появления каждого бита
                for (int b = 0; b < 8; ++b)
                    if ((newBits & (1 << b)) && bitFirstSeen[b] == -1)
                        bitFirstSeen[b] = absoluteIndex;
                QStringList chs;
                for (int b = 0; b < 7; ++b) if (newBits & (1 << b)) chs << QString::number(b + 1);
                if (m_logger) m_logger->log("EVENT", "срабатывание канала(ов): " + chs.join(", "), absoluteIndex);
                emit logMessage(QString("[%1] срабатывание канала(ов): %2").arg(absoluteIndex).arg(chs.join(", ")), "event");
                firedBits |= newBits;
            }

            // Проверка событий циклограммы
            struct PendingSignal { int id; int tick; QString key; int planMs; };
            QVector<PendingSignal> pending;
            {
                QMutexLocker locker(&m_eventsMutex);
                for (auto &ev : m_events) {
                    if (!ev.hasChannels) continue;

                    if (ev.status == "pending") {
                        const int64_t expectedIdx = timeToStartMs + ev.time_ms;
                        if (absoluteIndex < expectedIdx) continue;

                        uint8_t neededMask = 0;
                        for (const auto &m : m_mappings) { if (m.key == ev.key) { neededMask = m.mask; break; } }
                        if (neededMask == 0) continue;

                        if ((firedBits & neededMask) == neededMask) {
                            ev.status   = "ok";
                            ev.firedTick= static_cast<int>(firedAtFor(neededMask));
                            // Индивидуальные тики и разброс по каналам группы
                            {
                                int64_t minT = INT64_MAX, maxT = -1;
                                for (int b = 0; b < 8; ++b) {
                                    if ((neededMask & (1 << b)) && bitFirstSeen[b] >= 0) {
                                        ev.channelTicks[b + 1] = static_cast<int>(bitFirstSeen[b]);
                                        minT = std::min(minT, bitFirstSeen[b]);
                                        maxT = std::max(maxT, bitFirstSeen[b]);
                                    }
                                }
                                ev.channelSpreadMs = (maxT > minT) ? static_cast<int>(maxT - minT) : 0;
                            }
                            pending.push_back({ev.id, ev.firedTick, ev.key, ev.time_ms});
                        } else if (absoluteIndex >= expectedIdx + FAIL_MARGIN_MS) {
                            // Плановое окно истекло — помечаем fail внутренне.
                            // Оценка "НЕ СРАБОТАЛО" выводится только в анализе после полёта,
                            // не засоряя лог в реальном времени. Канал может сработать позже
                            // (late-detection ниже зафиксирует firedTick).
                            ev.status   = "fail";
                            ev.firedTick= -1;
                        }
                    } else if (ev.status == "fail" && ev.firedTick == -1) {
                        // Каналы сработали позже планового окна — фиксируем тик.
                        // analyzeEvents вычислит статус "late" относительно фактического T0.
                        uint8_t neededMask = 0;
                        for (const auto &m : m_mappings) { if (m.key == ev.key) { neededMask = m.mask; break; } }
                        if (neededMask != 0 && (firedBits & neededMask) == neededMask) {
                            ev.firedTick = static_cast<int>(firedAtFor(neededMask));
                            // Уведомляем UI о срабатывании (таблица покажет тик, статус — после анализа)
                            pending.push_back({ev.id, ev.firedTick, ev.key, ev.time_ms});
                        }
                    }
                }
            }
            for (const auto &p : pending) {
                if (m_logger) {
                    m_logger->log("EVENT",
                        QString("id=%1 key=%2 tick=%3 plan_ms=%4 status=OK")
                            .arg(p.id).arg(p.key).arg(p.tick).arg(p.planMs),
                        p.tick);
                }
                emit eventFired(p.id, p.tick);
            }

            ++absoluteIndex;

            // Выходим когда все события прошли:
            // после КП — считаем от реального T0 (учитываем опоздание старта);
            // если КП не было — от планового T0 (защита от зависания).
            {
                const int64_t t0Ref = m_syncFound ? m_syncIndex : timeToStartMs;
                if (started && absoluteIndex > t0Ref + m_flightDurationMs + 100) {
                    m_running = false;
                }
            }
        } // for bi
    } // while m_running

    m_port->close();
    m_finalIndex = absoluteIndex;

    if (m_syncFound && !m_analysisDone) {
        emit flightComplete();
    } else if (!m_syncFound && started) {
        if (m_logger) m_logger->log("WARN", "Синхроимпульс (канал 8) не найден. Используется расчётное T0.", absoluteIndex);
        emit logMessage("ВНИМАНИЕ: синхроимпульс (канал 8) не найден. Используется расчётное T0.", "system");
        m_syncIndex = timeToStartMs;
        m_syncFound = true;
        emit flightComplete();
    } else {
        if (m_logger) m_logger->log("WARN", "Полёт прерван до старта. Анализ невозможен.", absoluteIndex);
        emit logMessage("Полёт прерван до старта. Анализ невозможен.", "system");
        updatePhase(Phase::Stopped);
    }
}

// ─── completeFlight (GUI-тред) ───────────────────────────────────────────────

void Stand::completeFlight()
{
    // Если оператор нажал СТОП — игнорируем завершение
    if (m_stopped) return;

    updatePhase(Phase::Completed);
    if (m_logger) m_logger->log("INFO", "═══ Полётное задание завершено ═══", m_finalIndex);
    emit logMessage("═══ Полётное задание завершено ═══", "system");

    if (!m_syncFound) { m_syncIndex = m_timeToStartMs; m_syncFound = true; }

    QVector<EventRow> snapshot;
    { QMutexLocker l(&m_eventsMutex); snapshot = m_events; }

    const QVector<EventRow> result = analyzeEvents(snapshot, m_syncIndex, m_timeToStartMs);

    { QMutexLocker l(&m_eventsMutex); m_events = result; }

    // Если фактический T0 отличается от планового — сообщаем об опоздании старта
    const int64_t t0Delay = m_syncIndex - m_timeToStartMs;
    if (t0Delay != 0) {
        const QString dir = t0Delay > 0 ? "ОПОЗДАЛ" : "ОПЕРЕДИЛ";
        const QString msg = QString("─── Фактический T0 %1 на %2 мс ───").arg(dir).arg(qAbs(t0Delay));
        if (m_logger) m_logger->log("INFO", msg, m_syncIndex);
        emit logMessage(msg, "system");
    }

    for (const auto &ev : result) {
        // Не отслеживаемые события (нет маппинга каналов) — пишем только в файловый лог,
        // в UI-лог не выводим (нет смысла пугать оператора красным "НЕ СРАБОТАЛО").
        const bool showInUi = ev.hasChannels;

        if (ev.status == "ok") {
            if (m_logger)
                m_logger->log("INFO",
                    QString("АНАЛИЗ id=%1 key=%2 calculated_ms=%3 plan_ms=%4 dev_ms=%5 status=OK")
                        .arg(ev.id).arg(ev.key).arg(ev.calculatedMs).arg(ev.time_ms).arg(ev.deviationMs),
                    m_syncIndex + ev.calculatedMs);
            if (showInUi)
                emit logMessage(QString("[%1 мс] %2 (откл. %3 мс)")
                                    .arg(ev.calculatedMs).arg(ev.description).arg(ev.deviationMs), "event");
        } else if (ev.status == "late") {
            if (m_logger)
                m_logger->log("INFO",
                    QString("АНАЛИЗ id=%1 key=%2 calculated_ms=%3 plan_ms=%4 dev_ms=%5 status=LATE")
                        .arg(ev.id).arg(ev.key).arg(ev.calculatedMs).arg(ev.time_ms).arg(ev.deviationMs),
                    m_syncIndex + ev.calculatedMs);
            if (showInUi)
                emit logMessage(QString("[%1 мс] %2 (ОПОЗДАНИЕ СТАРТА, откл. %3 мс от T0)")
                                    .arg(ev.calculatedMs).arg(ev.description).arg(ev.deviationMs), "event-post");
        } else {
            if (m_logger)
                m_logger->log("INFO",
                    QString("АНАЛИЗ id=%1 key=%2 status=FAIL").arg(ev.id).arg(ev.key),
                    m_finalIndex);
            if (showInUi)
                emit logMessage(QString("[—] %1 НЕ СРАБОТАЛО").arg(ev.description), "event-post");
        }
    }
    emit analysisDone(result);
    m_analysisDone = true;
}

// ─── analyzeEvents (static, pure) ────────────────────────────────────────────

QVector<EventRow> Stand::analyzeEvents(const QVector<EventRow> &events,
                                        int64_t syncIndex,
                                        int64_t timeToStartMs)
{
    QVector<EventRow> result = events;
    for (auto &ev : result) {
        // LIFT_OFF_CONTACT — сам является T0, поэтому (firedTick - syncIndex) всегда 0.
        // Его реальное отклонение = опоздание/опережение старта = firedTick - timeToStartMs.
        if (ev.key == QLatin1String("LIFT_OFF_CONTACT") && timeToStartMs >= 0) {
            if (ev.firedTick != -1) {
                ev.calculatedMs = 0;
                ev.deviationMs  = static_cast<int>(qAbs(static_cast<int64_t>(ev.firedTick) - timeToStartMs));
                // readingThread ставит "fail", когда плановое окно истекло без срабатывания,
                // и потом пишет firedTick когда канал 8 всё-таки сработал.
                // Здесь конвертируем "fail+firedTick" → "late".
                if (ev.status == "fail") ev.status = "late";
            } else {
                ev.calculatedMs = -1;
                ev.deviationMs  = -1;
            }
            continue;
        }

        if (ev.status == "ok") {
            ev.calculatedMs = static_cast<int>(ev.firedTick - syncIndex);
            ev.deviationMs  = qAbs(ev.calculatedMs - ev.time_ms);
            for (auto it = ev.channelTicks.constBegin(); it != ev.channelTicks.constEnd(); ++it)
                ev.channelCalcMs[it.key()] = static_cast<int>(it.value() - syncIndex);
        } else if (ev.status == "fail" && ev.firedTick != -1) {
            // Каналы сработали, но позже планового окна — опоздание старта или запаздывание события.
            // Пересчитываем относительно фактического T0.
            ev.status       = "late";
            ev.calculatedMs = static_cast<int>(ev.firedTick - syncIndex);
            ev.deviationMs  = qAbs(ev.calculatedMs - ev.time_ms);
            for (auto it = ev.channelTicks.constBegin(); it != ev.channelTicks.constEnd(); ++it)
                ev.channelCalcMs[it.key()] = static_cast<int>(it.value() - syncIndex);
        } else {
            ev.calculatedMs = -1;
            ev.deviationMs  = -1;
        }
    }
    return result;
}

// ─── stop ─────────────────────────────────────────────────────────────────────

void Stand::stop()
{
    m_stopped = true;

    QUdpSocket udp;
    if (udp.writeDatagram(QByteArray("STOP"), QHostAddress(BCVM_IP), UDP_PORT) == 4) {
        if (m_logger) m_logger->log("INFO", "Команда STOP отправлена на БЦВМ", m_finalIndex);
        emit logMessage("Команда STOP отправлена на БЦВМ", "system");
    } else {
        if (m_logger) m_logger->log("ERROR", "Ошибка отправки STOP: " + udp.errorString(), m_finalIndex);
        emit logMessage("Ошибка отправки STOP: " + udp.errorString(), "system");
    }

    m_running = false;
    if (m_worker.joinable()) m_worker.join();

    QVector<EventRow> snapshot;
    {
        QMutexLocker l(&m_eventsMutex);
        for (auto &ev : m_events) {
            if (ev.hasChannels && ev.status == "pending") {
                ev.status = "fail"; ev.firedTick = -1; ev.calculatedMs = -1; ev.deviationMs = -1;
            }
        }
        snapshot = m_events;
    }
    emit analysisDone(snapshot);

    updatePhase(Phase::Stopped);
    if (m_logger) m_logger->log("INFO", "═══ СТОП: задание прервано оператором ═══", m_finalIndex);
    emit logMessage("═══ СТОП: задание прервано оператором ═══", "system");
}

// ─── updateNextEvent ─────────────────────────────────────────────────────────

void Stand::updateNextEvent(int64_t absoluteIndex, int64_t timeToStartMs)
{
    NextEventInfo info;
    {
        QMutexLocker locker(&m_eventsMutex);
        int64_t nextTime = INT64_MAX;
        for (const auto &ev : m_events) {
            if (ev.status != "pending" || !ev.hasChannels) continue;
            const int64_t t = timeToStartMs + ev.time_ms;
            if (t > absoluteIndex && t < nextTime) {
                nextTime          = t;
                info.eventId      = ev.id;
                info.description  = ev.description.isEmpty() ? ev.key : ev.description;
            }
        }
        if (nextTime != INT64_MAX)
            info.msRemaining = nextTime - absoluteIndex;
    }
    emit nextEventChanged(info);
}

// ─── resetForNewTest ─────────────────────────────────────────────────────────

void Stand::resetForNewTest()
{
    if (m_running) { m_running = false; if (m_worker.joinable()) m_worker.join(); }
    m_syncFound = false; m_syncIndex = -1; m_finalIndex = 0; m_analysisDone = false; m_stopped = false;
    m_masks.clear();
    {
        QMutexLocker l(&m_eventsMutex);
        for (auto &ev : m_events) {
            ev.status = "pending"; ev.firedTick = -1; ev.calculatedMs = -1; ev.deviationMs = 0;
        }
    }
}

// ─── updatePhase ─────────────────────────────────────────────────────────────

void Stand::updatePhase(Phase newPhase)
{
    m_phase = newPhase;
    emit phaseChanged(newPhase);
}
