#include "stand.h"
#include <QSerialPort>
#include <QDateTime>
#include <QThread>
#include <QUdpSocket>
#include <algorithm>
#include <QDebug>

struct StartMessage {
    uint8_t command;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
};

// --------------------------------------------------------------
// Конструктор / деструктор
// --------------------------------------------------------------
Stand::Stand(QObject* parent)
    : QObject(parent)
    , running(false)
    , syncIndex(-1)
    , syncFound(false)
    , analysisDone(false)
{
    // Копируем циклограмму
    for (int i = 0; i < g_cyclogramSize; ++i)
        events.append(g_cyclogram[i]);

    // Настройка последовательного порта
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

// --------------------------------------------------------------
// Управление временем старта
// --------------------------------------------------------------
void Stand::setStartMskTime(const QTime& mskTime) {
    startMskTime = mskTime;
    QDate currentDate = QDate::currentDate();
    QDateTime mskDateTime(currentDate, mskTime, Qt::LocalTime);
    if (mskDateTime <= QDateTime::currentDateTime())
        mskDateTime = mskDateTime.addDays(1);
    startUtcDateTime = mskDateTime.toUTC();
}

// --------------------------------------------------------------
// Остановка (тихая / с анализом)
// --------------------------------------------------------------
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

// --------------------------------------------------------------
// Запуск сбора
// --------------------------------------------------------------
bool Stand::start() {
    if (!serial->isOpen()) {
        emit logMessage("COM-порт не открыт. Невозможно запустить сбор.");
        return false;
    }

    // Сброс состояния
    masks.clear();
    syncFound = false;
    syncIndex = -1;
    analysisDone = false;

    // Очистка буфера COM-порта (игнорируем всё, что накопилось до пуска)
    serial->clear(QSerialPort::AllDirections);
    emit logMessage("Буфер COM-порта очищен, начинаем сбор данных");

    // Отправляем UDP-команду на БЦВМ
    sendUdpToBcvm();

    // Запускаем поток чтения
    running = true;
    worker = std::thread(&Stand::readingThread, this);
    return true;
}

// --------------------------------------------------------------
// Поток чтения из COM-порта (основная логика)
// --------------------------------------------------------------
void Stand::readingThread() {
    int64_t absoluteIndex = 0;          // счётчик тактов от момента пуска
    uint8_t firedBits = 0;              // биты, которые уже были зафиксированы (первое срабатывание)
    bool syncLogged = false;            // чтобы не дублировать сообщение о контакте подъёма

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
        masks.append({absoluteIndex, mask});   // сохраняем все маски для анализа

        // Определяем биты, которые появились впервые
        uint8_t newBits = mask & ~firedBits;
        if (newBits != 0) {
            // Формируем список каналов
            QStringList channels;
            if (newBits & 0x01) channels << "1";
            if (newBits & 0x02) channels << "2";
            if (newBits & 0x04) channels << "3";
            if (newBits & 0x08) channels << "4";
            if (newBits & 0x10) channels << "5";
            if (newBits & 0x20) channels << "6";
            if (newBits & 0x40) channels << "7";
            if (newBits & 0x80) channels << "8";

            QString msg = QString("[%1] срабатывание канала(ов): %2")
                              .arg(absoluteIndex)
                              .arg(channels.join(", "));
            emit logMessage(msg);

            // Если среди новых битов есть 8 канал – дополнительно выводим "Контакт подъёма"
            if ((newBits & SYNC_MASK) && !syncLogged) {
                emit logMessage(QString("[%1] Контакт подъёма, начало движения").arg(absoluteIndex));
                syncLogged = true;
                syncIndex = absoluteIndex;   // запоминаем для анализа
                syncFound = true;            // флаг для анализа
            }

            // Отмечаем эти биты как уже сработавшие
            firedBits |= newBits;
        }

        absoluteIndex++;

        // Автоматическая остановка после завершения полётного задания + запас
        // Здесь используем syncIndex, но он станет известен только после появления 8 канала.
        // Если 8 канал ещё не появился, не останавливаемся.
        if (syncFound && (absoluteIndex - syncIndex) > TOTAL_DURATION_MS + 100) {
            emit logMessage("Достигнут конец полётного задания, остановка сбора");
            running = false;
            break;
        }
    }
}

// --------------------------------------------------------------
// Отправка UDP-команды на БЦВМ
// --------------------------------------------------------------
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

// --------------------------------------------------------------
// Вспомогательные функции для анализа
// --------------------------------------------------------------
int64_t Stand::getRelTime(int64_t absIndex) const {
    if (!syncFound) return INT64_MIN;
    return absIndex - syncIndex;
}

QDateTime Stand::utcFromRelTime(int64_t relTime) const {
    return startUtcDateTime.addMSecs(relTime);
}

// Добавление ожидаемых событий циклограммы
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

// Добавление реальных срабатываний каналов (только первые)
void Stand::addFiredChannelsEntries(QVector<LogEntry>& entries) const {
    uint8_t recordedBits = 0;   // биты, которые уже были записаны
    // Проходим по всем сохранённым маскам в хронологическом порядке
    for (const auto& rec : masks) {
        int64_t relTime = getRelTime(rec.absoluteIndex);
        if (relTime == INT64_MIN) continue;   // синхронизация ещё не произошла

        // Находим новые биты, которые появились в этой маске и ещё не были записаны
        uint8_t newBits = rec.mask & ~recordedBits;
        if (newBits == 0) continue;

        // Игнорируем 8 канал (он уже выведен как "Контакт подъёма")
        newBits &= ~SYNC_MASK;
        if (newBits == 0) continue;

        // Фильтр по времени (отсекаем слишком ранние/поздние)
        if (relTime < -10000 || relTime > TOTAL_DURATION_MS + 1000) continue;

        // Формируем список каналов
        QStringList channels;
        if (newBits & 0x01) channels << "1";
        if (newBits & 0x02) channels << "2";
        if (newBits & 0x04) channels << "3";
        if (newBits & 0x08) channels << "4";
        if (newBits & 0x10) channels << "5";
        if (newBits & 0x20) channels << "6";
        if (newBits & 0x40) channels << "7";

        QDateTime utc = utcFromRelTime(relTime);
        QString text = QString("[%1][%2] <font color='red'>срабатывание канала(ов): %3</font>")
                           .arg(utc.toString("hh:mm:ss.zzz"))
                           .arg(relTime)
                           .arg(channels.join(", "));
        entries.push_back({relTime, text});

        // Запоминаем, что эти биты уже обработаны
        recordedBits |= newBits;
    }
}

// Сортировка записей по относительному времени и отправка в лог
void Stand::sortAndEmit(QVector<LogEntry>& entries) {
    std::sort(entries.begin(), entries.end(),
              [](const LogEntry& a, const LogEntry& b) {
                  return a.relTime < b.relTime;
              });
    for (const auto& e : entries) {
        emit logMessage(e.text);
    }
}

// --------------------------------------------------------------
// Анализ (вывод ожидаемых событий и реальных срабатываний)
// --------------------------------------------------------------
void Stand::performAnalysis() {
    if (!syncFound) {
        emit logMessage("Ошибка: синхроимпульс не найден. Анализ невозможен.");
        return;
    }

    QVector<LogEntry> entries;
    addEventEntries(entries);
    addFiredChannelsEntries(entries);
    sortAndEmit(entries);
    emit analysisComplete(masks, syncIndex);
}
