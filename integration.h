#pragma once

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QLocalSocket>
#include <QObject>
#include <QSerialPort>
#include <QTemporaryFile>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

#include <array>
#include <optional>

struct PlannedAction {
    QString key;
    qint32 offsetMs = 0;
    int ordinal = 0;
};

struct CyclogramPlan {
    QDateTime t0Utc;
    QVector<PlannedAction> actions;
};

struct BoardChannel {
    bool triggered = false;
    bool ready = false;
    quint32 riseUs = 0;
    quint32 fallUs = 0;
};

struct BoardFrame {
    std::array<BoardChannel, 96> channels;
};

struct InputRef {
    int index = -1;
    bool falling = false;
};

struct ActionMapping {
    QVector<InputRef> inputs;
    std::optional<double> toleranceMs;
};

struct StandMapping {
    QString version;
    InputRef kp;
    QHash<QString, ActionMapping> actions;
};

namespace StandProtocol {
bool parseCyclogram(const QByteArray &bytes, CyclogramPlan *plan, QString *error);
bool parseBoardFrame(const QByteArray &bytes, BoardFrame *frame, QString *error);
bool parseMapping(const QByteArray &bytes, StandMapping *mapping, QString *error);
QByteArray resetCommand();
}

class IntegrationSession : public QObject {
    Q_OBJECT
public:
    enum class BoardTransport { Udp, Serial };
    struct Options {
        QString debugSocket = QStringLiteral("kasupp-debug-events-v1");
        BoardTransport boardTransport = BoardTransport::Udp;
        QString serialPort;
        QHostAddress boardIp = QHostAddress(QStringLiteral("192.168.17.144"));
        QHostAddress bindIp = QHostAddress::AnyIPv4;
        quint16 telemetryPort = 5000;
        quint16 commandPort = 101;
    };

    explicit IntegrationSession(Options options, QObject *parent = nullptr);
    ~IntegrationSession() override;
    void start();
    bool loadMapping(const QString &path);
    bool resetBoard();
    bool saveReport(const QString &path, QString *error);

    bool debugConnected() const { return m_debug.state() == QLocalSocket::ConnectedState; }
    bool boardReceiving() const { return m_boardReceiving; }
    bool boardArmed() const { return m_boardArmed; }
    bool hasPlan() const { return m_hasPlan; }
    bool hasBoardFrame() const { return m_hasBoardFrame; }
    const CyclogramPlan &plan() const { return m_plan; }
    const BoardFrame &boardFrame() const { return m_boardFrame; }
    const StandMapping &mapping() const { return m_mapping; }
    QString uploadId() const { return m_uploadId; }
    QString cyclogramSha256() const { return m_cyclogramSha; }
    QString uploadState() const { return m_uploadState; }
    QString lockState() const { return m_lockState; }
    QString bcvmUtc() const { return m_bcvmUtc; }
    bool executionEnabled() const { return m_executionEnabled; }
    bool executionPermissionObserved() const { return m_executionObserved; }
    QString lastError() const { return m_lastError; }
    quint64 acceptedFrames() const { return m_acceptedFrames; }
    quint64 rejectedFrames() const { return m_rejectedFrames; }
    std::optional<quint32> kpTimeUs() const;
    QString actionResult(const PlannedAction &action) const;

signals:
    void changed();
    void message(const QString &text);

private:
    void connectDebug();
    void readDebug();
    void processDebug(const QJsonObject &object);
    void resetDebugState();
    void receiveUdp();
    void receiveSerial();
    void processFrame(const QByteArray &raw);
    void recordFrame(const QByteArray &raw, const QString &error = {});
    void setError(const QString &text);

    Options m_options;
    QLocalSocket m_debug;
    QUdpSocket m_udp;
    QSerialPort m_serial;
    QTimer m_reconnect;
    QTimer m_boardWatchdog;
    QTimer m_resetWatchdog;
    QByteArray m_debugBuffer;
    QByteArray m_serialBuffer;
    QString m_runId;
    QString m_streamId;
    quint64 m_nextSeq = 0;
    QString m_uploadId;
    QString m_uploadState = QStringLiteral("NONE");
    QString m_lockState = QStringLiteral("UNKNOWN");
    QString m_bcvmUtc;
    bool m_executionEnabled = false;
    bool m_executionObserved = false;
    bool m_boardReceiving = false;
    bool m_boardArmed = false;
    bool m_prePermissionActivity = false;
    bool m_awaitReset = false;
    bool m_hasPlan = false;
    bool m_hasBoardFrame = false;
    CyclogramPlan m_plan;
    BoardFrame m_boardFrame;
    StandMapping m_mapping;
    QByteArray m_mappingBytes;
    QByteArray m_cyclogramBytes;
    QString m_cyclogramSha;
    QString m_lastError;
    quint64 m_acceptedFrames = 0;
    quint64 m_rejectedFrames = 0;
    QJsonArray m_debugEvidence;
    QJsonArray m_localEvidence;
    QTemporaryFile m_frameEvidence;
};
