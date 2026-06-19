#include "stand.h"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QDir>
#include <QUdpSocket>
#include <QThread>
#include <QDateTime>
#include <QTimeZone>
#include <QDebug>
#include <QProcess>

struct MaskRecord {
    int64_t absoluteIndex;
    uint8_t mask;
};

Stand::Stand(QObject *parent)
    : QObject(parent)
    , m_phase(Phase::Idle)
    , m_flightDurationMs(0)
    , m_serial(nullptr)
    , m_running(false)
    , m_syncIndex(-1)
    , m_syncFound(false)
    , m_analysisDone(false)
    , m_timeToStartMs(0)
{
    m_mappings = {
        {"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12", 0x03, "1, 2"},
        {"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8",   0x1C, "3, 4, 5"},
        {"CLOSE_MAIN_VALVES_ENGINES_9_TO_12",   0x60, "6, 7"},
        {"LIFT_OFF_CONTACT",                    0x80, "8"}
    };

    // Попытка открыть COM-порт с повторами
    bool opened = false;
    for (int i = 0; i < RETRY_COUNT; ++i) {
        m_serial = new QSerialPort(SERIAL_PORT, this);
        m_serial->setBaudRate(SERIAL_BAUDRATE);
        m_serial->setDataBits(QSerialPort::Data8);
        m_serial->setParity(QSerialPort::NoParity);
        m_serial->setStopBits(QSerialPort::OneStop);
        m_serial->setFlowControl(QSerialPort::NoFlowControl);

        if (m_serial->open(QIODevice::ReadOnly)) {
            opened = true;
            emit logMessage("COM-порт " + QString(SERIAL_PORT) + " открыт", "system");
            break;
        } else {
            delete m_serial;
            m_serial = nullptr;
            if (i < RETRY_COUNT - 1) {
                QThread::msleep(500);
            }
        }
    }
    if (!opened) {
        emit logMessage("Не удалось открыть COM-порт после " + QString::number(RETRY_COUNT) + " попыток. Тест невозможен.", "system");
        emit portError("Ошибка COM-порта");
        // Кнопка "ЗАГРУЗИТЬ НА БОРТ" будет заблокирована в UI
    }

    // Подключаем сигнал завершения к слоту completeFlight (в главном потоке)
    connect(this, &Stand::flightComplete, this, &Stand::completeFlight, Qt::QueuedConnection);

    // Загружаем циклограмму
    loadCyclogram();
}

Stand::~Stand()
{
    m_running = false;
    if (m_worker.joinable())
        m_worker.join();
    delete m_serial;
}

