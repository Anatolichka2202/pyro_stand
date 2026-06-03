#include "stand.h"
#include <QSerialPort>
#include <QDateTime>
#include <QThread>
#include <QDebug>
#include <QUdpSocket>
#include <QtGlobal>

// Структура для UDP-пакета (как раньше)
struct StartMessage {
    uint8_t command;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
};

Stand::Stand(QObject* parent)
    : QObject(parent), running(false), syncIndex(-1), syncFound(false) {
    for (int i = 0; i < g_cyclogramSize; ++i)
        events.append(g_cyclogram[i]);

    serial = new QSerialPort("COM5", this);
    serial->setBaudRate(QSerialPort::Baud115200);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);
}

Stand::~Stand() {
    stop();
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

bool Stand::start() {
    if (!serial->open(QIODevice::ReadOnly)) {
        emit logMessage("Ошибка открытия COM-порта COM3");
        return false;
    }
    // Очищаем данные предыдущего запуска
    masks.clear();
    syncFound = false;
    syncIndex = -1;

    sendUdpToBcvm();  // обязательно отправляем UDP с запланированным временем

    running = true;
    worker = std::thread(&Stand::readingThread, this);
    return true;
}

void Stand::stop() {
    running = false;
    if (worker.joinable())
        worker.join();
    serial->close();
    performAnalysis();  // после остановки проводим анализ
    emit logMessage("Стенд остановлен, анализ выполнен");
}

void Stand::readingThread() {
    int64_t absoluteIndex = 0;
    uint8_t lastLoggedMask = 0;
    while (running) {
        char byte;
        qint64 bytesRead = serial->read(&byte, 1);
        if (bytesRead != 1) {
            QThread::msleep(1);
            continue;
        }

        uint8_t mask = static_cast<uint8_t>(byte);
        masks.append({absoluteIndex, mask});

        // Синхронизация по каналу 8 (бит 7)
        if (!syncFound && (mask & 0x80)) {
            syncFound = true;
            syncIndex = absoluteIndex;
            emit logMessage(QString("Синхронизация: канал 8 сработал на индексе %1 (0 мс по циклограмме)").arg(absoluteIndex));
            emit logMessage(QString("[0 мс] Контакт подъёма, начало движения"));
            lastLoggedMask = mask;
        }

        if (syncFound) {
            int64_t relTime = absoluteIndex - syncIndex;
            // Логируем изменения маски (кроме самого синхросигнала, который уже выведен)
            if (mask != lastLoggedMask) {
                if (mask == 0) {
                    emit logMessage(QString("[%1 мс] Маска сброшена (0x00)").arg(relTime));
                } else {
                    emit logMessage(QString("[%1 мс] Маска изменилась: 0x%2")
                                        .arg(relTime).arg(mask, 2, 16, QChar('0')));
                }
                lastLoggedMask = mask;
            }
            // Каждые 100 мс выводим такт тестера (можно закомментировать, если не нужно)
            if (relTime % 100 == 0 && relTime != 0) {
                emit logMessage(QString("[%1 мс] Такт тестера").arg(relTime));
            }
        }

        absoluteIndex++;

        // Автоматическая остановка после завершения полётного задания + запас
        if (syncFound && (absoluteIndex - syncIndex) > totalDurationMs + 100) {
            emit logMessage("Достигнут конец полётного задания, остановка сбора");
            running = false;
            break;
        }
    }
}

void Stand::sendUdpToBcvm() {
    QUdpSocket udpSocket;
    StartMessage msg;
    msg.command = 0x03;
    QDateTime utc = startUtcDateTime;
    msg.hour = utc.time().hour();
    msg.min = utc.time().minute();
    msg.sec = utc.time().second();
    QByteArray datagram((const char*)&msg, sizeof(msg));
    udpSocket.writeDatagram(datagram, QHostAddress("192.168.17.246"), 4000);
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

    // 1. Все события циклограммы (ожидаемые)
    for (const auto& ev : events) {
        int64_t relTimeMs = ev.time_ms;   // время от старта (может быть отрицательным)
        QDateTime eventUtc = startUtc.addMSecs(relTimeMs);
        QString utcStr = eventUtc.toString("hh:mm:ss.zzz");
        emit logMessage(QString("[%1][%2] %3")
                            .arg(utcStr)
                            .arg(relTimeMs)
                            .arg(ev.description));
    }

    // 2. Фактические подрывы пиросредств (ненулевые маски, не синхросигнал)
    for (const auto& rec : masks) {
        int64_t relTime = rec.absoluteIndex - syncIndex;
        if (rec.mask == 0) continue;
        if (relTime < -10000 || relTime > totalDurationMs + 1000) continue; // отсекаем шум
        if (rec.mask == 0x80) continue; // чистый синхросигнал

        QDateTime eventUtc = startUtc.addMSecs(relTime);
        QString utcStr = eventUtc.toString("hh:mm:ss.zzz");
        QString hexMask = QString("0x%1").arg(rec.mask, 2, 16, QChar('0'));
        // Формат: [UTC][такт_относительный][такт_циклограммы] подрыв пиросредств {маска} *фактический*
        // Третий параметр – такт циклограммы (ожидаемый), но так как мы не сопоставляем с конкретным событием,
        // выводим тот же относительный такт.
        emit logMessage(QString("[%1][%2][%2] подрыв пиросредств {%3} *фактический*")
                            .arg(utcStr)
                            .arg(relTime)
                            .arg(hexMask));
    }

    emit analysisComplete(masks, syncIndex);
}
