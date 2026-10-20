#pragma once

#include "integration.h"
#include <QJsonObject>

// Одна запись относится только к одному прибору и одной восьмиканальной группе.
class FreeRecording {
public:
    struct Config {
        bool legacy = false;
        int group = 0;
        quint8 selected = 255;
        int kp = -1;
        bool fallingKp = false;
        QString source;
    };
    bool start(const Config &config);
    void acceptFrame(const BoardFrame &frame, const QByteArray &raw);
    void acceptRejected(const QByteArray &raw, const QString &error);
    void acceptLegacy(const QByteArray &raw);
    void stop(const QString &reason);
    void checkDeadline();
    bool active() const { return m_active; }
    bool armed() const { return m_armed; }
    QString status() const;
    const BoardFrame &frame() const { return m_frame; }
    const Config &config() const { return m_config; }
    std::optional<quint32> kp() const { return m_kp; }
    std::optional<double> relativeMs(quint32 tick) const;
    QJsonObject report() const;
private:
    bool retain(const QByteArray &raw, const QString &error = {});
    void observe(const BoardFrame &frame, bool legacy, quint32 sampleTick = 0);
    bool relevant(int index) const;
    Config m_config;
    BoardFrame m_frame{};
    LegacyTesterDecoder m_decoder;
    QElapsedTimer m_elapsed;
    bool m_active = false, m_armed = false;
    QString m_status, m_id, m_started, m_stopped;
    std::optional<quint32> m_kp;
    QJsonArray m_raw, m_edges;
    QJsonObject m_baseline;
    qsizetype m_bytes = 0;
    int m_rejectedFrames = 0;
};
