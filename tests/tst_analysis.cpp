#include <QtTest/QtTest>
#include "../stand.h"

class TstAnalysis : public QObject
{
    Q_OBJECT

private:
    // Вспомогательные фабрики EventRow
    static EventRow makeEvent(int id, int time_ms, const QString &key,
                               const QString &status, int firedTick = -1)
    {
        EventRow ev;
        ev.id          = id;
        ev.key         = key;
        ev.description = key;
        ev.time_ms     = time_ms;
        ev.hasChannels = true;
        ev.status      = status;
        ev.firedTick   = firedTick;
        ev.calculatedMs= -1;
        ev.deviationMs = 0;
        ev.channels    = "1";
        return ev;
    }

private slots:
    // ── analyzeEvents ─────────────────────────────────────────────────────

    void exactFire()
    {
        // Событие ожидается в T+0, syncIndex=1000, firedTick=1000 → откл.=0
        QVector<EventRow> events = {makeEvent(1, 0, "EVT", "ok", 1000)};
        const auto result = Stand::analyzeEvents(events, 1000);
        QCOMPARE(result[0].calculatedMs, 0);
        QCOMPARE(result[0].deviationMs,  0);
        QCOMPARE(result[0].status, QString("ok"));
    }

    void lateFirePositive()
    {
        // Событие ожидается T+100, syncIndex=0, firedTick=110 → откл.=10
        QVector<EventRow> events = {makeEvent(1, 100, "EVT", "ok", 110)};
        const auto result = Stand::analyzeEvents(events, 0);
        QCOMPARE(result[0].calculatedMs, 110);
        QCOMPARE(result[0].deviationMs,  10);
    }

    void earlyFireNegativeTime()
    {
        // Предстартовое событие: time_ms=-8000, syncIndex=30000, firedTick=22005
        // calculatedMs = 22005 - 30000 = -7995, deviation = |-7995 - (-8000)| = 5
        QVector<EventRow> events = {makeEvent(1, -8000, "PYRO", "ok", 22005)};
        const auto result = Stand::analyzeEvents(events, 30000);
        QCOMPARE(result[0].calculatedMs, -7995);
        QCOMPARE(result[0].deviationMs,   5);
    }

    void failEventNotCalculated()
    {
        QVector<EventRow> events = {makeEvent(1, 1000, "EVT", "fail")};
        const auto result = Stand::analyzeEvents(events, 0);
        QCOMPARE(result[0].calculatedMs, -1);
        QCOMPARE(result[0].deviationMs,  -1);
    }

    void pendingEventNotCalculated()
    {
        QVector<EventRow> events = {makeEvent(1, 1000, "EVT", "pending")};
        const auto result = Stand::analyzeEvents(events, 0);
        QCOMPARE(result[0].calculatedMs, -1);
        QCOMPARE(result[0].deviationMs,  -1);
    }

    void mixedEvents()
    {
        QVector<EventRow> events = {
            makeEvent(1, 0,    "A", "ok",   1000),  // точно в срок
            makeEvent(2, 1000, "B", "ok",   2010),  // откл. 10 мс
            makeEvent(3, 2000, "C", "fail"),         // не сработало
        };
        const auto result = Stand::analyzeEvents(events, 1000);
        QCOMPARE(result[0].deviationMs, 0);
        QCOMPARE(result[1].deviationMs, 10);
        QCOMPARE(result[2].calculatedMs, -1);
    }

    void emptyInput()
    {
        const auto result = Stand::analyzeEvents({}, 0);
        QVERIFY(result.isEmpty());
    }

    // ── LIFT_OFF_CONTACT (канал 8) ────────────────────────────────────────────

    void liftOffContactOnTime()
    {
        // Старт точно по плану: syncIndex == timeToStartMs → опоздание 0
        QVector<EventRow> events = {makeEvent(1, 0, "LIFT_OFF_CONTACT", "ok", 30000)};
        const auto result = Stand::analyzeEvents(events, 30000, 30000);
        QCOMPARE(result[0].calculatedMs, 0);
        QCOMPARE(result[0].deviationMs,  0);
        QCOMPARE(result[0].status, QString("ok"));
    }

    void liftOffContactLate()
    {
        // Старт опоздал на 1000 мс: syncIndex=31000, timeToStartMs=30000
        QVector<EventRow> events = {makeEvent(1, 0, "LIFT_OFF_CONTACT", "late", 31000)};
        const auto result = Stand::analyzeEvents(events, 31000, 30000);
        QCOMPARE(result[0].calculatedMs, 0);      // T0 относительно самого себя = 0
        QCOMPARE(result[0].deviationMs,  1000);   // опоздание старта
        QCOMPARE(result[0].status, QString("late"));
    }

    void liftOffContactNotFired()
    {
        // Канал 8 не сработал вообще
        QVector<EventRow> events = {makeEvent(1, 0, "LIFT_OFF_CONTACT", "fail")};
        const auto result = Stand::analyzeEvents(events, 30000, 30000);
        QCOMPARE(result[0].calculatedMs, -1);
        QCOMPARE(result[0].deviationMs,  -1);
        QCOMPARE(result[0].status, QString("fail"));
    }

    void liftOffContactLateFromFail()
    {
        // readingThread-сценарий: статус "fail" выставлен по плановому окну,
        // затем firedTick обновлён когда канал 8 наконец сработал.
        // analyzeEvents должен конвертировать "fail" → "late".
        QVector<EventRow> events = {makeEvent(1, 0, "LIFT_OFF_CONTACT", "fail", 39962)};
        const auto result = Stand::analyzeEvents(events, 39962, 30000);
        QCOMPARE(result[0].status, QString("late"));
        QCOMPARE(result[0].calculatedMs, 0);
        QCOMPARE(result[0].deviationMs,  9962);
    }

    void syncIndexZero()
    {
        // syncIndex=0, firedTick=500, time_ms=500 → dev=0
        QVector<EventRow> events = {makeEvent(1, 500, "E", "ok", 500)};
        const auto result = Stand::analyzeEvents(events, 0);
        QCOMPARE(result[0].calculatedMs, 500);
        QCOMPARE(result[0].deviationMs,  0);
    }
};

QTEST_GUILESS_MAIN(TstAnalysis)
#include "tst_analysis.moc"
