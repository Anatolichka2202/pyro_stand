#include "integration_window.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
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

QTEST_MAIN(OperatorGuiTest)
#include "tst_operator_gui.moc"