bool Stand::loadCyclogram()
{
    QString exePath = QCoreApplication::applicationDirPath();
    QString filePath = exePath + QDir::separator() + CYCLOGRAM_FILE;
    QFile file(filePath);
    if (!file.exists()) {
        emit logMessage("Файл циклограммы не найден: " + filePath, "system");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit logMessage("Не удалось открыть файл: " + filePath, "system");
        return false;
    }

    m_events.clear();
    QTextStream stream(&file);
    int lineNum = 0;
    bool startTimeFound = false;
    bool setTimeFound = false;
    int maxTime = 0;


    while (!stream.atEnd()) {
        QString line = stream.readLine();
        lineNum++;
        line = line.trimmed();
        if (line.isEmpty()) continue;

        int commentPos = line.indexOf('#');
        QString comment;
        if (commentPos != -1) {
            comment = line.mid(commentPos + 1).trimmed();
            line = line.left(commentPos).trimmed();
        }
        if (line.isEmpty()) continue;

        int eqPos = line.indexOf('=');
        if (eqPos == -1) {
            emit logMessage(QString("Ошибка в строке %1: нет '='").arg(lineNum), "system");
            continue;
        }
        QString key = line.left(eqPos).trimmed();
        QString valueStr = line.mid(eqPos + 1).trimmed();

        if (key == "SET_UTC_TIME") {
            QTime setTime = QTime::fromString(valueStr, "hh:mm:ss");
            if (!setTime.isValid()) {
                emit logMessage("Неверный формат SET_UTC_TIME: " + valueStr, "system");
                return false;
            }
            m_setTime = setTime;
            setTimeFound = true;
            continue;
        }
        if (key == "START_UTC_TIME") {
            QTime startTime = QTime::fromString(valueStr, "hh:mm:ss");
            if (!startTime.isValid()) {
                emit logMessage("Неверный формат START_UTC_TIME: " + valueStr, "system");
                return false;
            }
            m_startTime = startTime;
            startTimeFound = true;
            continue;
        }

        bool ok;
        int timeMs = valueStr.toInt(&ok);
        if (!ok) {
            emit logMessage(QString("Неверное число в строке %1: %2").arg(lineNum).arg(valueStr), "system");
            continue;
        }

        QString channels = "—";
        bool hasChannels = false;
        for (const auto &m : m_mappings) {
            if (m.key == key) {
                channels = m.channelsStr;
                hasChannels = true;
                break;
            }
        }

        EventRow ev;
        ev.id = m_events.size() + 1;
        ev.key = key;
        ev.description = comment.isEmpty() ? key : comment;   // <-- сохраняем комментарий
        ev.time_ms = timeMs;
        ev.channels = channels;
        ev.hasChannels = hasChannels;
        ev.firedTick = -1;
        ev.calculatedMs = -1;
        ev.status = "pending";
        ev.deviationMs = 0;
        m_events.append(ev);
    }

    if (!startTimeFound || !setTimeFound) {
        emit logMessage("В файле не найдены обе строки SET_UTC_TIME и START_UTC_TIME", "system");
        return false;
    }
    if (m_events.isEmpty()) {
        emit logMessage("Циклограмма не содержит событий", "system");
        return false;
    }

    for (const auto &ev : m_events) {
        if (ev.hasChannels && ev.time_ms > maxTime) {
            maxTime = ev.time_ms;
        }
    }
    // Если все отслеживаемые события отрицательные, maxTime = 0
    if (maxTime < 0) maxTime = 0;

    m_flightDurationMs = maxTime + FLIGHT_SAFETY_MARGIN_MS;

    emit logMessage(QString("Загружено событий: %1").arg(m_events.size()), "system");
    emit logMessage(QString("Общая длительность полётного задания: %1 мс").arg(m_flightDurationMs), "system");
    emit logMessage(QString("Текущее лабораторное время (SET_UTC_TIME): %1").arg(m_setTime.toString("hh:mm:ss")), "system");
    emit logMessage(QString("Время старта (START_UTC_TIME): %1").arg(m_startTime.toString("hh:mm:ss")), "system");

    emit analysisDone(m_events);

    updatePhase(Phase::Loaded);
    return true;
}

bool Stand::setStartTimeFromUI(const QTime &time)
{
    if (!time.isValid()) {
        emit logMessage("Ошибка: введено неверное время", "system");
        return false;
    }

    // Записываем новое время в файл (заменяем только START_UTC_TIME)
    if (!writeStartTimeToFile(time)) {
        emit logMessage("Не удалось записать время старта в файл", "system");
        return false;
    }

    // Обновляем внутреннюю переменную
    m_startTime = time;
    emit logMessage(QString("Время старта установлено: %1").arg(time.toString("hh:mm:ss")), "system");

    // Пересчёт m_timeToStartMs не делаем, он будет вычислен при вызове sendToBoard()
    return true;
}

