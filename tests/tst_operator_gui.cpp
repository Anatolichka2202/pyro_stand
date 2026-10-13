#include "integration_window.h"
#include "timeline_widget.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTextEdit>
#include <QLocalServer>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QUuid>

class OperatorGuiTest : public QObject {
    Q_OBJECT
private slots:
    void showsMappedEventsNotRawTesterChannels();
    void showsDualLegacyComStatuses();
    void fullPlanKeepsTableUsableAndHardwareClockStable();
    void kpClusterDoesNotHideFailure();
};

void OperatorGuiTest::showsMappedEventsNotRawTesterChannels() {
    const QString socketName = QStringLiteral("operator-ui-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QLocalServer server;
    QVERIFY(server.listen(socketName));

    IntegrationSession::Options options;
    options.debugSocket = socketName;
    options.boardIp = QHostAddress::LocalHost;
    options.bindIp = QHostAddress::LocalHost;
    options.telemetryPort = 0;
    options.firstStageYps = 1;
    options.kpYps = 1;
    IntegrationWindow window(options);
    window.show();

    auto *table = window.findChild<QTableWidget *>(QStringLiteral("eventTable"));
    QVERIFY(table);
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 1);
    QCOMPARE(window.findChildren<QTextEdit *>().size(), 1);
    QCOMPARE(table->columnCount(), 7);
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("КАНАЛЫ"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("СОБЫТИЕ"));
    QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("ФАКТ МС"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("ОТКЛ МС"));
    QVERIFY(window.findChild<QWidget *>(QStringLiteral("eventTimeline")));
    QVERIFY(window.findChild<QWidget *>(QStringLiteral("operatorSurface")));
    QVERIFY(window.findChild<QWidget *>(QStringLiteral("journalCaption")));
    QVERIFY(window.findChild<QWidget *>(QStringLiteral("eventJournal")));
    QVERIFY(window.findChild<QWidget *>(QStringLiteral("timerLabel")));
    QVERIFY(window.findChild<QPushButton *>(QStringLiteral("testerConfigurationBtn")));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("legacyStreamsStatus")));

    for (auto *button : window.findChildren<QPushButton *>())
        QVERIFY(!button->text().contains(QStringLiteral("ЗАГРУЗИТЬ НА БОРТ")));

    QTRY_VERIFY(server.hasPendingConnections());
    QLocalSocket *client = server.nextPendingConnection();
    QVERIFY(client);

    const QByteArray plan =
        "START_UTC_DATE = 2026-10-12\n"
        "START_UTC_TIME = 12:00:00\n"
        "OTHER_1 = 0\nOTHER_2 = 1\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = 20\n"
        "CLOSE_MAIN_VALVES_ENGINES_1_TO_8 = 40000\n"; // intentionally NOT mapped
    QJsonObject available;
    available.insert(QStringLiteral("contract"), QStringLiteral("kasu.pp.debug.stand.v1"));
    available.insert(QStringLiteral("type"), QStringLiteral("CYCLOGRAM_AVAILABLE"));
    available.insert(QStringLiteral("seq"), QStringLiteral("1"));
    available.insert(QStringLiteral("stream_id"), QStringLiteral("ui-test-stream"));
    available.insert(QStringLiteral("run_id"), QStringLiteral("ui-test-run"));
    available.insert(QStringLiteral("upload_id"), QStringLiteral("ui-test-upload"));
    available.insert(QStringLiteral("byte_count"), plan.size());
    available.insert(QStringLiteral("sha256_hex"),
        QString::fromLatin1(QCryptographicHash::hash(plan, QCryptographicHash::Sha256).toHex()));
    available.insert(QStringLiteral("content_base64"), QString::fromLatin1(plan.toBase64()));
    client->write(QJsonDocument(available).toJson(QJsonDocument::Compact) + '\n');
    client->flush();

    // One mapped action, its two input subrows, and the physical КП reference; never 96 channels
    // and never an unassigned cyclogram command in the execution table.
    QTRY_COMPARE(table->rowCount(), 4);
    QStringList names;
    for (int row = 0; row < table->rowCount(); ++row)
        names << table->item(row, 1)->text();
    QVERIFY(names.join(QStringLiteral("|")).contains(
        QStringLiteral("Зажигание пиросвечей двигателей 9–12")));
    QVERIFY(names.join(QStringLiteral("|")).contains(QStringLiteral("КОНТАКТ ПОДЪЁМА")));
    QVERIFY(!names.join(QStringLiteral("|")).contains(QStringLiteral("CLOSE_MAIN_VALVES")));
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 1);
}

