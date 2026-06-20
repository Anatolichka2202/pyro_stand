#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTableWidget>
#include <QLabel>
#include <QPushButton>
#include "../mainwindow.h"
#include "../stand.h"
#include "../mocks/mock_serial.h"

// ─── Helpers ──────────────────────────────────────────────────────────────────

static constexpr int64_t T0    = 5000;  // timeToStartMs
static constexpr int64_t IGNITE_TICK = T0 - 2000;
static constexpr int64_t LIFTOFF_TICK = T0;
static constexpr int64_t ENGINES_TICK = T0 + 1000;
static constexpr int64_t VALVES_TICK  = T0 + 3000;
static constexpr int64_t DROP_TICK    = T0 + 5000;

static QString writeCyclogram()
{
    auto *f = new QTemporaryFile;
    f->setAutoRemove(false);
    f->open();
    QTextStream out(f);
    out << "SET_UTC_TIME = 10:00:00\n";
    out << "START_UTC_TIME = 10:00:05\n";
    out << "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -2000 # Зажигание 9-12\n";
    out << "LIFT_OFF_CONTACT = 0 # Контакт подъёма\n";
    out << "IGNITE_PYRO_CANDLES_ENGINES_1_TO_8 = 1000 # Зажигание 1-8\n";
    out << "CLOSE_MAIN_VALVES_ENGINES_9_TO_12 = 3000 # Закрытие клапанов\n";
    f->close();
    const QString path = f->fileName();
    delete f;
    return path;
}

// Creates a MainWindow backed by MockSerial.
// Returns the raw Stand pointer so callers can spy on its signals.
static MainWindow *makeWindow(MockSerial *mock, const QString &cyclogramPath,
                               Stand **outStand = nullptr)
{
    auto stand = std::make_unique<Stand>(nullptr, "",
                                         std::unique_ptr<ISerialPort>(mock));
    if (outStand) *outStand = stand.get();
    return new MainWindow(std::move(stand), cyclogramPath);
}

// ─── Test class ───────────────────────────────────────────────────────────────

class TstGui : public QObject
{
    Q_OBJECT

private slots:

    // ── Scenario 1: all events fire on time ──────────────────────────────────
    void fullFlightSuccess()
    {
        const QString path = writeCyclogram();
        auto cleanup = qScopeGuard([&]{ QFile::remove(path); });

        auto *mock = new MockSerial;
        mock->fireAt(IGNITE_TICK,  0x03);
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->fireAt(ENGINES_TICK, 0x1C);
        mock->fireAt(VALVES_TICK,  0x60);
        mock->dropAfter(DROP_TICK);

        Stand *standPtr = nullptr;
        std::unique_ptr<MainWindow> w(makeWindow(mock, path, &standPtr));
        w->show();

        // Table populated after loadCyclogram
        auto *table = w->findChild<QTableWidget*>();
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 4); // 4 hasChannels events

        // Spy on analysisDone before clicking
        QSignalSpy done(standPtr, &Stand::analysisDone);

        // Click "ЗАГРУЗИТЬ НА БОРТ"
        auto *loadBtn = w->findChild<QPushButton*>("loadBtn");
        QVERIFY(loadBtn);
        QVERIFY(loadBtn->isEnabled());
        QTest::mouseClick(loadBtn, Qt::LeftButton);

        // Wait for flight to complete (up to 10s — mock runs at CPU speed)
        QVERIFY(done.wait(10000));

        // All 4 tracked events should be "ok"
        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        QVector<EventRow> tracked;
        for (const auto &e : events) if (e.hasChannels) tracked.append(e);

        QCOMPARE(tracked.size(), 4);
        for (const auto &e : tracked)
            QCOMPARE(e.status, QString("ok"));

        // Summary strip visible and contains "4 ✓"
        auto *summaryLabel = w->findChild<QLabel*>("summaryLabel");
        QVERIFY(summaryLabel);
        QVERIFY(summaryLabel->text().contains("4"));

