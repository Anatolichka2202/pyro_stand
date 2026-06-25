#ifndef STAND_H
#define STAND_H

#include <QObject>
#include <QTime>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include "types.h"
#include "serial_port.h"
#include "platform.h"
#include <QMutex>

// Forward declaration — полный заголовок не нужен в stand.h,
// session_logger.h включается только в stand.cpp и mainwindow.h
class SessionLogger;

class Stand : public QObject
{
    Q_OBJECT
public:
    // portName injectable для тестов (пустая строка = порт не открывается)
    // port: инжектируемый ISerialPort (nullptr = использовать RealSerialPort)
    // logger: опциональный SessionLogger (nullptr = логгер не используется)
    explicit Stand(QObject *parent = nullptr,
                   const QString &portName = DEFAULT_PORT,
                   std::unique_ptr<ISerialPort> port = nullptr,
                   SessionLogger *logger = nullptr);
    ~Stand();

    // filePath: если пустой — ищет cyclogram.ini рядом с .exe
    bool loadCyclogram(const QString &filePath = {});
    bool setStartTimeFromUI(const QTime &time, const QString &filePath = {});

    bool isPortOpen() const { return m_portAvailable; }
    bool pingBcvm() const;

    void sendToBoard();
    void setTransferMode(TransferMode mode) { m_transferMode = mode; }
    TransferMode transferMode() const { return m_transferMode; }
    void stop();
    void resetForNewTest();

    // Чистая функция анализа — без side-эффектов, тестируема без железа
    // timeToStartMs: плановый тик T0 (для расчёта опоздания LIFT_OFF_CONTACT).
    // По умолчанию -1 — обратная совместимость с тестами, не проверяющими канал 8.
    static QVector<EventRow> analyzeEvents(const QVector<EventRow> &events,
                                           int64_t syncIndex,
                                           int64_t timeToStartMs = -1);

    QVector<EventRow> getEvents() const { QMutexLocker l(&m_eventsMutex); return m_events; }
    Phase getPhase() const { return m_phase; }
    QTime getStartTime() const { return m_startTime; }
    QTime getSetTime() const { return m_setTime; }
    int64_t getSyncIndex() const { return m_syncIndex; }

    // Только для тестов: запустить reading loop напрямую без UDP/ping
    void startReadingForTest(int64_t timeToStartMs = 0);

signals:
    void phaseChanged(Phase newPhase);
    void timerTick(TimerState state);
    void nextEventChanged(NextEventInfo info);
    void eventFired(int eventId, int absoluteTick);
    void analysisDone(const QVector<EventRow> &events);
    void logMessage(const QString &msg, const QString &type);
    void portError(const QString &msg);
    void flightComplete();
    void transferProgress(const QString &stage); // "checking"|"sending"|"done"|"error"

private slots:
    void completeFlight();

private:
    void readingThread(int64_t timeToStartMs);
    void updatePhase(Phase newPhase);
    void updateNextEvent(int64_t absoluteIndex, int64_t timeToStartMs);

    QString cyclogramFilePath() const;
    bool writeStartTimeToFile(const QTime &time, const QString &filePath);

    std::unique_ptr<ISerialPort> m_port;
    QString m_portName;
    bool m_portAvailable = false;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopped{false};
    std::thread m_worker;

    QVector<EventRow> m_events;
    QVector<ChannelMapping> m_mappings;

    Phase m_phase = Phase::Idle;
    int m_flightDurationMs = 0;

    struct MaskRecord { int64_t absoluteIndex; uint8_t mask; };
    std::vector<MaskRecord> m_masks;

    int64_t m_syncIndex   = -1;
    int64_t m_finalIndex  = 0;   // последний absoluteIndex из readingThread
    bool m_syncFound    = false;
    bool m_analysisDone = false;

    QTime m_setTime;
    QTime m_startTime;
    int64_t m_timeToStartMs = 0;

    mutable QMutex m_eventsMutex;

    SessionLogger *m_logger = nullptr; // не владеет — владеет MainWindow

    TransferMode m_transferMode = TransferMode::TFTP;

    bool sendCyclogramTftp(const QByteArray &data);

    static constexpr const char* DEFAULT_PORT   = DEFAULT_SERIAL_PORT;
    static constexpr int   BAUD_RATE            = 115200;
    static constexpr const char* CYCLOGRAM_FILE = "cyclogram.ini";
    static constexpr uint8_t SYNC_MASK          = 0x80;
    static constexpr quint16 UDP_PORT           = 4000;
    static constexpr quint16 TFTP_PORT          = 69;
    static constexpr const char* BCVM_IP        = "192.168.17.246";
    static constexpr const char* TFTP_PATH      = "/cyclogram.ini";
    static constexpr int FLIGHT_SAFETY_MS       = 5000;
    static constexpr int FAIL_MARGIN_MS         = 5;
    static constexpr int SERIAL_RETRIES         = 3;
};

#endif // STAND_H
