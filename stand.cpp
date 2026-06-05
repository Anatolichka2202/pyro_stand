#include "stand.h"
#include <QSerialPort>
#include <QDateTime>
#include <QThread>
#include <QDebug>
#include <QUdpSocket>
#include <QtGlobal>
#include <algorithm>

struct StartMessage {
    uint8_t command;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
};

Stand::Stand(QObject* parent)
    : QObject(parent)
    , running(false)
    , syncIndex(-1)
    , syncFound(false)
    , analysisDone(false)
{
    for (int i = 0; i < g_cyclogramSize; ++i)
        events.append(g_cyclogram[i]);

    serial = new QSerialPort(SERIAL_PORT, this);
    serial->setBaudRate(SERIAL_BAUDRATE);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);

    // Открываем порт при старте программы
    if (!serial->open(QIODevice::ReadOnly)) {
        emit logMessage("Ошибка открытия COM-порта " + QString(SERIAL_PORT));
    } else {
        emit logMessage("COM-порт " + QString(SERIAL_PORT) + " открыт, ожидание команды ПУСК");
    }
}

Stand::~Stand() {
    shutdown();
    delete serial;
}

void Stand::setStartMskTime(const QTime& mskTime) {
    startMskTime = mskTime;
    QDate currentDate = QDate::currentDate();
    QDateTime mskDateTime(currentDate, mskTime, Qt::LocalTime);
    if (mskDateTime <= QDateTime::currentDateTime())
        mskDateTime = mskDateTime.addDays(1);
    startUtcDateTime = mskDateTime.toUTC();
}

void Stand::shutdown() {
    doStop(false);
}

void Stand::stop() {
    doStop(true);
}

void Stand::doStop(bool withAnalysis) {
    running = false;
    if (worker.joinable())
        worker.join();

    if (withAnalysis && !analysisDone) {
        performAnalysis();
        analysisDone = true;
        emit logMessage("Стенд остановлен, анализ выполнен");
    }
}

bool Stand::start() {
    if (!serial->isOpen()) {
        emit logMessage("COM-порт не открыт. Невозможно запустить сбор.");
        return false;
    }

    // Сброс состояния перед новым пуском
    masks.clear();
    syncFound = false;
    syncIndex = -1;
    analysisDone = false;

    // Очистка буфера приёма – игнорируем всё, что пришло до пуска
    serial->clear(QSerialPort::AllDirections);
    emit logMessage("Буфер COM-порта очищен, начинаем сбор данных");

    sendUdpToBcvm();

    running = true;
    worker = std::thread(&Stand::readingThread, this);
    return true;
}

void Stand::readingThread() {
    int64_t absoluteIndex = 0;   // счётчик тактов от момента пуска
    uint8_t lastLoggedMask = 0;

    while (running) {
        char byte;
        qint64 bytesRead = serial->read(&byte, 1);
        if (bytesRead == -1) {
            emit logMessage("Ошибка чтения из COM-порта, останов сбора");
            running = false;
            break;
        }
        if (bytesRead != 1) {
            QThread::msleep(1);
            continue;
        }

        uint8_t mask = static_cast<uint8_t>(byte);
        masks.append({absoluteIndex, mask});

        // Синхронизация по каналу 8 (бит 7)
        if (!syncFound && (mask & SYNC_MASK)) {
            syncFound = true;
            syncIndex = absoluteIndex;
            emit logMessage(QString("Синхронизация: канал 8 сработал на индексе %1 (0 мс по циклограмме)").arg(absoluteIndex));
            emit logMessage(QString("[0 мс] Контакт подъёма, начало движения"));
            lastLoggedMask = mask;
        }

        if (syncFound) {
            int64_t relTime = absoluteIndex - syncIndex;
            // Логируем изменения маски
            if (mask != lastLoggedMask) {
                if (mask == 0) {
                    emit logMessage(QString("[%1 мс] Маска сброшена (0x00)").arg(relTime));
                } else {
                    emit logMessage(QString("[%1 мс] Маска изменилась: 0x%2")
                                        .arg(relTime).arg(mask, 2, 16, QChar('0')));
                }
                lastLoggedMask = mask;
            }
        }

        absoluteIndex++;

        // Автоматическая остановка после завершения полётного задания + запас
        if (syncFound && (absoluteIndex - syncIndex) > TOTAL_DURATION_MS + 100) {
            emit logMessage("Достигнут конец полётного задания, остановка сбора");
            running = false;
            break;
        }
    }
}