bool Stand::writeStartTimeToFile(const QTime &time)
{
    QString exePath = QCoreApplication::applicationDirPath();
    QString filePath = exePath + QDir::separator() + CYCLOGRAM_FILE;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) {
        emit logMessage("Не удалось открыть файл для записи: " + filePath, "system");
        return false;
    }

    QStringList lines;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.trimmed().startsWith("START_UTC_TIME", Qt::CaseInsensitive)) {
            line = "START_UTC_TIME = " + time.toString("hh:mm:ss");
        }
        // Строку SET_UTC_TIME не трогаем
        lines.append(line);
    }
    file.resize(0);
    QTextStream out(&file);
    for (const QString &line : lines) {
        out << line << "\n";
    }
    file.close();
    return true;
}

bool Stand::pingBcvm() const
{
#ifdef Q_OS_WIN
    QStringList args = {"-n", "1", BCVM_IP};
#else
    QStringList args = {"-c", "1", BCVM_IP};
#endif
    QProcess ping;
    ping.start("ping", args);
    if (!ping.waitForFinished(2000)) {
        ping.kill();
        ping.waitForFinished(500);
        return false;
    }
    if (ping.exitCode() != 0)
        return false;
    QString output = ping.readAllStandardOutput();
    return output.contains("TTL=") || output.contains("ttl=") || output.contains("time=");
}

void Stand::sendToBoard()
{
    if (m_running) {
        emit logMessage("Предыдущий тест ещё выполняется. Останавливаем...", "system");
        m_running = false;
        if (m_worker.joinable())
            m_worker.join();
    }

    if (!m_serial || !m_serial->isOpen()) {
        emit logMessage("COM-порт не открыт. Невозможно отправить циклограмму.", "system");
        return;
    }
    if (!pingBcvm()) {
        emit logMessage("ОШИБКА: ЯВ (БЦВМ) недоступен. Проверьте подключение к сети.", "system");
        return;   // <-- блокируем выполнение, ничего не делаем
    }

    // Вычисляем время до старта (разница между START_UTC_TIME и SET_UTC_TIME)
    int secsToStart = m_setTime.secsTo(m_startTime);
    if (secsToStart < 0) {
        secsToStart += 24 * 3600; // переход через сутки
    }
    m_timeToStartMs = static_cast<int64_t>(secsToStart) * 1000;

    // Проверка: время до старта должно быть больше 1 секунды
    if (m_timeToStartMs < 1000) {
        emit logMessage("Время до старта менее 1 секунды. Проверьте SET_UTC_TIME и START_UTC_TIME.", "system");
        return;
    }

    emit logMessage(QString("Время до старта: %1 секунд (%2 мс)").arg(secsToStart).arg(m_timeToStartMs), "system");

    // Формируем датаграмму из файла с заменой START_UTC_TIME на установленное время
    QString exePath = QCoreApplication::applicationDirPath();
    QString filePath = exePath + QDir::separator() + CYCLOGRAM_FILE;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit logMessage("Не удалось открыть файл для отправки", "system");
        return;
    }
    QStringList lines;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.trimmed().startsWith("START_UTC_TIME", Qt::CaseInsensitive)) {
            line = "START_UTC_TIME = " + m_startTime.toString("hh:mm:ss");
        }
        lines.append(line);
    }
    file.close();
    QString content = lines.join("\n");
    QByteArray datagram = content.toUtf8();

    // Отправка UDP
    QUdpSocket udpSocket;
    qint64 sent = udpSocket.writeDatagram(datagram, QHostAddress(BCVM_IP), UDP_PORT);
    if (sent == -1) {
        emit logMessage("Ошибка отправки UDP: " + udpSocket.errorString(), "system");
        return;
    } else {
        emit logMessage(QString("Циклограмма отправлена на БЦВМ (%1 байт)").arg(sent), "system");
    }


    // Сброс состояния
    m_masks.clear();
    m_syncFound = false;
    m_syncIndex = -1;
    m_analysisDone = false;

    // Очистка буфера COM-порта
    m_serial->clear(QSerialPort::AllDirections);
    emit logMessage("Буфер COM-порта очищен", "system");

    m_running = true;

    // Запуск потока чтения с m_timeToStartMs
    m_worker = std::thread(&Stand::readingThread, this, m_timeToStartMs);
    updatePhase(Phase::Countdown);
}

