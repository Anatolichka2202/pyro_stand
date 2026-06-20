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

Stand::Stand(QObject *parent, const QString &portName, std::unique_ptr<ISerialPort> port,
             SessionLogger *logger)
    : QObject(parent), m_portName(portName), m_logger(logger)
{
    m_mappings = {
        {"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12", 0x03, "1, 2"},
        {"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8",  0x1C, "3, 4, 5"},
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
            m_port->close(); // закроем — откроем в readingThread
            emit logMessage("COM-порт " + portName + " доступен", "system");
        } else {
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

    // Записываем заголовок сессии в лог-файл
    if (m_logger) {
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
        emit logMessage("Время до старта менее 1 с. Проверьте SET/START_UTC_TIME.", "system");
        return;
    }
    emit logMessage(QString("Время до старта: %1 с (%2 мс)").arg(secsToStart).arg(m_timeToStartMs), "system");

    if (!mockMode) {
        // Отправляем файл циклограммы по UDP
        const QString path = cyclogramFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
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

        const QByteArray datagram = lines.join("\n").toUtf8();
        QUdpSocket udp;
        const qint64 sent = udp.writeDatagram(datagram, QHostAddress(BCVM_IP), UDP_PORT);
        if (sent == -1) { emit logMessage("Ошибка UDP: " + udp.errorString(), "system"); return; }
        emit logMessage(QString("Циклограмма отправлена на БЦВМ (%1 байт)").arg(sent), "system");

        m_port->clearBuffers();
    }

    m_masks.clear();
    m_syncFound   = false;
    m_syncIndex   = -1;
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

    int64_t absoluteIndex = 0;
    uint8_t firedBits = 0;
    bool started = false;

    while (m_running) {
        if (!m_port->waitForReadyRead(100)) {
            if (!m_port->isOpen() || m_port->hasError()) {
                const QString errMsg = QString("COM-порт отключён: %1").arg(m_port->errorString());
                if (m_logger) m_logger->log("ERROR", errMsg);
                emit portError(errMsg);
                m_running = false;
                break;
            }
            continue;
        }

        char byte = 0;
        if (m_port->read(&byte, 1) != 1) continue;

        const uint8_t mask = static_cast<uint8_t>(byte);
        m_masks.push_back({absoluteIndex, mask});

        // Таймер (раз в секунду)
        if (absoluteIndex % 1000 == 0) {
            if (!started && absoluteIndex < timeToStartMs) {
                emit timerTick(TimerState{-(timeToStartMs - absoluteIndex), m_phase});
                updateNextEvent(absoluteIndex, timeToStartMs);
            } else if (started) {
                emit timerTick(TimerState{absoluteIndex - timeToStartMs, m_phase});
                updateNextEvent(absoluteIndex, timeToStartMs);
            }
        }

        if (!started && absoluteIndex >= timeToStartMs) {
            started = true;
            if (m_logger) m_logger->log("INFO", "─── СТАРТ ───");
            emit logMessage("─── СТАРТ ───", "system");
            updatePhase(Phase::Running);
        }

        // Обработка входящих бит-масок
        uint8_t newBits = mask & ~firedBits;
        if (newBits & SYNC_MASK) {
            emit logMessage(QString("[%1] Контакт подъёма (канал 8)").arg(absoluteIndex), "event");
            m_syncIndex = absoluteIndex;
            m_syncFound = true;
            newBits    &= ~SYNC_MASK;
            firedBits  |= SYNC_MASK;
        }
        if (newBits != 0) {
            QStringList chs;
            for (int b = 0; b < 7; ++b) if (newBits & (1 << b)) chs << QString::number(b + 1);
            emit logMessage(QString("[%1] срабатывание канала(ов): %2").arg(absoluteIndex).arg(chs.join(", ")), "event");
            firedBits |= newBits;
        }

        // Проверка событий циклограммы
        // Сначала собираем результаты под мьютексом, потом эмитим — вне мьютекса
        struct PendingSignal { bool fired; int id; int tick; QString key; int planMs; };
        QVector<PendingSignal> pending;
        {
            QMutexLocker locker(&m_eventsMutex);
            for (auto &ev : m_events) {
                if (!ev.hasChannels || ev.status != "pending") continue;
                const int64_t expectedIdx = timeToStartMs + ev.time_ms;
                if (absoluteIndex < expectedIdx) continue;

                uint8_t neededMask = 0;
                for (const auto &m : m_mappings) { if (m.key == ev.key) { neededMask = m.mask; break; } }
                if (neededMask == 0) continue;

                if ((firedBits & neededMask) == neededMask) {
                    ev.status   = "ok";
                    ev.firedTick= static_cast<int>(absoluteIndex);
                    pending.push_back({true, ev.id, ev.firedTick, ev.key, ev.time_ms});
                    emit logMessage(QString("[%1] '%2' выполнено (каналы %3)")
                                        .arg(absoluteIndex).arg(ev.key).arg(ev.channels), "event");
                } else if (absoluteIndex >= expectedIdx + FAIL_MARGIN_MS) {
                    ev.status   = "fail";
                    ev.firedTick= -1;
                    pending.push_back({false, ev.id, -1, ev.key, ev.time_ms});
                    emit logMessage(QString("[%1] '%2' НЕ СРАБОТАЛО (каналы %3)")
                                        .arg(absoluteIndex).arg(ev.key).arg(ev.channels), "event-post");
                }
            }
        }
        // Эмитим вне мьютекса — исключаем deadlock и гарантируем доставку в GUI-тред
        for (const auto &p : pending) {
            if (p.fired) {
                if (m_logger) {
                    // actual_ms и dev_ms будут уточнены после анализа; здесь — tick-based
                    const int actualMs = p.tick;
                    const int devMs    = qAbs(actualMs - (static_cast<int>(timeToStartMs) + p.planMs));
                    m_logger->log("EVENT",
                        QString("id=%1 key=%2 tick=%3 plan_ms=%4 actual_ms=%5 dev_ms=%6 status=OK")
                            .arg(p.id).arg(p.key).arg(p.tick).arg(p.planMs).arg(actualMs).arg(devMs));
                }
                emit eventFired(p.id, p.tick);
            } else {
                if (m_logger) {
                    m_logger->log("EVENT",
                        QString("id=%1 key=%2 tick=-1 plan_ms=%3 actual_ms=-1 dev_ms=-1 status=FAIL")
                            .arg(p.id).arg(p.key).arg(p.planMs));
                }
                emit eventFailed(p.id);
            }
        }

        ++absoluteIndex;

        if (started && (absoluteIndex - timeToStartMs) > m_flightDurationMs + 100) {
            m_running = false;
            break;
        }
    }

    m_port->close();

    if (m_syncFound && !m_analysisDone) {
        emit flightComplete();
    } else if (!m_syncFound && started) {
        emit logMessage("ВНИМАНИЕ: синхроимпульс (канал 8) не найден. Используется расчётное T0.", "system");
        m_syncIndex = timeToStartMs;
        m_syncFound = true;
        emit flightComplete();
    } else {
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
    emit logMessage("═══ Полётное задание завершено ═══", "system");

    if (!m_syncFound) { m_syncIndex = m_timeToStartMs; m_syncFound = true; }

    QVector<EventRow> snapshot;
    { QMutexLocker l(&m_eventsMutex); snapshot = m_events; }

    const QVector<EventRow> result = analyzeEvents(snapshot, m_syncIndex);

    { QMutexLocker l(&m_eventsMutex); m_events = result; }

    for (const auto &ev : result) {
        if (ev.calculatedMs != -1) {
            const QString color = (ev.deviationMs == 0) ? "#3fb950" : (ev.deviationMs <= 5) ? "#e3b341" : "#f85149";
            emit logMessage(QString("[%1 мс] %2 (откл. %3 мс)")
                                .arg(ev.calculatedMs).arg(ev.description).arg(ev.deviationMs), "event-post");
            Q_UNUSED(color)
        } else {
            emit logMessage(QString("[—] %1 НЕ СРАБОТАЛО").arg(ev.description), "event-post");
        }
    }
    emit analysisDone(result);
    m_analysisDone = true;
}

// ─── analyzeEvents (static, pure) ────────────────────────────────────────────

QVector<EventRow> Stand::analyzeEvents(const QVector<EventRow> &events, int64_t syncIndex)
{
    QVector<EventRow> result = events;
    for (auto &ev : result) {
        if (ev.status == "ok") {
            ev.calculatedMs = static_cast<int>(ev.firedTick - syncIndex);
            ev.deviationMs  = qAbs(ev.calculatedMs - ev.time_ms);
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
    const char stopCmd = 99;
    if (udp.writeDatagram(QByteArray(&stopCmd, 1), QHostAddress(BCVM_IP), UDP_PORT) == 1)
        emit logMessage("Команда STOP отправлена на БЦВМ", "system");
    else
        emit logMessage("Ошибка отправки STOP: " + udp.errorString(), "system");

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
    m_syncFound = false; m_syncIndex = -1; m_analysisDone = false; m_stopped = false;
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
