#ifndef STAND_H
#define STAND_H

#include <QObject>
#include <QSerialPort>
#include <QTime>
#include <atomic>
#include <thread>
#include <vector>
#include "types.h"
#include <QMutex>

class Stand : public QObject
{
    Q_OBJECT
public:
    // portName injectable для тестов (пустая строка = порт не открывается)
    explicit Stand(QObject *parent = nullptr, const QString &portName = DEFAULT_PORT);
    ~Stand();

    // filePath: если пустой — ищет cyclogram.ini рядом с .exe
    bool loadCyclogram(const QString &filePath = {});
    bool setStartTimeFromUI(const QTime &time, const QString &filePath = {});

    bool isPortOpen() const { return m_serial && m_serial->isOpen(); }
    bool pingBcvm() const;

    void sendToBoard();
    void stop();
    void resetForNewTest();

    // Чистая функция анализа — без side-эффектов, тестируема без железа
    static QVector<EventRow> analyzeEvents(const QVector<EventRow> &events, int64_t syncIndex);

    QVector<EventRow> getEvents() const { QMutexLocker l(&m_eventsMutex); return m_events; }
    Phase getPhase() const { return m_phase; }
    QTime getStartTime() const { return m_startTime; }

signals:
    void phaseChanged(Phase newPhase);
    void timerUpdated(const QString &text, const QString &color);
    void nextEventTimer(const QString &text);
    void eventFired(int eventId, int absoluteTick);
    void analysisDone(const QVector<EventRow> &events);
    void logMessage(const QString &msg, const QString &type);
    void portError(const QString &msg);
    void flightComplete();
    void eventFailed(int eventId);

private slots:
    void completeFlight();

private:
    void readingThread(int64_t timeToStartMs);
    void updatePhase(Phase newPhase);
    void updateNextEvent(int64_t absoluteIndex, int64_t timeToStartMs);

    QString cyclogramFilePath() const;
    bool writeStartTimeToFile(const QTime &time, const QString &filePath);

    QSerialPort *m_serial = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopped{false};
    std::thread m_worker;

    QVector<EventRow> m_events;
    QVector<ChannelMapping> m_mappings;

    Phase m_phase = Phase::Idle;
    int m_flightDurationMs = 0;

    struct MaskRecord { int64_t absoluteIndex; uint8_t mask; };
    std::vector<MaskRecord> m_masks;

    int64_t m_syncIndex = -1;
    bool m_syncFound = false;
    bool m_analysisDone = false;

    QTime m_setTime;
    QTime m_startTime;
    int64_t m_timeToStartMs = 0;

    mutable QMutex m_eventsMutex;

    static constexpr const char* DEFAULT_PORT   = "COM7";
    static constexpr int   BAUD_RATE            = 115200;
    static constexpr const char* CYCLOGRAM_FILE = "cyclogram.ini";
    static constexpr uint8_t SYNC_MASK          = 0x80;
    static constexpr quint16 UDP_PORT           = 4000;
    static constexpr const char* BCVM_IP        = "192.168.17.246";
    static constexpr int FLIGHT_SAFETY_MS       = 1000;
    static constexpr int SERIAL_RETRIES         = 3;
};

#endif // STAND_H