void Stand::readingThread(int64_t timeToStartMs)
{
    int64_t absoluteIndex = 0;
    uint8_t firedBits = 0;
    bool started = false;

    while (m_running) {
        if (!m_serial->waitForReadyRead(100)) {
            if (!m_serial->isOpen() || m_serial->error() != QSerialPort::NoError) {
                emit portError(QString("COM-порт отключён: %1").arg(m_serial->errorString()));
                m_running = false;
                break;
            }
            continue;
        }
        char byte = 0;
        if (m_serial->read(&byte, 1) != 1) continue;

        uint8_t mask = static_cast<uint8_t>(byte);
        m_masks.push_back({absoluteIndex, mask});

        // Обновление таймера каждые 1000 байт (1 секунда)
        if (absoluteIndex % 1000 == 0) {
            if (!started && absoluteIndex < timeToStartMs) {
                // Обратный отсчёт
                int64_t remaining = timeToStartMs - absoluteIndex;
                int secs = remaining / 1000;
                QString timeStr = QString("%1:%2")
                                      .arg(secs/60, 2, 10, QChar('0'))
                                      .arg(secs%60, 2, 10, QChar('0'));
                emit timerUpdated(timeStr, "#e3b341");

                // Вычисляем время до следующего события (включая отрицательные)
                updateNextEvent(absoluteIndex, timeToStartMs);
            } else if (started) {
                // Время полёта
                int64_t flightMs = absoluteIndex - timeToStartMs;
                int secs = flightMs / 1000;
                int ms = flightMs % 1000;
                QString timeStr = QString("%1:%2.%3")
                                      .arg(secs/60, 2, 10, QChar('0'))
                                      .arg(secs%60, 2, 10, QChar('0'))
                                      .arg(ms/100, 1, 10, QChar('0'));
                emit timerUpdated(timeStr, "#3fb950");

                // Вычисляем время до следующего события
                updateNextEvent(absoluteIndex, timeToStartMs);
            }
        }

        // Проверка наступления старта
        if (!started && absoluteIndex >= timeToStartMs) {
            started = true;
            emit logMessage("─── СТАРТ ───", "system");
            updatePhase(Phase::Running);
        }

        // Обработка масок (срабатывания каналов)
        uint8_t newBits = mask & ~firedBits;
        if (newBits != 0) {
            // Канал 8 – синхросигнал (Контакт подъёма)
            if (newBits & SYNC_MASK) {
                emit logMessage(QString("[%1] Контакт подъёма, начало движения").arg(absoluteIndex), "event");
                m_syncIndex = absoluteIndex;
                m_syncFound = true;
                newBits &= ~SYNC_MASK;
                firedBits |= SYNC_MASK;
            }

            // Остальные каналы
            if (newBits != 0) {
                QStringList channels;
                if (newBits & 0x01) channels << "1";
                if (newBits & 0x02) channels << "2";
                if (newBits & 0x04) channels << "3";
                if (newBits & 0x08) channels << "4";
                if (newBits & 0x10) channels << "5";
                if (newBits & 0x20) channels << "6";
                if (newBits & 0x40) channels << "7";

                QString msg = QString("[%1] срабатывание канала(ов): %2")
                                  .arg(absoluteIndex)
                                  .arg(channels.join(", "));
                emit logMessage(msg, "event");
                firedBits |= newBits;
            }
        }

        // Проверка событий циклограммы (универсальная логика)
        QMutexLocker locker(&m_eventsMutex);
        for (int i = 0; i < m_events.size(); ++i) {
            EventRow &ev = m_events[i];
            if (!ev.hasChannels || ev.status != "pending") continue;

            int64_t eventAbsIndex = timeToStartMs + ev.time_ms;

            if (absoluteIndex >= eventAbsIndex) {
                // Найти маску для этого события
                uint8_t neededMask = 0;
                for (const auto &m : m_mappings) {
                    if (m.key == ev.key) {
                        neededMask = m.mask;
                        break;
                    }
                }
                if (neededMask == 0) continue; // без маски не отслеживаем

                // Проверить, появились ли все нужные биты
                if ((firedBits & neededMask) == neededMask) {
                    ev.status = "ok";
                    ev.firedTick = absoluteIndex;
                    emit eventFired(ev.id, ev.firedTick);
                    emit logMessage(QString("[%1] событие '%2' выполнено (каналы %3)")
                                        .arg(ev.firedTick).arg(ev.key).arg(ev.channels), "event");
                } else if (absoluteIndex >= eventAbsIndex + FAIL_MARGIN_MS) { // задержка FAIL_MARGIN_MS мс
                    ev.status = "fail";
                    ev.firedTick = -1;
                    emit eventFailed(ev.id);
                    emit logMessage(QString("[%1] событие '%2' НЕ СРАБОТАЛО (каналы %3)")
                                        .arg(absoluteIndex).arg(ev.key).arg(ev.channels), "event-post");
                }
            }
        }

        absoluteIndex++;

        // Проверка завершения полёта по таймауту
        if (started && (absoluteIndex - timeToStartMs) > m_flightDurationMs + 100) {
            emit logMessage("Достигнут конец полётного задания, остановка сбора", "system");
            m_running = false;
            break;
        }
    }

    // Если синхросигнал найден, выполняем анализ
    if (m_syncFound && !m_analysisDone) {
        emit flightComplete();
    } else if (!m_syncFound && started) {
        // Синхросигнал не найден, но старт наступил – выполняем анализ с предупреждением
        emit logMessage("ВНИМАНИЕ: Синхроимпульс (канал 8) не найден. Анализ будет выполнен с использованием расчётного времени старта.", "system");
        m_syncIndex = timeToStartMs; // Используем расчётное время как синхроимпульс
        m_syncFound = true; // Принудительно считаем, что синхроимпульс есть
       emit flightComplete();
    } else {
        emit logMessage("Полёт прерван до старта или ошибка. Анализ невозможен.", "system");
        updatePhase(Phase::Stopped);
    }
}