void OperatorGuiTest::showsDualLegacyComStatuses() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = QStringLiteral("NONEXISTENT_TEST_COM_A");
    options.secondLegacySerialPort = QStringLiteral("NONEXISTENT_TEST_COM_B");
    IntegrationWindow window(options);
    window.show();
    auto *status = window.findChild<QLabel *>(QStringLiteral("legacyStreamsStatus"));
    QVERIFY(status);
    QVERIFY(status->text().contains(QStringLiteral("БКД-А-1")));
    QVERIFY(status->text().contains(QStringLiteral("БКД-Б")));
    QVERIFY(status->text().contains(QStringLiteral("СБОР НЕДОСТОВЕРЕН")));
}

void OperatorGuiTest::fullPlanKeepsTableUsableAndHardwareClockStable() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = "NONEXISTENT_TEST_A";
    options.secondLegacySerialPort = "NONEXISTENT_TEST_B";
    IntegrationWindow window(options);
    window.show();
    QVERIFY(!window.m_summaryLabel->text().contains("/ -1"));
    QByteArray file = "START_UTC_DATE = 2026-10-13\nSTART_UTC_TIME = 12:00:00\n";
    const QHash<int, QByteArray> keys = {
        {3, "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"},
        {5, "FIRE_STARTING_CHAMBER_ENGINES_9_TO_12"},
        {7, "LIFT_OFF_CONTACT"}, {16, "CLOSE_MAIN_VALVES_ENGINES_9_TO_12"},
        {20, "IGNITE_PYRO_CANDLE"}, {21, "OPEN_OXIDIZER_AND_FUEL_VALVES"},
        {22, "SHUTDOWN_ENGINE_BLOCK_B"}};
    for (int i = 1; i <= 22; ++i)
        file += keys.value(i, "OTHER_" + QByteArray::number(i)) + " = " + QByteArray::number(i) + '\n';
    QJsonObject event{{"contract", "kasu.pp.debug.stand.v1"}, {"type", "CYCLOGRAM_AVAILABLE"},
        {"seq", "1"}, {"run_id", "run"}, {"stream_id", "stream"}, {"upload_id", "upload"},
        {"byte_count", file.size()}, {"sha256_hex", QString::fromLatin1(
            QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex())},
        {"content_base64", QString::fromLatin1(file.toBase64())}};
    window.m_session.processDebug(event);
    window.refresh();
    auto *table = window.m_eventTable;
    QCOMPARE(table->rowCount(), 21);
    QTRY_VERIFY(table->viewport()->height() >= 6 * table->rowHeight(0));
    QCOMPARE(window.size(), QSize(1280, 800));
    QVERIFY(table->columnWidth(1) > table->columnWidth(2));

    event["type"] = "CYCLOGRAM_UPLOAD_STATE";
    event["seq"] = "2";
    event["upload_state"] = "CONFIRMED";
    window.m_session.processDebug(event);
    event["type"] = "BCVM_EXECUTION_ENABLED";
    event["seq"] = "3";
    window.m_session.processDebug(event);
    window.m_session.m_awaitReset = true;
    window.m_session.processLegacyBytes(QByteArray(1, char(0)), 0);
    window.m_session.processLegacyBytes(QByteArray(1, char(0)), 1);
    window.m_session.processLegacyBytes(QByteArray(61001, char(0x80)), 0);
    window.refresh();
    QCOMPARE(window.m_timerLabel->text(), QStringLiteral("01:01.0"));
    QVERIFY(window.m_timerLabel->styleSheet().contains("font-size:52px"));
    QVERIFY(window.m_timerLabel->styleSheet().contains("font-weight:600"));
    QTest::qWait(30);
    window.updatePlannedClock();
    QCOMPARE(window.m_timerLabel->text(), QStringLiteral("01:01.0"));
}

void OperatorGuiTest::kpClusterDoesNotHideFailure() {
    TimelineWidget timeline;
    timeline.resize(800, 72);
    EventRow kp;
    kp.id = 1; kp.time_ms = 0; kp.hasChannels = true;
    EventRow failed;
    failed.id = 2; failed.time_ms = 1; failed.hasChannels = true;
    timeline.setEvents({kp, failed});
    timeline.markEventFired(1, "ok");
    timeline.markEventFired(2, "fail");
    const QImage image = timeline.grab().toImage();
    // The rendered failure must remain visible even when its node shares KP.
    const QColor failure("#f85149");
    bool visibleFailure = false;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            visibleFailure |= image.pixelColor(x, y) == failure ||
                              image.pixelColor(x, y) == failure.darker(130);
    QVERIFY(visibleFailure);
}

QTEST_MAIN(OperatorGuiTest)
#include "tst_operator_gui.moc"
