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
#include <algorithm>

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

QByteArray benchCyclogram() {
    QByteArray bytes = "START_UTC_DATE = 2026-10-12\nSTART_UTC_TIME = 12:00:00\n";
    const QHash<int, QByteArray> keys = {
        {3, "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"},
        {5, "FIRE_STARTING_CHAMBER_ENGINES_9_TO_12"},
        {7, "LIFT_OFF_CONTACT"}, {16, "CLOSE_MAIN_VALVES_ENGINES_9_TO_12"},
        {20, "IGNITE_PYRO_CANDLE"}, {21, "OPEN_OXIDIZER_AND_FUEL_VALVES"},
        {22, "SHUTDOWN_ENGINE_BLOCK_B"}};
    const QHash<int, int> offsets = {{3,-3},{5,-1},{7,0},{16,2},{20,-3},{21,-1},{22,2}};
    for (int i = 1; i <= 22; ++i)
        bytes += keys.value(i, "OTHER_" + QByteArray::number(i)) + " = " +
                 QByteArray::number(offsets.value(i, i * 10)) + '\n';
    return bytes;
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
    void builtInBenchProfile();
    void legacyDecoder();
    void legacyIndependentCounters();
};

void IntegrationTest::builtInBenchProfile() {
    CyclogramPlan plan;
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(benchCyclogram(), &plan, &error));
    const auto mapped = StandProtocol::benchMapping(4, 9, 4, plan);
    QVERIFY(mapped.warnings.isEmpty());
    QCOMPARE(mapped.kp.index, 31);
    QCOMPARE(mapped.actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12").inputs[0].index, 24);
    QCOMPARE(mapped.actions.value("IGNITE_PYRO_CANDLE").inputs[0].index, 64);
    QCOMPARE(mapped.actions.value("SHUTDOWN_ENGINE_BLOCK_B").inputs[1].index, 70);
    QCOMPARE(mapped.actions.value("SHUTDOWN_ENGINE_BLOCK_B").toleranceMs.value(), 1.0);
    // Item numbers refer to original cyclogram order, not sorted display order.
    QCOMPARE(plan.actions[0].ordinal, 3);
    const auto unassigned = StandProtocol::benchMapping(0, 0, 0, plan);
    QVERIFY(unassigned.actions.isEmpty());
    QVERIFY(unassigned.kp.index < 0);
    for (auto &action : plan.actions)
        if (action.ordinal == 22) action.key = "FAIRING_SEPARATION";
    const auto mismatch = StandProtocol::benchMapping(4, 9, 4, plan);
    QVERIFY(!mismatch.actions.contains("FAIRING_SEPARATION"));
    QVERIFY(!mismatch.warnings.isEmpty());
}

void IntegrationTest::legacyDecoder() {
    LegacyTesterDecoder decoder;
    decoder.append(0);
    decoder.append(1);
    decoder.append(3);
    decoder.append(0x83);
    const auto frame = decoder.append(0x80);
    QCOMPARE(frame.channels[0].riseUs, quint32(2000));
    QCOMPARE(frame.channels[1].riseUs, quint32(3000));
    QCOMPARE(frame.channels[7].riseUs, quint32(4000));
    QCOMPARE(frame.channels[0].fallUs, quint32(5000));
    QVERIFY(!frame.channels[0].ready);
    QCOMPARE(decoder.sampleCount(), quint64(5));
    decoder.append(0x81); // repeated pulse does not overwrite the first front
    QCOMPARE(decoder.append(0x81).channels[0].riseUs, quint32(2000));
    decoder.reset();
    QCOMPARE(decoder.sampleCount(), quint64(0));
    QVERIFY(!decoder.append(0).channels[0].triggered);
}

