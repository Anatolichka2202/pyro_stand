#pragma once

#include <QString>
#include <cstdint>

// Запись действия для полной шкалы циклограммы.
// Список физических каналов и достоверность измерения хранятся отдельно.
struct EventRow {
    int id = 0;
    QString key;
    QString description;
    int64_t time_ms = 0;
    bool hasChannels = false;
    QString status;
};