void Stand::sendUdpToBcvm() {
    QUdpSocket udpSocket;
    StartMessage msg;
    msg.command = UDP_COMMAND_START;
    QDateTime utc = startUtcDateTime;
    msg.hour = utc.time().hour();
    msg.min = utc.time().minute();
    msg.sec = utc.time().second();
    QByteArray datagram((const char*)&msg, sizeof(msg));
    udpSocket.writeDatagram(datagram, QHostAddress(BCVM_IP), UDP_PORT);
    emit logMessage(QString("Старт передан на БЦВМ: UTC %1:%2:%3")
                        .arg(msg.hour, 2, 10, QChar('0'))
                        .arg(msg.min, 2, 10, QChar('0'))
                        .arg(msg.sec, 2, 10, QChar('0')));
}

void Stand::performAnalysis() {
    if (!syncFound) {
        emit logMessage("Ошибка: синхроимпульс не найден. Анализ невозможен.");
        return;
    }

    QDateTime startUtc = startUtcDateTime;



    // События циклограммы
    for (const auto& ev : events) {
        int64_t relTimeMs = ev.time_ms;
        QDateTime eventUtc = startUtc.addMSecs(relTimeMs);
        QString utcStr = eventUtc.toString("hh:mm:ss.zzz");
        QString text = QString("[%1][%2] %3")
                           .arg(utcStr)
                           .arg(relTimeMs)
                           .arg(ev.description);
        entries.push_back({relTimeMs, text});
    }

    // Фактические маски (ненулевые, не синхросигнал)
    for (const auto& rec : masks) {
        int64_t relTime = rec.absoluteIndex - syncIndex;
        if (rec.mask == 0) continue;
        if (rec.mask == SYNC_MASK) continue;
        if (relTime < -10000 || relTime > TOTAL_DURATION_MS + 1000) continue;

        QDateTime eventUtc = startUtc.addMSecs(relTime);
        QString utcStr = eventUtc.toString("hh:mm:ss.zzz");
        QString hexMask = QString("0x%1").arg(rec.mask, 2, 16, QChar('0'));
        QString text = QString("[%1][%2] маска <font color='red'>%3</font>")
                           .arg(utcStr)
                           .arg(relTime)
                           .arg(hexMask);
        entries.push_back({relTime, text});
    }

    // Сортировка по времени
    std::sort(entries.begin(), entries.end(),
              [](const LogEntry& a, const LogEntry& b) {
                  return a.relTime < b.relTime;
              });

    for (const auto& e : entries) {
        emit logMessage(e.text);
    }

    emit analysisComplete(masks, syncIndex);
}

int64_t Stand::getRelTime(int64_t absIndex) const {
    if (!syncFound) return INT64_MIN;
    return absIndex - syncIndex;
}

QDateTime Stand::utcFromRelTime(int64_t relTime) const {
    return startUtcDateTime.addMSecs(relTime);
}

void Stand::addEventEntries(QVector<LogEntry>& entries) const {
    for (const auto& ev : events) {
        int64_t relTime = ev.time_ms;
        QDateTime utc = utcFromRelTime(relTime);
        QString text = QString("[%1][%2] %3")
                           .arg(utc.toString("hh:mm:ss.zzz"))
                           .arg(relTime)
                           .arg(ev.description);
        entries.push_back({relTime, text});
    }
}

void Stand::addMaskEntries(QVector<LogEntry>& entries) const {
    for (const auto& rec : masks) {
        int64_t relTime = getRelTime(rec.absoluteIndex);
        if (relTime == INT64_MIN) continue; // синхронизации ещё нет
        if (rec.mask == 0) continue;
        if (rec.mask == SYNC_MASK) continue;
        if (relTime < -10000 || relTime > TOTAL_DURATION_MS + 1000) continue;

        QDateTime utc = utcFromRelTime(relTime);
        QString hexMask = QString("0x%1").arg(rec.mask, 2, 16, QChar('0'));
        QString text = QString("[%1][%2] маска <font color='red'>%3</font>")
                           .arg(utc.toString("hh:mm:ss.zzz"))
                           .arg(relTime)
                           .arg(hexMask);
        entries.push_back({relTime, text});
    }
}

void Stand::sortAndEmit(QVector<LogEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const LogEntry& a, const LogEntry& b) {
                  return a.relTime < b.relTime;
              });
    for (const auto& e : entries) {
        emit logMessage(e.text);
    }
}

/*void Stand::performAnalysis() {
    if (!syncFound) {
        emit logMessage("Ошибка: синхроимпульс не найден. Анализ невозможен.");
        return;
    }
    QVector<LogEntry> entries;
    addEventEntries(entries);
    addMaskEntries(entries);
    sortAndEmit(entries);
    emit analysisComplete(masks, syncIndex);
}*/

