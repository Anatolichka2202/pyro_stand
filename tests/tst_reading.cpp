#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTextStream>
#include "../stand.h"
#include "mock_serial.h"

// Путь к тестовой циклограмме
static QString writeCyclogram()
{
    auto *f = new QTemporaryFile;
    f->setAutoRemove(false);
    f->open();
    QTextStream out(f);
    out << "SET_UTC_TIME = 10:00:00\n";
    out << "START_UTC_TIME = 10:00:30\n";
    out << "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -8000\n";
    out << "LIFT_OFF_CONTACT = 0\n";
    out << "CLOSE_MAIN_VALVES_ENGINES_9_TO_12 = 20000\n";
    f->close();
    QString path = f->fileName();
    delete f;
    return path;
}

static constexpr int64_t TIME_TO_START = 30000; // мс
static constexpr int64_t IGNITE_TICK   = TIME_TO_START - 8000; // 22000
static constexpr int64_t LIFTOFF_TICK  = TIME_TO_START;        // 30000
static constexpr int64_t VALVE_TICK    = TIME_TO_START + 20000; // 50000
static constexpr int64_t DROP_TICK     = 52000; // после штатного завершения

class TstReading : public QObject
{
    Q_OBJECT

private slots:

    void allEventsOnTime()
    {
        auto *mock = new MockSerial;
        mock->fireAt(IGNITE_TICK,  0x03);
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->fireAt(VALVE_TICK,   0x60);
        mock->dropAfter(DROP_TICK);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(30000)); // до 30 сек ждём

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        // Берём только отслеживаемые
        QVector<EventRow> tracked;
        for (const auto &e : events) if (e.hasChannels) tracked.append(e);

        QCOMPARE(tracked.size(), 3);
        for (const auto &e : tracked)
            QCOMPARE(e.status, QString("ok"));