void IntegrationTest::legacyIndependentCounters() {
    IntegrationSession::Options options;
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = "TEST-A";
    options.secondLegacySerialPort = "TEST-B";
    IntegrationSession session(options);
    const QByteArray file = benchCyclogram();
    QJsonObject available = contract("CYCLOGRAM_AVAILABLE", 1, "run", "stream");
    available.insert("upload_id", "upload");
    available.insert("byte_count", file.size());
    available.insert("sha256_hex", QString::fromLatin1(QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex()));
    available.insert("content_base64", QString::fromLatin1(file.toBase64()));
    session.processDebug(available);
    QJsonObject upload = contract("CYCLOGRAM_UPLOAD_STATE", 2, "run", "stream");
    upload.insert("upload_id", "upload"); upload.insert("upload_state", "CONFIRMED");
    session.processDebug(upload);
    QJsonObject enabled = contract("BCVM_EXECUTION_ENABLED", 3, "run", "stream");
    enabled.insert("upload_id", "upload"); session.processDebug(enabled);
    session.m_awaitReset = true; // local collection request, no physical UART in this test
    QByteArray first(15, char(0));
    for (int i = 6; i < first.size(); ++i) first[i] = char(i == 6 ? 1 : 3);
    for (int i = 9; i < first.size(); ++i) first[i] = char(quint8(first[i]) | 0x80);
    session.processLegacyBytes(first.first(8), 0);
    session.processLegacyBytes(first.mid(8), 0);
    QVERIFY(!session.boardArmed()); // second COM has no baseline yet
    QByteArray second(35, char(0));
    for (int i = 26; i < second.size(); ++i) second[i] = char(i == 26 ? 1 : 3);
    for (int i = 29; i < second.size(); ++i) second[i] = char(quint8(second[i]) | 0x80);
    session.processLegacyBytes(second.first(29), 1);
    QVERIFY(session.boardArmed());
    const auto firstAction = *std::find_if(session.plan().actions.cbegin(), session.plan().actions.cend(),
        [](const auto &a) { return a.ordinal == 3; });
    const auto secondAction = *std::find_if(session.plan().actions.cbegin(), session.plan().actions.cend(),
        [](const auto &a) { return a.ordinal == 20; });
    QCOMPARE(session.measureAction(firstAction).channels[0].actualMs.value(), -3.0);
    QCOMPARE(session.measuredPositionMs().value(), 5.0);
    QCOMPARE(session.measureAction(secondAction).state, ActionMeasurement::State::WaitingForKp);
    session.processLegacyBytes(second.mid(29), 1);
    QCOMPARE(session.measureAction(secondAction).channels[0].actualMs.value(), -3.0);
    QCOMPARE(session.measureAction(secondAction).channels[1].actualMs.value(), -2.0);
    QCOMPARE(session.measuredPositionMs().value(), 5.0); // delivery of the other UART cannot move A's clock
    QCOMPARE(session.measureAction(firstAction).channels[1].deviationMs.value(), 1.0);
    session.m_boardArmed = false; // a UART error / detected gap invalidates the collection
    QCOMPARE(session.measureAction(firstAction).state, ActionMeasurement::State::BoardNotArmed);

    // One old eight-channel board can be assigned to either block, without a JSON file.
    options.secondLegacySerialPort.clear();
    for (bool blockB : {false, true}) {
        options.legacySecondStage = blockB;
        IntegrationSession single(options);
        single.processDebug(available); single.processDebug(upload); single.processDebug(enabled);
        single.m_awaitReset = true;
        single.processLegacyBytes(first);
        QVERIFY(single.boardArmed());
        const auto selected = single.measureAction(blockB ? secondAction : firstAction);
        QCOMPARE(selected.state, ActionMeasurement::State::Measured);
        QCOMPARE(selected.channels[0].actualMs.value(), -3.0);
        QCOMPARE(single.measureAction(blockB ? firstAction : secondAction).state,
                 ActionMeasurement::State::Unmapped);
    }
}

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
    QCOMPARE(mapping.actions.value(QStringLiteral("IGNITE_PYRO_CANDLES_ENGINES_1_TO_8")).toleranceMs.value(), 2.0);
    const QByteArray defaultTolerance = R"({"version":"bench-default","kp":{"yps":1,"channel":1},"actions":[{"key":"A","inputs":[{"yps":1,"channel":2}]}]})";
    QVERIFY(StandProtocol::parseMapping(defaultTolerance, &mapping, &error));
    QCOMPARE(mapping.actions.value(QStringLiteral("A")).toleranceMs.value(), 1.0);
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
    const ActionMeasurement onTime = session.measureAction(session.plan().actions[0]);
    QCOMPARE(int(onTime.state), int(ActionMeasurement::State::Measured));
    QCOMPARE(onTime.channels.size(), 1);
    QCOMPARE(onTime.channels[0].actualMs.value(), 20.0);
    QCOMPARE(onTime.channels[0].deviationMs.value(), 0.0);
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
    const ActionMeasurement late = session.measureAction(session.plan().actions[0]);
    QCOMPARE(int(late.state), int(ActionMeasurement::State::Measured));
    QCOMPARE(late.channels[0].actualMs.value(), 27.296);
    QVERIFY(qAbs(late.channels[0].deviationMs.value() - 7.296) < 1e-9);

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
