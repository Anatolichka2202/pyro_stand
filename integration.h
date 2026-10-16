#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QLocalSocket>
#include <QObject>
#include <QSerialPort>
#include <QStringList>
#include <QTemporaryFile>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

#include <array>
#include <optional>

struct PlannedAction {
    QString key;
    qint32 offsetMs = 0;
    int ordinal = 0; // presentation only; never selects a hardware input
    int sourceLine = 0;
    QString actionId; // SHA256(raw file):physical line
    QString rawValue;
    QString comment;
    QByteArray rawLine;
};

struct CyclogramPlan {
    QDateTime t0Utc;
    QVector<PlannedAction> actions;
    QByteArray sourceBytes;
    QString sha256;
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
    QString signalId;
};

struct ActionMapping {
    QVector<InputRef> inputs;
    std::optional<double> toleranceMs;
    bool definitionConfirmed = false; // complete expected-signal set from the catalogue
};

struct StandMapping {
    QString version;
    QStringList warnings;
    InputRef kp;
    // Type -> expected signals -> inputs, independent of action occurrences.
    QHash<QString, ActionMapping> actions;
    QHash<QString, InputRef> signalBindings;
    QString kpActionKey = QStringLiteral("LIFT_OFF_CONTACT");
};

// Old tester: one eight-bit state mask per hardware millisecond.
// No PC clock or UART delivery time participates in the measurement.
class LegacyTesterDecoder {
public:
    void reset();
    BoardFrame append(quint8 mask);
    quint64 sampleCount() const { return m_samples; }
private:
    BoardFrame m_frame{};
    quint64 m_samples = 0;
    quint8 m_previous = 0;
};


// Evaluation is based on mapped cyclogram events, not raw tester diagnostics.
struct ChannelMeasurement {
    InputRef input;
    std::optional<double> actualMs;
    std::optional<double> deviationMs;
};

struct ActionMeasurement {
    enum class State {
        Unmapped, UploadUnconfirmed, ExecutionUnconfirmed, PrematureActivity,
        BoardNotArmed, WaitingForKp, RepeatedAction, AmbiguousInterval,
        WaitingForClockAlignment, WaitingForInputs, Partial, Measured,
        PlanReferenceUnknown, InvalidAssignment,
        MissingAfterFinish, PartialAfterFinish
    };
    State state = State::Unmapped;
    QString reason;
    std::optional<double> plannedMs;
    QVector<ChannelMeasurement> channels;
    std::optional<double> toleranceMs;
};

namespace StandProtocol {
bool parseCyclogram(const QByteArray &bytes, CyclogramPlan *plan, QString *error);
bool parseBoardFrame(const QByteArray &bytes, BoardFrame *frame, QString *error);
bool parseMapping(const QByteArray &bytes, StandMapping *mapping, QString *error);
StandMapping benchMapping(int firstStageYps, int secondStageYps, int kpYps,
                          const CyclogramPlan &plan = {});
QByteArray resetCommand();
std::optional<qint32> plannedKpOffset(const CyclogramPlan &plan, const StandMapping &mapping);
QJsonObject mappingSnapshot(const StandMapping &mapping);
}

class IntegrationSession : public QObject {
    Q_OBJECT
public:
    enum class BoardTransport { Udp, Serial, LegacySerial };
    struct Options {
        QString debugSocket = QStringLiteral("kasupp-debug-events-v1");
        QString wiringProfilePath; // optional; empty selects the built-in historical variant
        QString comparisonWiringProfilePath;
        BoardTransport boardTransport = BoardTransport::Udp;
        QString serialPort;
        QString comparisonLegacyPort; // independent legacy observer alongside a new tester
        bool comparisonLegacySecondStage = true;
        bool comparisonLegacyHasKp = false;
        bool legacyHasKp = true;
        QString secondLegacySerialPort; // optional simultaneous БКД-Б; own channel-8 KP
        QHostAddress boardIp = QHostAddress(QStringLiteral("192.168.17.144"));
        QHostAddress bindIp = QHostAddress::AnyIPv4;
        quint16 telemetryPort = 5000;
        quint16 commandPort = 101;
        int firstStageYps = 0;  // 0 = wiring not selected, never guess a YPS number
        int secondStageYps = 0;
        int kpYps = 0;         // physical KP is always channel 8
        bool legacySecondStage = false;
    };

    explicit IntegrationSession(Options options, QObject *parent = nullptr);
    ~IntegrationSession() override;
    void start();
    void configureBoard(const Options &options);
    const Options &options() const { return m_options; }
    bool loadMapping(const QString &path);
    bool resetBoard();
    bool saveReport(const QString &path, QString *error);