        QFile::remove(path);
    }

    void missedEvent()
    {
        auto *mock = new MockSerial;
        // IGNITE не подаём совсем
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->fireAt(VALVE_TICK,   0x60);
        mock->dropAfter(DROP_TICK);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(30000));

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        bool igniteFound = false;
        for (const auto &e : events) {
            if (e.key == "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12") {
                QCOMPARE(e.status, QString("fail"));
                igniteFound = true;
            }
        }
        QVERIFY(igniteFound);
        QFile::remove(path);
    }

    void portDropMidFlight()
    {
        auto *mock = new MockSerial;
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->dropAfter(LIFTOFF_TICK + 5000); // обрыв через 5 сек после старта

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy errorSpy(&s, &Stand::portError);
        QSignalSpy phaseSpy(&s, &Stand::phaseChanged);
        s.startReadingForTest(TIME_TO_START);

        // Ждём portError или смену фазы
        QTRY_VERIFY_WITH_TIMEOUT(!errorSpy.isEmpty(), 30000);

        QFile::remove(path);
    }

    void stopCommandDuringFlight()
    {
        auto *mock = new MockSerial;
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->dropAfter(DROP_TICK);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy phaseSpy(&s, &Stand::phaseChanged);
        s.startReadingForTest(TIME_TO_START);

        // Ждём старта (фаза Running)
        QTRY_VERIFY_WITH_TIMEOUT([&]{
            for (const auto &args : phaseSpy)
                if (qvariant_cast<Phase>(args.at(0)) == Phase::Running) return true;
            return false;
        }(), 20000);

        s.stop();

        // После stop() фаза должна быть Stopped, не Completed
        QTRY_VERIFY_WITH_TIMEOUT([&]{
            if (phaseSpy.isEmpty()) return false;
            Phase last = qvariant_cast<Phase>(phaseSpy.last().at(0));
            return last == Phase::Stopped;
        }(), 5000);

        QFile::remove(path);
    }

    void syncDetectedCorrectly()
    {
        auto *mock = new MockSerial;
        mock->fireAt(LIFTOFF_TICK,     0x80);
        mock->fireAt(VALVE_TICK,       0x60);
        mock->dropAfter(DROP_TICK);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(30000));

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        // CLOSE_VALVES: calculatedMs должен быть ≈ 20000 (VALVE_TICK - LIFTOFF_TICK)
        for (const auto &e : events) {
            if (e.key == "CLOSE_MAIN_VALVES_ENGINES_9_TO_12") {
                QCOMPARE(e.status, QString("ok"));
                QVERIFY(qAbs(e.calculatedMs - 20000) < 10); // точность ±10 мс
            }
        }
        QFile::remove(path);
    }

    void lateIgnition()
    {
        auto *mock = new MockSerial;
        // IGNITE с задержкой +3 мс (ещё в пределах FAIL_MARGIN_MS=5)
        mock->fireAt(IGNITE_TICK + 3, 0x03);
        mock->fireAt(LIFTOFF_TICK,    0x80);
        mock->fireAt(VALVE_TICK,      0x60);
        mock->dropAfter(DROP_TICK);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(30000));

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        for (const auto &e : events) {
            if (e.key == "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12") {
                QCOMPARE(e.status, QString("ok"));
                QVERIFY(e.deviationMs <= 3);
            }
        }
        QFile::remove(path);
    }

    // КП срабатывает с опозданием (после планового T0).
    // Ожидаем: firedTick == реальный тик, syncIndex == реальный тик (не плановый).
    void delayedT0_syncIndexIsActualTick()
    {
        static constexpr int64_t LATE_LIFTOFF = LIFTOFF_TICK + 1000; // T0 + 1 с
        static constexpr int64_t LATE_VALVE   = LATE_LIFTOFF + 20000;

        auto *mock = new MockSerial;
        mock->fireAt(LATE_LIFTOFF, 0x80);
        mock->fireAt(LATE_VALVE,   0x60);
        mock->dropAfter(LATE_VALVE + 2000);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(60000));

        // syncIndex должен отражать реальный тик КП, а не плановый
        QCOMPARE(s.getSyncIndex(), LATE_LIFTOFF);

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        for (const auto &e : events) {
            if (e.key == "LIFT_OFF_CONTACT") {
                QVERIFY(e.firedTick != -1);
                QCOMPARE(e.firedTick, static_cast<int>(LATE_LIFTOFF));
            }
        }
        QFile::remove(path);
    }

    // КП опаздывает → статус "late", deviationMs > 0, VALVE всё равно ОК (относительно реального T0).
    void delayedT0_analysisStatusIsLate()
    {
        static constexpr int64_t LATE_LIFTOFF = LIFTOFF_TICK + 1000;
        static constexpr int64_t LATE_VALVE   = LATE_LIFTOFF + 20000;

        auto *mock = new MockSerial;
        mock->fireAt(LATE_LIFTOFF, 0x80);
        mock->fireAt(LATE_VALVE,   0x60);
        mock->dropAfter(LATE_VALVE + 2000);

        const QString path = writeCyclogram();
        Stand s(nullptr, "", std::unique_ptr<ISerialPort>(mock));
        s.loadCyclogram(path);

        QSignalSpy done(&s, &Stand::analysisDone);
        s.startReadingForTest(TIME_TO_START);
        QVERIFY(done.wait(60000));

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        bool kpChecked = false, valveChecked = false;
        for (const auto &e : events) {
            if (e.key == "LIFT_OFF_CONTACT") {
                // КП опоздало — статус late, отклонение ≈ 1000 мс
                QCOMPARE(e.status, QString("late"));
                QVERIFY(e.deviationMs >= 1000);
                kpChecked = true;
            }
            if (e.key == "CLOSE_MAIN_VALVES_ENGINES_9_TO_12") {
                // VALVE сработал: статус ok или late (зависит от окна детектирования),
                // но отклонение от реального T0 должно быть ≈ 0
                QVERIFY(e.firedTick != -1);
                QVERIFY(qAbs(e.calculatedMs - 20000) < 10);
                QCOMPARE(e.deviationMs, 0);
                valveChecked = true;
            }
        }
        QVERIFY(kpChecked);
        QVERIFY(valveChecked);
        QFile::remove(path);
    }
};

QTEST_GUILESS_MAIN(TstReading)
#include "tst_reading.moc"