        // Timer label should not still show "--:--"
        auto *timerLabel = w->findChild<QLabel*>("timerLabel");
        QVERIFY(timerLabel);
        QVERIFY(timerLabel->text() != "--:--");
    }

    // ── Scenario 2: one event missed ─────────────────────────────────────────
    void missedEventShownInTable()
    {
        const QString path = writeCyclogram();
        auto cleanup = qScopeGuard([&]{ QFile::remove(path); });

        auto *mock = new MockSerial;
        // No IGNITE_9_TO_12
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->fireAt(ENGINES_TICK, 0x1C);
        mock->fireAt(VALVES_TICK,  0x60);
        mock->dropAfter(DROP_TICK);

        Stand *standPtr = nullptr;
        std::unique_ptr<MainWindow> w(makeWindow(mock, path, &standPtr));
        w->show();

        QSignalSpy done(standPtr, &Stand::analysisDone);
        auto *loadBtn = w->findChild<QPushButton*>("loadBtn");
        QTest::mouseClick(loadBtn, Qt::LeftButton);
        QVERIFY(done.wait(10000));

        const auto events = qvariant_cast<QVector<EventRow>>(done.last().at(0));
        bool igniteFound = false;
        for (const auto &e : events) {
            if (e.key == "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12") {
                QCOMPARE(e.status, QString("fail"));
                igniteFound = true;
            }
        }
        QVERIFY(igniteFound);

        // Table row 0 should show СТОП/fail status
        auto *table = w->findChild<QTableWidget*>();
        QVERIFY(table);
        // Status column = 6; row 0 = IGNITE_9_TO_12
        QVERIFY(table->item(0, 6) != nullptr);
        QVERIFY(table->item(0, 6)->text().contains("НЕ СРАБОТАЛО") ||
                table->item(0, 6)->text().contains("✗"));
    }

    // ── Scenario 3: operator presses СТОП mid-flight ──────────────────────────
    void stopButtonHaltsAndMarksFail()
    {
        const QString path = writeCyclogram();
        auto cleanup = qScopeGuard([&]{ QFile::remove(path); });

        auto *mock = new MockSerial;
        mock->fireAt(LIFTOFF_TICK, 0x80);
        mock->dropAfter(DROP_TICK);

        Stand *standPtr = nullptr;
        std::unique_ptr<MainWindow> w(makeWindow(mock, path, &standPtr));
        w->show();

        QSignalSpy phaseSpy(standPtr, &Stand::phaseChanged);
        auto *loadBtn = w->findChild<QPushButton*>("loadBtn");
        QTest::mouseClick(loadBtn, Qt::LeftButton);

        // Wait until Running phase
        QTRY_VERIFY_WITH_TIMEOUT([&]{
            for (const auto &args : phaseSpy)
                if (qvariant_cast<Phase>(args.at(0)) == Phase::Running) return true;
            return false;
        }(), 10000);

        // Click СТОП
        auto *stopBtn = w->findChild<QPushButton*>("stopBtn");
        QVERIFY(stopBtn);
        QVERIFY(stopBtn->isEnabled());
        QTest::mouseClick(stopBtn, Qt::LeftButton);

        // Phase must reach Stopped
        QTRY_VERIFY_WITH_TIMEOUT([&]{
            if (phaseSpy.isEmpty()) return false;
            return qvariant_cast<Phase>(phaseSpy.last().at(0)) == Phase::Stopped;
        }(), 5000);

        // Timer label shows "СТОП"
        auto *timerLabel = w->findChild<QLabel*>("timerLabel");
        QVERIFY(timerLabel);
        QCOMPARE(timerLabel->text(), QString("СТОП"));

        // СТОП button disabled after stop
        QVERIFY(!stopBtn->isEnabled());
    }
};

QTEST_MAIN(TstGui)
#include "tst_gui.moc"
