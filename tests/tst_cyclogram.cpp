#include <QtTest/QtTest>
#include <QTemporaryFile>
#include <QTextStream>
#include <QSignalSpy>
#include "../stand.h"

// Stand с пустым портом — железо не нужно
static Stand* makeStand()
{
    return new Stand(nullptr, "");
}

static QString writeTempCyclogram(const QStringList &lines)
{
    auto *f = new QTemporaryFile;
    f->setAutoRemove(false);   // caller removes
    f->open();
    QTextStream out(f);
    for (const QString &l : lines) out << l << "\n";
    f->close();
    const QString path = f->fileName();
    delete f;
    return path;
}

class TstCyclogram : public QObject
{
    Q_OBJECT

private slots:

    void parseValidFile()
    {
        QStringList ini = {
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -8000 # Зажигание пиросвечей",
            "LIFT_OFF_CONTACT = 0 # Контакт подъёма",
            "CLOSE_MAIN_VALVES_ENGINES_9_TO_12 = 20000 # Закрытие клапанов",
        };
        const QString path = writeTempCyclogram(ini);
        Stand *s = makeStand();

        QSignalSpy spy(s, &Stand::analysisDone);
        const bool ok = s->loadCyclogram(path);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);

        const auto events = qvariant_cast<QVector<EventRow>>(spy.at(0).at(0));
        QCOMPARE(events.size(), 3);

        // Первое событие: IGNITE, time=-8000, channels 1,2
        QCOMPARE(events[0].key,         QString("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"));
        QCOMPARE(events[0].time_ms,     -8000);
        QCOMPARE(events[0].hasChannels, true);
        QCOMPARE(events[0].channels,    QString("1, 2"));
        QCOMPARE(events[0].description, QString("Зажигание пиросвечей"));
        QCOMPARE(events[0].status,      QString("pending"));

        // LIFT_OFF_CONTACT: channel 8
        QCOMPARE(events[1].hasChannels, true);
        QCOMPARE(events[1].channels,    QString("8"));

        // CLOSE_MAIN_VALVES: channels 6,7
        QCOMPARE(events[2].hasChannels, true);

        delete s;
        QFile::remove(path);
    }

    void missingSetUtcTime()
    {
        const QString path = writeTempCyclogram({
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        QVERIFY(!s->loadCyclogram(path));
        delete s;
        QFile::remove(path);
    }

    void missingStartUtcTime()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        QVERIFY(!s->loadCyclogram(path));
        delete s;
        QFile::remove(path);
    }

    void invalidTimeFormat()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = not_a_time",
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        QVERIFY(!s->loadCyclogram(path));
        delete s;
        QFile::remove(path);
    }

    void emptyEventsSection()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
        });
        Stand *s = makeStand();
        QVERIFY(!s->loadCyclogram(path));
        delete s;
        QFile::remove(path);
    }

    void commentBecomesDescription()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0 # Это описание",
        });
        Stand *s = makeStand();
        QSignalSpy spy(s, &Stand::analysisDone);
        s->loadCyclogram(path);
        const auto events = qvariant_cast<QVector<EventRow>>(spy.at(0).at(0));
        QCOMPARE(events[0].description, QString("Это описание"));
        delete s;
        QFile::remove(path);
    }

    void eventWithoutCommentUsesKey()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        QSignalSpy spy(s, &Stand::analysisDone);
        s->loadCyclogram(path);
        const auto events = qvariant_cast<QVector<EventRow>>(spy.at(0).at(0));
        QCOMPARE(events[0].description, QString("LIFT_OFF_CONTACT"));
        delete s;
        QFile::remove(path);
    }

    void unknownKeyHasNoChannels()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "UNKNOWN_EVENT = 5000 # Неизвестное событие",
        });
        Stand *s = makeStand();
        QSignalSpy spy(s, &Stand::analysisDone);
        s->loadCyclogram(path);
        const auto events = qvariant_cast<QVector<EventRow>>(spy.at(0).at(0));
        QCOMPARE(events[0].hasChannels, false);
        QCOMPARE(events[0].channels,    QString("—"));
        delete s;
        QFile::remove(path);
    }

    void negativeTimeParsed()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -8000",
        });
        Stand *s = makeStand();
        QSignalSpy spy(s, &Stand::analysisDone);
        s->loadCyclogram(path);
        const auto events = qvariant_cast<QVector<EventRow>>(spy.at(0).at(0));
        QCOMPARE(events[0].time_ms, -8000);
        delete s;
        QFile::remove(path);
    }

    void flightPhaseBecomesLoaded()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        QSignalSpy spy(s, &Stand::phaseChanged);
        s->loadCyclogram(path);
        QVERIFY(!spy.isEmpty());
        const Phase p = qvariant_cast<Phase>(spy.last().at(0));
        QCOMPARE(p, Phase::Loaded);
        delete s;
        QFile::remove(path);
    }

    void writeAndReadBackStartTime()
    {
        const QString path = writeTempCyclogram({
            "SET_UTC_TIME = 10:00:00",
            "START_UTC_TIME = 10:00:30",
            "LIFT_OFF_CONTACT = 0",
        });
        Stand *s = makeStand();
        s->loadCyclogram(path);

        const QTime newTime(11, 30, 0);
        QVERIFY(s->setStartTimeFromUI(newTime, path));
        QCOMPARE(s->getStartTime(), newTime);

        // Перезагружаем и убеждаемся что файл обновился
        Stand *s2 = makeStand();
        s2->loadCyclogram(path);
        QCOMPARE(s2->getStartTime(), newTime);

        delete s;
        delete s2;
        QFile::remove(path);
    }
};

QTEST_GUILESS_MAIN(TstCyclogram)
#include "tst_cyclogram.moc"