void Stand::completeFlight()
{
    updatePhase(Phase::Completed);
    emit logMessage("═══ Полётное задание завершено ═══", "system");

    if (!m_syncFound) {
        emit logMessage("ВНИМАНИЕ: Синхроимпульс (канал 8) не найден. Используется расчётное время старта.", "system");
        m_syncIndex = m_timeToStartMs;
        m_syncFound = true;
    }

    performAnalysis(m_timeToStartMs);
    m_analysisDone = true;
}

void Stand::performAnalysis(int64_t timeToStartMs)
{
    if (!m_syncFound) {
        emit logMessage("Синхронизация не найдена, анализ невозможен", "system");
        return;
    }
    int64_t baseTime = m_syncFound ? m_syncIndex : timeToStartMs;

    {
        QMutexLocker locker(&m_eventsMutex);
        for (auto &ev : m_events) {
            if (ev.status == "ok") {
                ev.calculatedMs = ev.firedTick - baseTime;
                ev.deviationMs = qAbs(ev.calculatedMs - ev.time_ms);
            } else if (ev.status == "fail") {
                ev.calculatedMs = -1;
                ev.deviationMs = -1;
            } else {
                ev.deviationMs = -1;
            }
        }
    }
    emit analysisDone(m_events);

    // Лог с цветовой индикацией (используем описание)
    for (const auto &ev : m_events) {
        if (ev.calculatedMs != -1) {
            QString color;
            if (ev.deviationMs == 0) color = "#3fb950";
            else if (ev.deviationMs <= 5) color = "#e3b341";
            else color = "#f85149";
            QString msg = QString("[%1 мс] %2 (отклонение %3 мс)")
                              .arg(ev.calculatedMs)
                              .arg(ev.description.isEmpty() ? ev.key : ev.description)
                              .arg(ev.deviationMs);
            emit logMessage(msg, "event-post");
        } else {
            QString msg = QString("[%1] НЕ СРАБОТАЛО").arg(ev.description.isEmpty() ? ev.key : ev.description);
            emit logMessage(msg, "event-post");
        }
    }
}

