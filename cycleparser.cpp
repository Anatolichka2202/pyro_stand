#include "CycleParser.h"
#include <QSettings>
#include <QDebug>

bool CycleParser::loadFromIni(const QString& filename) {
    QSettings ini(filename, QSettings::IniFormat);
    QStringList groups = ini.childGroups();
    events_.clear();

    for (const QString& group : groups) {
        ini.beginGroup(group);
        CycleEvent e;
        e.time_ms = ini.value("time_ms", 0).toInt();
        e.description = ini.value("description").toString();
        e.block = ini.value("block", -1).toInt();
        e.action = ini.value("action", "none").toString();
        e.needTester = ini.value("tester", false).toBool();
        if (e.block >= 0 && !e.description.isEmpty())
            events_.append(e);
        ini.endGroup();
    }

    std::sort(events_.begin(), events_.end(),
              [](const CycleEvent& a, const CycleEvent& b) {
                  return a.time_ms < b.time_ms;
              });
    qDebug() << "Загружено событий:" << events_.size();
    return !events_.isEmpty();
}
