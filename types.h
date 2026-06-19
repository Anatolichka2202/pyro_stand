#ifndef TYPES_H
#define TYPES_H

#include <QString>
#include <QMetaType>

enum class TimeMode { UTC, MSK };

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
    QString status;         // "pending" | "ok" | "fail"
    int     deviationMs = 0;
};
Q_DECLARE_METATYPE(QVector<EventRow>)

struct ChannelMapping {
    QString key;
    uint8_t mask    = 0;
    QString channelsStr;
};

#endif // TYPES_H
