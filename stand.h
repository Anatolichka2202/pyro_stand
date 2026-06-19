#ifndef STAND_H
#define STAND_H

#include <QObject>
#include <QSerialPort>
#include <QTimer>
#include <QTime>
#include <QDateTime>
#include <atomic>
#include <thread>
#include <vector>
#include "types.h"
#include <QMutex>

class Stand : public QObject
{
    Q_OBJECT
public:
    explicit Stand(QObject *parent = nullptr);
    ~Stand();

    bool loadCyclogram();
    bool setStartTimeFromUI(const QTime &time);
    bool isPortOpen() const { return m_serial && m_serial->isOpen(); }
    bool pingBcvm() const; // проверка доступности ЯВ

    void sendToBoard();    // "ЗАГРУЗИТЬ НА БОРТ" – отправка UDP + запуск потока
    void stop();           // СТОП – отправка команды 99 + остановка

    void updateNextEvent(int64_t absoluteIndex, int64_t timeToStartMs);

    QVector<EventRow> getEvents() const { return m_events; }
    Phase getPhase() const { return m_phase; }
    QTime getStartTime() const { return m_startTime; }

    void resetForNewTest();

signals:
    void phaseChanged(Phase newPhase);
    void timerUpdated(const QString &text, const QString &color);   // основной таймер
    void nextEventTimer(const QString &text);                       // до следующего события
    void eventFired(int eventId, int absoluteTick);
    void analysisDone(const QVector<EventRow> &events);
    void logMessage(const QString &msg, const QString &type);
    void portError(const QString &msg); // ошибка порта
    void flightComplete();   // сигнал о завершении полёта
    void eventFailed(int eventId);   // для неудачных срабатываний

private:
    void readingThread(int64_t timeToStartMs);
    void sendUdpToBcvm(const QByteArray &datagram);
    void updatePhase(Phase newPhase);
    void completeFlight();
    void performAnalysis(int64_t timeToStartMs);

    bool writeStartTimeToFile(const QTime &time);

    QSerialPort *m_serial;
    std::atomic<bool> m_running;
    std::thread m_worker;

    QVector<EventRow> m_events;
    QVector<ChannelMapping> m_mappings;

    Phase m_phase = Phase::Idle;
    int m_flightDurationMs = 0;

    std::vector<struct MaskRecord> m_masks;
    int64_t m_syncIndex;
    bool m_syncFound;
    bool m_analysisDone;

    //время
    QTime m_setTime;              // текущее лабораторное время (SET_UTC_TIME)
    QTime m_startTime;            // целевое время старта (START_UTC_TIME)
    int64_t m_timeToStartMs = 0;  // вычисленная разница в миллисекундах

    mutable QMutex m_eventsMutex;

    static constexpr const char* SERIAL_PORT = "COM7";
    static constexpr int SERIAL_BAUDRATE = 115200;
    static constexpr const char* CYCLOGRAM_FILE = "cyclogram.ini";
    static constexpr uint8_t SYNC_MASK = 0x80;
    static constexpr quint16 UDP_PORT = 4000;
    static constexpr const char* BCVM_IP = "192.168.17.246";
    static constexpr int FLIGHT_SAFETY_MARGIN_MS = 1000;
    static constexpr int RETRY_COUNT = 3;
};

#endif // STAND_H