void Stand::stop()
{
    // Отправка команды STOP по UDP
    QUdpSocket udpSocket;
    char stopCmd = 99;
    QByteArray datagram(&stopCmd, 1);
    qint64 sent = udpSocket.writeDatagram(datagram, QHostAddress(BCVM_IP), UDP_PORT);
    if (sent == 1) {
        emit logMessage("Команда STOP отправлена на БЦВМ", "system");
    } else {
        emit logMessage("Ошибка отправки команды STOP: " + udpSocket.errorString(), "system");
    }

    m_running = false;
    if (m_worker.joinable())
        m_worker.join();

    // Обновляем статусы для всех событий, которые ещё в ожидании
    // Обновляем статусы событий под мьютексом
    {
        QMutexLocker locker(&m_eventsMutex);
        for (auto &ev : m_events) {
            if (ev.hasChannels && ev.status == "pending") {
                ev.status = "fail";
                ev.firedTick = -1;
                ev.calculatedMs = -1;
                ev.deviationMs = -1;
            }
        }
    }
    // Отправляем обновлённый список в UI
    emit analysisDone(m_events);

    // Переключаем фазу
    if (m_phase == Phase::Countdown || m_phase == Phase::Running) {
        updatePhase(Phase::Stopped);
        emit logMessage("═══ СТОП: задание прервано оператором ═══", "system");
    } else {
        updatePhase(Phase::Idle);
    }
}

void Stand::updatePhase(Phase newPhase)
{
    m_phase = newPhase;
    emit phaseChanged(newPhase);
}

void Stand::updateNextEvent(int64_t absoluteIndex, int64_t timeToStartMs)
{
    QMutexLocker locker(&m_eventsMutex);
    int64_t nextEventTime = INT64_MAX;
    QString nextEventKey;

    for (const auto &ev : m_events) {
        if (ev.status != "pending" || !ev.hasChannels) continue;
        int64_t eventAbsIndex = timeToStartMs + ev.time_ms;
        if (eventAbsIndex > absoluteIndex && eventAbsIndex < nextEventTime) {
            nextEventTime = eventAbsIndex;
            nextEventKey = ev.description.isEmpty() ? ev.key : ev.description;
        }
    }

    if (nextEventTime != INT64_MAX) {
        int64_t remaining = nextEventTime - absoluteIndex;
        int secs = remaining / 1000;
        int ms = (remaining % 1000) / 100;
        QString timeStr = QString("%1:%2.%3")
                              .arg(secs/60, 2, 10, QChar('0'))
                              .arg(secs%60, 2, 10, QChar('0'))
                              .arg(ms, 1, 10, QChar('0'));
        emit nextEventTimer(timeStr + " (" + nextEventKey + ")");
    } else {
        emit nextEventTimer("--:-- (нет событий)");
    }
}

void Stand::resetForNewTest()
{
    // Остановить текущий поток, если работает
    if (m_running) {
        m_running = false;
        if (m_worker.joinable())
            m_worker.join();
    }
    // Сброс флагов
    m_syncFound = false;
    m_syncIndex = -1;
    m_analysisDone = false;
    m_masks.clear(); // если оставили
    // Сброс событий (очистить статусы)
    {
        QMutexLocker locker(&m_eventsMutex);
        for (auto &ev : m_events) {
            ev.status = "pending";
            ev.firedTick = -1;
            ev.calculatedMs = -1;
            ev.deviationMs = 0;
        }
    }
}
