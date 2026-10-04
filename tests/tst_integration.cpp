#include "integration.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QUuid>
#include <QtEndian>

namespace {
QByteArray frame(quint32 kp = 0, quint32 action = 0) {
    QByteArray bytes(963, char(0));
    bytes[0] = char(0xAA);
    bytes[1] = char(0x55);
    auto setRise = [&](int index, quint32 time) {
        const int offset = 2 + index * 10;
        bytes[offset] = time ? 1 : 0;
        qToLittleEndian<quint32>(time, reinterpret_cast<uchar *>(bytes.data() + offset + 2));
    };
    setRise(0, kp);
    setRise(1, action);
    quint8 crc = 0;
    for (int i = 0; i < bytes.size() - 1; ++i) {
        crc ^= quint8(bytes[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = crc & 0x80 ? quint8((crc << 1) ^ 0x31) : quint8(crc << 1);
    }
    bytes[962] = char(crc);
    return bytes;
}

QByteArray cyclogram() {
    return "START_UTC_DATE = 2026-10-03\nSTART_UTC_TIME = 12:00:00\n"
           "IGNITE_PYRO_CANDLES_ENGINES_1_TO_8 = 20\n";
}

QJsonObject contract(QString type, quint64 seq, QString run, QString stream) {
    QJsonObject object;
    object.insert("contract", "kasu.pp.debug.stand.v1");
    object.insert("type", type);
    object.insert("seq", QString::number(seq));
    object.insert("run_id", run);
    object.insert("stream_id", stream);
    return object;
}

void send(QLocalSocket *socket, const QJsonObject &object) {
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
    socket->flush();
}
}

class IntegrationTest : public QObject {
    Q_OBJECT
private slots:
    void boardFrameValidation();
    void cyclogramAndMapping();
    void twoExternalStreams();
};

void IntegrationTest::boardFrameValidation() {
    BoardFrame parsed;
    QString error;
    QByteArray valid = frame(1000000, 1020000);
    QVERIFY(StandProtocol::parseBoardFrame(valid, &parsed, &error));
    QCOMPARE(parsed.channels[0].riseUs, quint32(1000000));
    QCOMPARE(parsed.channels[1].riseUs, quint32(1020000));
    valid[12] = char(quint8(valid[12]) ^ 1);
    QVERIFY(!StandProtocol::parseBoardFrame(valid, &parsed, &error));
    QVERIFY(error.contains("CRC8"));
    QCOMPARE(StandProtocol::resetCommand().size(), 4);
}

void IntegrationTest::cyclogramAndMapping() {
    CyclogramPlan plan;
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(), &plan, &error));
    QCOMPARE(plan.actions.size(), 1);
    QCOMPARE(plan.actions[0].offsetMs, 20);
    QVERIFY(!StandProtocol::parseCyclogram("START_UTC_TIME = 12:00:00\nACTION = 0\n", &plan, &error));
    StandMapping mapping;
    const QByteArray config = R"({"version":"bench-1","kp":{"yps":1,"channel":1},"actions":[{"key":"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8","inputs":[{"yps":1,"channel":2}],"tolerance_ms":2}]})";
    QVERIFY(StandProtocol::parseMapping(config, &mapping, &error));
    QCOMPARE(mapping.kp.index, 0);
    QCOMPARE(mapping.actions.size(), 1);
    const QByteArray duplicate = R"({"version":"bench-1","kp":{"yps":1,"channel":1},"actions":[{"key":"A","inputs":[{"yps":1,"channel":2}]},{"key":"B","inputs":[{"yps":1,"channel":2}]}]})";
    QVERIFY(!StandProtocol::parseMapping(duplicate, &mapping, &error));
}

