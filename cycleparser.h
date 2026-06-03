#pragma once

#include <QString>
#include <QVector>

struct CycleEvent {
    int time_ms;               // время от "Контакт подъёма" (0), может быть отрицательным
    QString description;
    int block;                 // номер блока (0..11)
    QString action;            // "none", "pyro"
    bool needTester;           // true – отправлять команду тестеру
};

class CycleParser {
public:
    bool loadFromIni(const QString& filename);
    const QVector<CycleEvent>& events() const { return events_; }

private:
    QVector<CycleEvent> events_;
};
