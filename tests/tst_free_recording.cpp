#include "free_recording.h"
#include <QTest>

class FreeRecordingTest : public QObject {
    Q_OBJECT
private slots:
    void retrospectiveKpAndWrap() {
        FreeRecording r; FreeRecording::Config c; c.group = 7; c.kp = 63;
        QVERIFY(r.start(c));
        BoardFrame f; f.channels[56] = {true, false, 123, 150};
        r.acceptFrame(f, "stale"); QVERIFY(!r.armed());
        r.acceptFrame({}, "baseline"); QVERIFY(r.armed());
        f = {}; f.channels[56] = {true, false, 0xfffff000u, 500};
        r.acceptFrame(f, "pre-kp"); QVERIFY(!r.kp());
        f.channels[63] = {true, true, 1000, 0};
        r.acceptFrame(f, "kp"); QCOMPARE(*r.relativeMs(0xfffff000u), -5.096);
        QCOMPARE(*r.relativeMs(1000), 0.0); QCOMPARE(*r.relativeMs(2500), 1.5);
        QVERIFY(!r.relativeMs(1000u + 0x80000000u));
        QCOMPARE(r.report().value("edges").toArray().size(), 3);
        r.stop("manual"); const auto report = r.report();
        r.acceptFrame({}, "late"); QCOMPARE(r.report(), report);
    }
    void noKpAndEpochChange() {
        FreeRecording r; QVERIFY(r.start({})); r.acceptFrame({}, "baseline");
        BoardFrame f; f.channels[0] = {true, false, 150, 300};
        r.acceptFrame(f, "edge"); QVERIFY(!r.relativeMs(150));
        r.acceptFrame({}, "reset"); QVERIFY(!r.active());
        QCOMPARE(r.frame().channels[0].riseUs, 150u);
        QCOMPARE(r.report().value("raw_records").toArray().size(), 3);
    }
    void legacyRepeatedPulses() {
        FreeRecording r; FreeRecording::Config c; c.legacy = true; c.kp = 7;
        QVERIFY(r.start(c));
        r.acceptLegacy(QByteArray::fromHex("ff")); QVERIFY(!r.armed());
        r.acceptLegacy(QByteArray::fromHex("000100018100"));
        QVERIFY(r.armed()); QCOMPARE(*r.kp(), 5000u);
        QCOMPARE(*r.relativeMs(r.frame().channels[0].riseUs), -3.0);
        QCOMPARE(r.frame().channels[0].fallUs - r.frame().channels[0].riseUs, 1000u);
        QCOMPARE(r.frame().channels[0].observedRises, 2ull);
        QCOMPARE(r.report().value("edges").toArray().size(), 6);
    }
    void selectedInputsAndInvalidConfig() {
        FreeRecording r; FreeRecording::Config c; c.selected = 0; QVERIFY(!r.start(c));
        c.selected = 1; c.kp = 96; QVERIFY(!r.start(c));
        c.kp = -1; QVERIFY(r.start(c));
        BoardFrame f; f.channels[5] = {true, false, 15, 30};
        r.acceptFrame(f, "irrelevant"); QVERIFY(r.armed());
        QCOMPARE(r.report().value("edges").toArray().size(), 0);
        QCOMPARE(r.frame().channels[5].riseUs, 0u);
    }
    void legacyIgnoresUnselectedActiveInput() {
        FreeRecording r; FreeRecording::Config c; c.legacy = true; c.selected = 1;
        QVERIFY(r.start(c)); r.acceptLegacy(QByteArray::fromHex("808180"));
        QVERIFY(r.armed()); QCOMPARE(r.frame().channels[0].riseUs, 2000u);
        QCOMPARE(r.frame().channels[7].riseUs, 0u);
    }
    void boundedArchive() {
        FreeRecording r; QVERIFY(r.start({}));
        r.acceptFrame({}, QByteArray(16 * 1024 * 1024, 'x'));
        QVERIFY(r.active()); r.acceptFrame({}, "limit"); QVERIFY(!r.active());
        QCOMPARE(r.report().value("raw_records").toArray().size(), 1);
    }
    void contradictoryNewFlags() {
        FreeRecording r; QVERIFY(r.start({})); r.acceptFrame({}, "baseline");
        BoardFrame f; f.channels[0] = {false, false, 1000, 1500};
        r.acceptFrame(f, "contradiction"); QVERIFY(!r.active());
        QCOMPARE(r.report().value("edges").toArray().size(), 0);
    }
};
QTEST_GUILESS_MAIN(FreeRecordingTest)
#include "tst_free_recording.moc"
