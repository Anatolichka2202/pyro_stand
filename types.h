#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QMap>
#include <QMetaType>
#include <cstdint>

enum class TimeMode { UTC, MSK };

enum class TransferMode { UDP, TFTP };

enum class Phase {
    Idle,
    Loaded,
    Countdown,
    Running,
    Completed,
    Stopped
};
Q_DECLARE_METATYPE(Phase)

struct EventRow {
    int     id          = 0;
    QString key;
    QString description;
    int     time_ms     = 0;
    QString channels;
    bool    hasChannels = false;
    int     firedTick   = -1;
    int     calculatedMs= -1;
    QString status;         // "pending" | "ok" | "fail" | "late"
    int     deviationMs    = 0;
    int     channelSpreadMs= 0;             // мс между первым и последним каналом группы
    QMap<int,int> channelTicks;             // канал(1-8) → абсолютный тик (readingThread)
    QMap<int,int> channelCalcMs;            // канал(1-8) → мс от T0 (analyzeEvents)
};
Q_DECLARE_METATYPE(QVector<EventRow>)

struct ChannelMapping {
    QString key;
    uint8_t mask    = 0;
    QString channelsStr;
};

// Structured signal payloads (T21)
struct TimerState {
    int64_t msToStart = 0; // <0 = countdown (ms remaining); >=0 = elapsed ms since T0
    Phase   phase     = Phase::Idle;
};
Q_DECLARE_METATYPE(TimerState)

struct NextEventInfo {
    int     eventId     = -1;          // -1 = no pending event
    int64_t msRemaining = INT64_MAX;   // ms until next event
    QString description;               // event description or key for display
};
Q_DECLARE_METATYPE(NextEventInfo)

#endif // TYPES_H