void IntegrationTest::twoExternalStreams() {
    const QString socketName = "pyro-integration-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QLocalServer server;
    QVERIFY(server.listen(socketName));
    QUdpSocket reserved;
    QVERIFY(reserved.bind(QHostAddress::LocalHost, 0));
    const quint16 telemetryPort = reserved.localPort();
    reserved.close();
    QUdpSocket board;
    QVERIFY(board.bind(QHostAddress::LocalHost, 0));

    IntegrationSession::Options options;
    options.debugSocket = socketName;
    options.boardIp = QHostAddress::LocalHost;
    options.bindIp = QHostAddress::LocalHost;
    options.telemetryPort = telemetryPort;
    options.commandPort = board.localPort();
    IntegrationSession session(options);
    QSignalSpy changed(&session, &IntegrationSession::changed);
    QTemporaryFile mappingFile;
    QVERIFY(mappingFile.open());
    const QByteArray map = R"({"version":"bench-1","kp":{"yps":1,"channel":1},"actions":[{"key":"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8","inputs":[{"yps":1,"channel":2}],"tolerance_ms":2}]})";
    QCOMPARE(mappingFile.write(map), qint64(map.size()));
    mappingFile.flush();
    QVERIFY(session.loadMapping(mappingFile.fileName()));
    session.start();
    QTRY_VERIFY(server.hasPendingConnections());
    QLocalSocket *client = server.nextPendingConnection();
    QVERIFY(client);

    const QString run = "run-1", stream = "stream-1", id = "upload-1";
    const QByteArray file = cyclogram();
    QJsonObject available = contract("CYCLOGRAM_AVAILABLE", 1, run, stream);
    available.insert("upload_id", id);
    available.insert("byte_count", file.size());
    available.insert("sha256_hex", QString::fromLatin1(QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex()));
    available.insert("content_base64", QString::fromLatin1(file.toBase64()));
    send(client, available);
    QTRY_VERIFY(session.hasPlan());
    QCOMPARE(session.uploadState(), QString("PREPARED"));
    QVERIFY(!session.executionEnabled());

    QJsonObject upload = contract("CYCLOGRAM_UPLOAD_STATE", 2, run, stream);
    upload.insert("upload_id", id);
    upload.insert("upload_state", "CONFIRMED");
    send(client, upload);
    QTRY_COMPARE(session.uploadState(), QString("CONFIRMED"));
    QJsonObject enabled = contract("BCVM_EXECUTION_ENABLED", 3, run, stream);
    enabled.insert("upload_id", id);
    send(client, enabled);
    QTRY_VERIFY(session.executionEnabled());
    QVERIFY(!session.kpTimeUs());

    const QByteArray blank = frame();
    QVERIFY(session.resetBoard());
    QTRY_VERIFY(board.hasPendingDatagrams());
    QByteArray command;
    command.resize(int(board.pendingDatagramSize()));
    board.readDatagram(command.data(), command.size());
    QCOMPARE(command, StandProtocol::resetCommand());
    QCOMPARE(board.writeDatagram(blank, QHostAddress::LocalHost, telemetryPort), qint64(blank.size()));
    QTRY_VERIFY(session.boardArmed());
    const QByteArray measured = frame(1000000, 1020000);
    QCOMPARE(board.writeDatagram(measured, QHostAddress::LocalHost, telemetryPort), qint64(measured.size()));
    QTRY_VERIFY(session.kpTimeUs().has_value());
    QCOMPARE(*session.kpTimeUs(), quint32(1000000));
    QVERIFY(session.actionResult(session.plan().actions[0]).contains("в допуске"));
    QTemporaryDir reportDir;
    QVERIFY(reportDir.isValid());
    QString reportError;
    const QString reportPath = reportDir.filePath("report.json");
    QVERIFY2(session.saveReport(reportPath, &reportError), qPrintable(reportError));
    QFile reportFile(reportPath);
    QVERIFY(reportFile.open(QIODevice::ReadOnly));
    const QJsonObject report = QJsonDocument::fromJson(reportFile.readAll()).object();
    QCOMPARE(report.value("upload_id").toString(), id);
    QCOMPARE(report.value("mapping_version").toString(), QString("bench-1"));
    QVERIFY(report.value("actions").toArray().at(0).toObject().value("result").toString().contains("в допуске"));
    QFile evidence(reportPath + ".frames.jsonl");
    QVERIFY(evidence.open(QIODevice::ReadOnly));
    QVERIFY(evidence.readAll().contains(measured.toBase64()));
    const QByteArray wrapped = frame(4294960000U, 20000U);
    QCOMPARE(board.writeDatagram(wrapped, QHostAddress::LocalHost, telemetryPort), qint64(wrapped.size()));
    QTRY_VERIFY(session.actionResult(session.plan().actions[0]).contains("27.296"));

    QJsonObject stale = contract("BCVM_STATE", 4, run, stream);
    stale.insert("fresh", false);
    stale.insert("lock_state", "UNLOCKED");
    stale.insert("execution_enabled_confirmed", true);
    stale.insert("upload_id", id);
    send(client, stale);
    QTRY_COMPARE(session.lockState(), QString("UNKNOWN"));
    QVERIFY(!session.executionEnabled());

    QJsonObject corrupt = contract("CYCLOGRAM_AVAILABLE", 5, run, stream);
    corrupt.insert("upload_id", "upload-2");
    corrupt.insert("byte_count", file.size());
    corrupt.insert("sha256_hex", QString(64, u'0'));
    corrupt.insert("content_base64", QString::fromLatin1(file.toBase64()));
    send(client, corrupt);
    QTRY_VERIFY(session.lastError().contains("SHA-256"));
    QCOMPARE(session.uploadId(), id);

    QJsonObject failed = contract("CYCLOGRAM_UPLOAD_STATE", 6, run, stream);
    failed.insert("upload_id", id);
    failed.insert("upload_state", "FAILED");
    send(client, failed);
    QTRY_COMPARE(session.uploadState(), QString("FAILED"));
    QVERIFY(session.actionResult(session.plan().actions[0]).contains("не подтверждена"));
    QJsonObject falseEnable = contract("BCVM_EXECUTION_ENABLED", 7, run, stream);
    falseEnable.insert("upload_id", id);
    send(client, falseEnable);
    QCoreApplication::processEvents();
    QVERIFY(!session.executionEnabled());
    QVERIFY(changed.count() > 0);
}

QTEST_GUILESS_MAIN(IntegrationTest)
#include "tst_integration.moc"
