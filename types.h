#ifndef TYPES_H
#define TYPES_H

#include <QString>

// Режим времени (для отладки)
enum class TimeMode {
    UTC,
    MSK
};

// Состояния системы
enum class Phase {
    Idle,
    Loaded,
    Countdown,
    Running,
    Completed,
    Stopped
};

// Структура события циклограммы (используется и в логике, и в UI)
struct EventRow {
    int id;
    QString key;           // ключ из файла
    QString description;   // описание (комментарий)
    int time_ms;           // относительное время в мс (может быть отрицательным)
    QString channels;      // строка каналов, например "1, 2"
    bool hasChannels;      // true если событие отслеживается
    int firedTick;         // -1 если не сработало
    int calculatedMs;      // -1 если не вычислено
    QString status;        // "pending", "ok", "fail"
    int deviationMs;       // отклонение вычисленного от ожидаемого (для анализа)
};

// Маппинг ключ -> битовая маска каналов
struct ChannelMapping {
    QString key;
    uint8_t mask;
    QString channelsStr;
};

#endif // TYPES_H