    bool debugConnected() const { return m_debug.state() == QLocalSocket::ConnectedState; }
    bool boardReceiving() const {
        return m_options.boardTransport == BoardTransport::LegacySerial
            ? m_legacyReceiving[0] &&
                  (m_options.secondLegacySerialPort.isEmpty() || m_legacyReceiving[1])
            : m_boardReceiving;
    }
    bool legacyStreamReceiving(int stream) const {
        return stream >= 0 && stream < 2 && m_legacyReceiving[size_t(stream)];
    }
    bool legacyStreamKpObserved(int stream) const;
    bool boardArmed() const { return m_boardArmed; }
    bool hasPlan() const { return m_hasPlan; }
    bool hasBoardFrame() const { return m_hasBoardFrame; }
    const CyclogramPlan &plan() const { return m_plan; }
    const BoardFrame &boardFrame() const { return m_boardFrame; }
    const StandMapping &mapping() const { return m_mapping; }
    QString collectionId() const { return m_collectionId; }
    QString uploadId() const { return m_uploadId; }
    QString cyclogramSha256() const { return m_cyclogramSha; }
    QString uploadState() const { return m_uploadState; }
    QString lockState() const { return m_lockState; }
    QString bcvmUtc() const { return m_bcvmUtc; }
    bool executionEnabled() const { return m_executionEnabled; }
    bool executionPermissionObserved() const { return m_executionObserved; }
    // Достоверная оценка отделена от регистрации физических входов.
    const BoardFrame &liveFrame() const { return m_liveFrame; }
    quint64 discardedUdpSenders() const { return m_discardedUdpSenders; }
    QString lastUdpSender() const { return m_lastUdpSender; }
    bool collectionFinished() const { return m_collectionFinished; }
    QString finishReason() const { return m_finishReason; }
    void finishCollection(const QString &reason = QStringLiteral("Сбор завершён оператором"));
    void checkCompletion();
    bool measurementEligible() const {
        return m_fileIdentityValid && m_uploadState == QStringLiteral("CONFIRMED") && m_executionObserved &&
               !m_prePermissionActivity && m_boardArmed;
    }
    QString lastError() const { return m_lastError; }
    quint64 acceptedFrames() const { return m_acceptedFrames; }
    quint64 rejectedFrames() const { return m_rejectedFrames; }
    std::optional<quint32> kpTimeUs() const;
    std::optional<double> measuredPositionMs() const;
    ActionMeasurement measureAction(const PlannedAction &action) const;
    QString actionResult(const PlannedAction &action) const;

signals:
    void changed();
    void message(const QString &text);

private:
    friend class IntegrationTest;
    friend class OperatorGuiTest;
    void connectDebug();
    void readDebug();
    void processDebug(const QJsonObject &object);
    void resetDebugState();
    void receiveUdp();
    void receiveSerial();
    void processFrame(const QByteArray &raw);
    void processLegacyBytes(const QByteArray &raw, int stream = 0);
    void updateBenchMapping();
    bool clearFrameEvidence();
    bool discardPendingBoardData();
    void resetCollectionEvidence(bool resetDebugEvidence);
    void applyFrame(const BoardFrame &frame);
    void recordFrame(const QByteArray &raw, const QString &error = {});
    void setError(const QString &text);

    Options m_options;
    QLocalSocket m_debug;
    QUdpSocket m_udp;
    QSerialPort m_serial;
    QSerialPort m_secondSerial;
    QTimer m_reconnect;
    QTimer m_completionTimer;
    QElapsedTimer m_kpSeenOnPc;
    std::array<quint64, 2> m_samplesAtFinish{};
    bool m_collectionFinished = false;
    QString m_finishReason;
    quint64 m_discardedUdpSenders = 0;
    // Счётчики завершённого сбора не меняются при последующем live-приёме.
    quint64 m_acceptedAtFinish = 0;
    quint64 m_rejectedAtFinish = 0;
    quint64 m_discardedUdpAtFinish = 0;
    QString m_lastUdpSender;
    BoardFrame m_liveFrame{};
    QTimer m_boardWatchdog;
    QTimer m_resetWatchdog;
    std::array<QTimer, 2> m_legacyWatchdog;
    QByteArray m_debugBuffer;
    QByteArray m_serialBuffer;
    std::array<LegacyTesterDecoder, 2> m_legacy;
    std::array<bool, 2> m_legacyArmed{};
    std::array<bool, 2> m_legacyReceiving{};
    QString m_runId;
    QString m_collectionId;
    QElapsedTimer m_collectionElapsed;
    QString m_streamId;
    quint64 m_nextSeq = 0;
    QString m_uploadId;
    QString m_uploadState = QStringLiteral("NONE");
    QString m_lockState = QStringLiteral("UNKNOWN");
    QString m_bcvmUtc;
    bool m_executionEnabled = false;
    bool m_executionObserved = false;
    bool m_fileIdentityValid = true;
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
    bool m_evidenceValid = true;
    QJsonArray m_debugEvidence;
    QJsonArray m_localEvidence;
    QTemporaryFile m_frameEvidence;
};
