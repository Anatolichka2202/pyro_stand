#include "integration.h"
#include "command_catalog.h"

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
#include <QUdpSocket>
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
    setRise(2, action);
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
           "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = 20\nLIFT_OFF_CONTACT = 0\n";
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
        {24, "SHUTDOWN_ENGINE_BLOCK_B"}};
    const QHash<int, int> offsets = {{3,-3},{5,-1},{7,0},{16,2},{20,-3},{21,-1},{24,2}};
    for (int i = 1; i <= 24; ++i)
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
    void rejectedObserversCoverUdpAndSerial();
    void newBoardLinkLossIsIncomplete_data();
    void newBoardLinkLossIsIncomplete();
    void legacyProfileRejectsForeignKp_data();
    void legacyProfileRejectsForeignKp();
    void legacyProfileAcceptsOwnKp();
    void passiveObserversWithoutPlanDoNotGrowFlightArchive();
    void emptyStartupFramePreparesNewBoard();
    void lateEmptyFrameCannotBypassFailedReset();
    void legacyLateEventGetsFiveSecondTail();
    void newBoardFinishesFiveSecondsAfterTrackedEvent();
    void newBoardYpsSevenAndEightShareKp();
    void resetDoesNotCarryOldFrameIntoNewRun();
    void legacyRepeatedPulseIsNotSingleMeasurement();
    void repeatedLegacyKpIsNotAnAnchor();
    void finishKeepsCounterAmbiguity();
    void debugHistoryHasExplicitBound();
    void annotatedWiring();
    void invalidAnnotations_data();
    void invalidAnnotations();
    void workingTxtProvenanceAndReordering();
    void kpTimeVectors_data();
    void kpTimeVectors();
    void missingReferencesAndRepeatedActions();
    void profileChangeInvalidatesEvidence();
    void signalWiringAcrossGroups();
    void conflictingFileIdentity();
    void boardFrameValidation();
    void cyclogramAndMapping();
    void twoExternalStreams();
    void builtInBenchProfile();
    void currentOwnerCyclogramMapping();
    void legacyDecoder();
    void legacyIndependentCounters();
    void legacyStreamHealth();
    void evidenceIsolation();
    void resetDiscardsPartialSerialFrame();
    void failedExportPreservesEvidence();
    void legacyTicksFinishCollection();
    void liveFrameKeepsReceivingAfterCompletion();
    void mismatchedUdpSenderIsCounted();
    void finishedLegacyCollectIsImmutable();
    void missingInputsTurnIntoFailureAfterFinish();
    void dualLegacyWaitsForBothCounters();
    void manualEarlyFinishNeverClaimsMissedPulse();
    void actual33ActionCyclogramHasUnambiguousChannels();
    void completedActionStateIsFrozenAcrossLateDebugChanges();
    void transientKpRemainsAfterNextUdpFrame();
};

void IntegrationTest::rejectedObserversCoverUdpAndSerial() {
    IntegrationSession session({});
    QSignalSpy rejected(&session, &IntegrationSession::frameRejected);
    session.processFrame("short");
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].toByteArray(), QByteArray("short"));
    QVERIFY(!rejected[0][1].toString().isEmpty());
    auto invalid = frame(); invalid[962] = char(quint8(invalid[962]) ^ 1);
    session.processFrame(invalid); QCOMPARE(rejected.count(), 2);
    session.m_collectionFinished = true;
    session.processFrame(invalid); QCOMPARE(rejected.count(), 3);
    session.m_options.boardTransport = IntegrationSession::BoardTransport::Serial;
    session.m_serialBuffer = invalid; session.receiveSerial();
    QCOMPARE(rejected.count(), 4);
    QCOMPARE(rejected.last()[0].toByteArray(), invalid);
    QVERIFY(!rejected.last()[1].toString().isEmpty());
}

void IntegrationTest::newBoardLinkLossIsIncomplete_data() {
    QTest::addColumn<bool>("partial");
    QTest::addColumn<int>("failure");
    for (int failure = 0; failure < 3; ++failure) {
        QTest::addRow("missing-%d", failure) << false << failure;
        QTest::addRow("partial-%d", failure) << true << failure;
    }
}

void IntegrationTest::newBoardLinkLossIsIncomplete() {
    QFETCH(bool, partial);
    QFETCH(int, failure);
    IntegrationSession::Options options;
    if (failure) options.boardTransport = IntegrationSession::BoardTransport::Serial;
    IntegrationSession session(options); QString error;
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(), &session.m_plan, &error));
    session.m_mapping = StandProtocol::benchMapping(1, 0, 1, session.m_plan);
    session.m_hasPlan = session.m_boardArmed = session.m_hasBoardFrame = true;
    session.m_boardReceiving = session.m_executionObserved = true;
    session.m_uploadState = "CONFIRMED";
    session.m_boardFrame.channels[7] = {true, false, 1000000, 0};
    if (partial) session.m_boardFrame.channels[0] = {true, false, 1020000, 0};
    const auto action = session.m_plan.actions.last();
    const auto disconnect = [&] {
        if (failure)
            emit session.m_serial.errorOccurred(failure == 1 ? QSerialPort::ResourceError : QSerialPort::ReadError);
        else QMetaObject::invokeMethod(&session.m_boardWatchdog, "timeout", Qt::DirectConnection);
    };
    disconnect();
    QVERIFY(session.collectionFinished());
    QVERIFY(!session.plannedWindowComplete());
    const auto result = session.measureAction(action);
    QVERIFY(result.state != ActionMeasurement::State::MissingAfterFinish);
    QVERIFY(result.state != ActionMeasurement::State::PartialAfterFinish);
    QVERIFY(result.reason.contains(QStringLiteral("связ")));
    if (partial) {
        QVERIFY(result.channels.first().actualMs.has_value());
        QCOMPARE(*result.channels.first().actualMs, 20.0);
    }
    const auto channels = result.channels.size();
    session.processFrame(frame(1000000, 1020000));
    QCOMPARE(session.measureAction(action).state, result.state);
    QCOMPARE(session.measureAction(action).reason, result.reason);
    QCOMPARE(session.measureAction(action).channels.size(), channels);
    if (partial) QCOMPARE(*session.measureAction(action).channels.first().actualMs, 20.0);
    QTemporaryDir dir; QVERIFY(session.saveReport(dir.filePath("interrupted.json"), &error));
    QFile report(dir.filePath("interrupted.json")); QVERIFY(report.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(report.readAll()).object().value("observation_interrupted").toBool());
    session.resetCollectionEvidence(false);
    QVERIFY(!session.collectionFinished());
    // Поздний timeout после штатного finish не переоценивает завершённый результат.
    session.m_boardReceiving = true;
    session.finishCollection(QStringLiteral("complete"), true);
    disconnect();
    QVERIFY(session.plannedWindowComplete());
    QVERIFY(session.saveReport(dir.filePath("complete.json"), &error));
    QFile complete(dir.filePath("complete.json")); QVERIFY(complete.open(QIODevice::ReadOnly));
    QVERIFY(!QJsonDocument::fromJson(complete.readAll()).object().value("observation_interrupted").toBool());
}

void IntegrationTest::legacyProfileRejectsForeignKp_data() {
    QTest::addColumn<int>("yps"); QTest::addColumn<int>("channel"); QTest::addColumn<QString>("edge");
    QTest::newRow("second-COM") << 2 << 8 << QStringLiteral("rise");
    QTest::newRow("unavailable-group") << 12 << 8 << QStringLiteral("rise");
    QTest::newRow("wrong-channel") << 1 << 3 << QStringLiteral("rise");
    QTest::newRow("wrong-edge") << 1 << 8 << QStringLiteral("fall");
}

void IntegrationTest::legacyProfileRejectsForeignKp() {
    QFETCH(int, yps); QFETCH(int, channel); QFETCH(QString, edge);
    IntegrationSession::Options options; options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.secondLegacySerialPort = "COM-test";
    IntegrationSession session(options);
    QTemporaryFile file; QVERIFY(file.open());
    file.write(QJsonDocument(QJsonObject{{"version", "regression"},
        {"kp", QJsonObject{{"yps", yps}, {"channel", channel}, {"edge", edge}}},
        {"actions", QJsonArray{QJsonObject{{"key", "IGNITE_PYRO_CANDLE"},
            {"inputs", QJsonArray{QJsonObject{{"yps", 1}, {"channel", 1}}}}}}}}).toJson());
    file.flush();
    const auto collection = session.collectionId(); const auto mapping = session.mapping().version;
    QVERIFY(!session.loadMapping(file.fileName()));
    QCOMPARE(session.collectionId(), collection); QCOMPARE(session.mapping().version, mapping);
    QVERIFY(session.lastError().contains(QStringLiteral("КП")));
}

void IntegrationTest::legacyProfileAcceptsOwnKp() {
    IntegrationSession::Options options; options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.secondLegacySerialPort = "COM-test";
    IntegrationSession session(options);
    QTemporaryFile file; QVERIFY(file.open());
    file.write(R"({"version":"own-kp","kp":{"yps":1,"channel":8},"actions":[{"key":"X","inputs":[{"yps":1,"channel":1}]}]})");
    file.flush(); QVERIFY(session.loadMapping(file.fileName()));
    session.m_boardArmed = session.m_hasBoardFrame = session.m_executionObserved = true;
    session.m_uploadState = "CONFIRMED";
    session.m_boardFrame.channels[7] = {true, false, 1000, 0};
    session.m_boardFrame.channels[15] = {true, false, 9000, 0};
    for (int i = 0; i < 10; ++i) session.m_legacy[0].append(0);
    QCOMPARE(*session.measuredPositionMs(), 9.0);
    // Защита расчёта действует и при обходе загрузчика профиля.
    session.m_mapping.kp.index = 15;
    QVERIFY(!session.kpTimeUs()); QVERIFY(!session.measuredPositionMs());
}

void IntegrationTest::lateEmptyFrameCannotBypassFailedReset() {
    QUdpSocket receiver; QVERIFY(receiver.bind(QHostAddress::LocalHost,0));
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost; options.commandPort=receiver.localPort();
    IntegrationSession session(options);
    QVERIFY(session.resetBoard());
    QVERIFY(QMetaObject::invokeMethod(&session.m_resetWatchdog,"timeout",Qt::DirectConnection));
    QVERIFY(!session.m_awaitReset);
    session.applyFrame(BoardFrame{}); QVERIFY(!session.boardArmed());
    QVERIFY(session.resetBoard()); session.applyFrame(BoardFrame{}); QVERIFY(session.boardArmed());
    QCOMPARE(session.m_localEvidence.last().toObject().value("type").toString(),QString("BOARD_EMPTY_FRAME_AFTER_RESET"));
}

void IntegrationTest::legacyLateEventGetsFiveSecondTail() {
    IntegrationSession::Options options; options.debugSocket.clear();
    options.boardTransport=IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options); QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",&session.m_plan,&error));
    session.m_hasPlan=true; session.updateBenchMapping();
    session.m_boardArmed=true; session.m_hasBoardFrame=true;
    session.m_boardFrame.channels[7]={true,false,1000,0};
    session.m_boardFrame.channels[0]={true,false,10001000,0};
    session.m_boardFrame.channels[1]=session.m_boardFrame.channels[0];
    for(int i=0;i<10002;++i) session.m_legacy[0].append(0);
    session.checkCompletion(); QVERIFY(!session.collectionFinished());
    for(int i=0;i<5000;++i) session.m_legacy[0].append(0);
    session.checkCompletion(); QVERIFY(session.collectionFinished());
    // Во втором COM другая опора: его предел нельзя считать от КП первого.
    options.secondLegacySerialPort="TEST_SECOND_COM";
    IntegrationSession dual(options);
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\nLIFT_OFF_CONTACT=0\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\nIGNITE_PYRO_CANDLE=20\n",
        &dual.m_plan,&error));
    dual.m_hasPlan=true; dual.updateBenchMapping();
    dual.m_boardArmed=true; dual.m_hasBoardFrame=true; dual.m_legacyArmed={true,true};
    dual.m_boardFrame.channels[7]={true,false,1000,0};
    dual.m_boardFrame.channels[15]={true,false,99000,0};
    for(int index:{0,1}) dual.m_boardFrame.channels[size_t(index)]={true,false,21000,0};
    for(int index:{8,9}) dual.m_boardFrame.channels[size_t(index)]={true,false,10099000,0};
    for(int i=0;i<20000;++i) dual.m_legacy[0].append(0);
    for(int i=0;i<15098;++i) dual.m_legacy[1].append(0);
    dual.checkCompletion(); QVERIFY(!dual.collectionFinished());
    dual.m_legacy[1].append(0);
    dual.checkCompletion(); QVERIFY(dual.collectionFinished());
}

void IntegrationTest::emptyStartupFramePreparesNewBoard() {
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort=0;
    IntegrationSession session(options);
    // Старые сохранённые фронты сами по себе не разрешают новый сбор.
    BoardFrame stale; stale.channels[0]={true,false,123000,0};
    session.applyFrame(stale); QVERIFY(!session.boardArmed());
    session.applyFrame(BoardFrame{});
    QVERIFY(session.boardArmed());
    QVERIFY(!session.boardFrame().channels[0].triggered);
    // Потеря связи после готовности требует явного нового сбора.
    session.m_boardArmed=false;
    session.applyFrame(BoardFrame{});
    QVERIFY(!session.boardArmed());
}

void IntegrationTest::newBoardFinishesFiveSecondsAfterTrackedEvent() {
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort=0;
    options.firstStageYps=8; options.secondStageYps=7; options.kpYps=8;
    IntegrationSession session(options); QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\nLIFT_OFF_CONTACT=0\n"
        "IGNITE_PYRO_CANDLE=20\nINFORMATION_ONLY=100000\n", &session.m_plan,&error));
    session.m_hasPlan=true; session.m_uploadState="CONFIRMED"; session.m_executionObserved=true;
    session.m_boardArmed=true; session.m_hasBoardFrame=true; session.updateBenchMapping();
    session.m_boardFrame.channels[63]={true,false,1000000,0};
    session.m_boardFrame.channels[48]={true,false,1020000,0};
    session.m_boardFrame.channels[49]={true,false,1020000,0};
    session.checkCompletion();
    QTest::qWait(4900); session.checkCompletion();
    QVERIFY(!session.collectionFinished());
    QTRY_VERIFY_WITH_TIMEOUT(session.collectionFinished(),700);
    QVERIFY(session.plannedWindowComplete());
    const auto frozen=session.boardFrame().channels[48].riseUs;
    BoardFrame late; late.channels[48]={true,false,2000000,0}; session.applyFrame(late);
    QCOMPARE(session.boardFrame().channels[48].riseUs,frozen);
}

void IntegrationTest::newBoardYpsSevenAndEightShareKp() {
    IntegrationSession::Options options;
    options.debugSocket.clear(); options.telemetryPort=0;
    options.firstStageYps=8; options.secondStageYps=7; options.kpYps=8;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8\n"
        "IGNITE_PYRO_CANDLE=20\nLIFT_OFF_CONTACT=0\n", &session.m_plan, &error));
    session.m_hasPlan=true; session.m_uploadState="CONFIRMED";
    session.m_executionObserved=true; session.m_boardArmed=true; session.m_hasBoardFrame=true;
    session.updateBenchMapping();
    QCOMPARE(session.mapping().kp.index,63);
    const auto &actions=session.plan().actions;
    const auto actionA=std::find_if(actions.cbegin(),actions.cend(),[](const auto &action) {
        return action.key=="IGNITE_PYRO_CANDLES_ENGINES_9_TO_12";
    });
    const auto actionB=std::find_if(actions.cbegin(),actions.cend(),[](const auto &action) {
        return action.key=="IGNITE_PYRO_CANDLE";
    });
    QVERIFY(actionA!=actions.cend()); QVERIFY(actionB!=actions.cend());
    const auto a=session.mapping().forAction(*actionA);
    const auto b=session.mapping().forAction(*actionB);
    QCOMPARE(a.inputs.size(),2); QCOMPARE(b.inputs.size(),2);
    QCOMPARE(a.inputs[0].index,56); QCOMPARE(a.inputs[1].index,57);
    QCOMPARE(b.inputs[0].index,48); QCOMPARE(b.inputs[1].index,49);
    // До КП сохраняем аппаратные отметки, но не придумываем относительное время.
    for (const auto &input:a.inputs) session.m_boardFrame.channels[size_t(input.index)]={true,false,992000,0};
    for (const auto &input:b.inputs) session.m_boardFrame.channels[size_t(input.index)]={true,false,1020500,0};
    QCOMPARE(session.measureAction(*actionA).state,ActionMeasurement::State::WaitingForKp);
    session.m_boardFrame.channels[63]={true,false,1000000,0};
    const auto measuredA=session.measureAction(*actionA);
    const auto measuredB=session.measureAction(*actionB);
    QCOMPARE(measuredA.channels.size(),2); QCOMPARE(measuredB.channels.size(),2);
    QCOMPARE(measuredA.channels[0].actualMs.value(),-8.0);
    QCOMPARE(measuredB.channels[0].actualMs.value(),20.5);
    QCOMPARE(measuredB.channels[0].deviationMs.value(),0.5);
    // Неиспользуемые соседние ЯПС не подменяют ни события, ни КП.
    session.m_boardFrame.channels[55]={true,false,5,0};
    session.m_boardFrame.channels[71]={true,false,123,0};
    QCOMPARE(session.kpTimeUs().value(),quint32(1000000));
    QCOMPARE(session.measureAction(*actionB).channels[0].actualMs.value(),20.5);
}

void IntegrationTest::debugHistoryHasExplicitBound() {
    IntegrationSession::Options options;options.debugSocket.clear();IntegrationSession session(options);
    QString error;QVERIFY(StandProtocol::parseCyclogram(cyclogram(),&session.m_plan,&error));session.m_hasPlan=true;
    const QJsonObject large{{"type","BCVM_STATE"},{"padding",QString(1024*1024,u'x')}};
    int recorded=0;
    while(!session.collectionFinished() && recorded<20) {
        if(session.appendDebugEvidence(large)) ++recorded;
    }
    QVERIFY(recorded>0 && recorded<20);QVERIFY(session.collectionFinished());
    QVERIFY(session.m_debugEvidenceBytes<=16*1024*1024);
    QCOMPARE(session.m_debugEvidence.size(),recorded);QVERIFY(!session.m_evidenceValid);
    QVERIFY(!session.saveReport("not-written.json",&error));
    session.resetCollectionEvidence(true);QCOMPARE(session.m_debugEvidenceBytes,qsizetype(0));
    QVERIFY(session.appendDebugEvidence(large));
    QLocalServer server;const auto name=QStringLiteral("history-recovery-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(server.listen(name));options.debugSocket=name;options.telemetryPort=0;
    options.bindIp=QHostAddress::LocalHost;options.boardIp=QHostAddress::LocalHost;
    IntegrationSession unplanned(options);unplanned.m_debugEvidenceBytes=16*1024*1024;
    QVERIFY(!unplanned.appendDebugEvidence(large));unplanned.start();QTRY_VERIFY(server.hasPendingConnections());
    auto *client=server.nextPendingConnection();QVERIFY(client);
    auto available=contract("CYCLOGRAM_AVAILABLE",1,"recovery-run","recovery-stream");
    available["upload_id"]="new-upload";available["byte_count"]=cyclogram().size();
    available["content_base64"]=QString::fromLatin1(cyclogram().toBase64());
    available["sha256_hex"]=QString::fromLatin1(QCryptographicHash::hash(cyclogram(),QCryptographicHash::Sha256).toHex());
    send(client,available);QTRY_VERIFY_WITH_TIMEOUT(unplanned.hasPlan(),1000);
    QVERIFY(unplanned.m_evidenceValid);QVERIFY(unplanned.m_debugEvidenceBytes<16*1024*1024);
}

void IntegrationTest::finishKeepsCounterAmbiguity() {
    IntegrationSession::Options options;options.boardTransport=IntegrationSession::BoardTransport::LegacySerial;
    options.debugSocket.clear();options.firstStageYps=1;options.kpYps=1;
    IntegrationSession session(options);QString error;
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(),&session.m_plan,&error));
    session.m_hasPlan=true;session.updateBenchMapping();session.m_boardArmed=true;session.m_hasBoardFrame=true;
    session.m_uploadState="CONFIRMED";session.m_executionObserved=true;
    session.m_boardFrame.channels[7]={true,false,1000000,0};
    session.m_boardFrame.channels[0]=session.m_boardFrame.channels[1]={true,false,1020000,0};
    for(int i=0;i<2147484;++i) session.m_legacy[0].append(0);
    const auto action=session.plan().actions[1];
    QCOMPARE(session.measureAction(action).state,ActionMeasurement::State::AmbiguousInterval);
    session.finishCollection();
    QCOMPARE(session.measureAction(action).state,ActionMeasurement::State::AmbiguousInterval);
    session.m_options.boardTransport=IntegrationSession::BoardTransport::Udp;
    session.m_collectionElapsed.start();session.m_durationAtFinishMs=2147484;
    session.m_finalActionMeasurements.clear();
    QCOMPARE(session.measureAction(action).state,ActionMeasurement::State::AmbiguousInterval);
}

void IntegrationTest::annotatedWiring() {
    const QByteArray source="START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8000 # Ignition ## board=NEW; link=ETH; bkd=A1; yps=9; out=1,2; in=3,4; edge=rise\n"
        "LIFT_OFF_CONTACT=0 # KP ## board=NEW; link=ETH; bkd=A1; yps=9; out=8; in=8; edge=rise; role=kp\n"
        "IGNITE_PYRO_CANDLE=100 # B ## board=OLD; link=COM9; bkd=B; yps=none; out=1,2; in=2,1; edge=fall\n";
    CyclogramPlan plan;QString error;
    QVERIFY2(StandProtocol::parseCyclogram(source,&plan,&error),qPrintable(error));
    QCOMPARE(plan.sourceBytes,source);QCOMPARE(plan.actions[0].comment.trimmed(),QString("Ignition"));
    QCOMPARE(plan.actions[0].wiring.size(),1);
    QCOMPARE(plan.actions[0].wiring[0].ypsIndex,8);
    QCOMPARE(plan.actions[0].wiring[0].inputs,QVector<int>({2,3}));
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    IntegrationSession session(options);session.m_plan=plan;session.m_hasPlan=true;session.updateBenchMapping();
    const auto mapped=session.mapping().forAction(plan.actions[0]);
    QCOMPARE(mapped.inputs.size(),2);QCOMPARE(mapped.inputs[0].index,66);
    QVERIFY(!mapped.definitionConfirmed); // Аннотация не доказывает разводку.
    QCOMPARE(session.mapping().kp.index,71);
    QTemporaryFile profile;QVERIFY(profile.open());
    profile.write(R"({"schema":"pyro.signal-wiring.v2","version":"conflicting","kp":null,"bindings":[]})");
    profile.flush();
    QVERIFY(!session.loadMapping(profile.fileName()));
    QCOMPARE(session.mapping().kp.index,71);
    QTemporaryDir directory;QVERIFY(directory.isValid());
    session.m_cyclogramBytes=source;session.m_cyclogramSha=plan.sha256;
    const auto reportPath=directory.filePath("annotated.json");
    QVERIFY2(session.saveReport(reportPath,&error),qPrintable(error));
    QFile report(reportPath);QVERIFY(report.open(QIODevice::ReadOnly));
    const auto evidence=QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(QByteArray::fromBase64(evidence["cyclogram_base64"].toString().toLatin1()),source);
    const auto wiring=evidence["actions"].toArray()[0].toObject()["wiring_annotations"].toArray();
    QCOMPARE(wiring.size(),1);QCOMPARE(wiring[0].toObject()["in"].toArray()[0].toInt(),3);
    QVERIFY(!wiring[0].toObject()["wiring_confirmed"].toBool());
    QVERIFY(!session.mapping().hasAction(plan.actions[2]));
    options.boardTransport=IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort="COM9";options.legacySecondStage=true;options.legacyHasKp=false;
    session.m_options=options;session.updateBenchMapping();
    QCOMPARE(session.mapping().forAction(plan.actions[2]).inputs[0].index,1);
    QCOMPARE(session.mapping().kp.index,-1);
    session.m_options.wiringProfilePath="profile.json";session.updateBenchMapping();
    QVERIFY(session.mapping().actions.isEmpty());QVERIFY(!session.mapping().warnings.isEmpty());
    const auto noComment=QByteArray(source).replace("# Ignition ##","##");
    QVERIFY(StandProtocol::parseCyclogram(noComment,&plan,&error));
    QVERIFY(plan.actions[0].comment.isEmpty());QCOMPARE(plan.actions[0].wiring.size(),1);
    const auto multi=QByteArray(source).replace("in=3,4; edge=rise\n",
        "in=3,4; edge=rise ## board=OLD; link=COM9; bkd=A1; yps=none; out=1,2; in=1,2; edge=rise\n");
    QVERIFY2(StandProtocol::parseCyclogram(multi,&plan,&error),qPrintable(error));
    QCOMPARE(plan.actions[0].wiring.size(),2);
    const auto ambiguousKp=QByteArray(source).replace("edge=rise; role=kp\n",
        "edge=rise; role=kp ## board=NEW; link=ETH; bkd=A1; yps=10; out=8; in=8; edge=rise; role=kp\n");
    QVERIFY(!StandProtocol::parseCyclogram(ambiguousKp,&plan,&error));
    const QByteArray dualKp="START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0 ## board=OLD; link=COM1; bkd=A1; yps=none; out=8; in=8; edge=rise; role=kp"
        " ## board=OLD; link=COM2; bkd=A1; yps=none; out=8; in=8; edge=rise; role=kp\n";
    QVERIFY(StandProtocol::parseCyclogram(dualKp,&plan,&error));
    session.m_plan=plan;session.m_options.wiringProfilePath.clear();
    session.m_options.serialPort="COM1";session.m_options.secondLegacySerialPort="COM2";
    session.m_options.legacySecondStage=false;session.m_options.legacyHasKp=true;
    session.updateBenchMapping();QCOMPARE(session.mapping().kp.index,7);
    session.m_boardArmed=true;session.m_hasBoardFrame=true;
    session.m_boardFrame.channels[7]={true,false,1000,0};
    session.m_boardFrame.channels[15]={true,false,99000,0};
    QVERIFY(session.kpTimeUs().has_value());
    QCOMPARE(session.kpTimeUs().value(),quint32(1000));
    session.m_mappingBytes=R"({"schema":"pyro.signal-wiring.v2","version":"restored-json","kp":null,"bindings":[]})";
    session.updateBenchMapping();QVERIFY(session.mapping().actions.isEmpty());
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(),&session.m_plan,&error));
    session.updateBenchMapping();QCOMPARE(session.mapping().version,QString("restored-json"));
}

void IntegrationTest::invalidAnnotations_data() {
    QTest::addColumn<QByteArray>("annotation");
    const QByteArray valid="board=NEW; link=ETH; bkd=A1; yps=9; out=1,2; in=1,2; edge=rise";
    QTest::newRow("missing")<<QByteArray("board=NEW");
    QTest::newRow("unknown")<<(valid+"; foo=1");
    QTest::newRow("duplicate-field")<<(valid+"; yps=9");
    QTest::newRow("zero-yps")<<QByteArray(valid).replace("yps=9","yps=0");
    QTest::newRow("input-range")<<QByteArray(valid).replace("in=1,2","in=1,9");
    QTest::newRow("mismatch")<<QByteArray(valid).replace("in=1,2","in=1");
    QTest::newRow("duplicate-input")<<QByteArray(valid).replace("in=1,2","in=1,1");
    QTest::newRow("edge")<<QByteArray(valid).replace("edge=rise","edge=both");
    QTest::newRow("wrong-mask")<<QByteArray(valid).replace("out=1,2","out=3,4");
    QTest::newRow("wrong-block")<<QByteArray(valid).replace("bkd=A1","bkd=B");
    QTest::newRow("role")<<(valid+"; role=kp");
    QTest::newRow("duplicate-source")<<(valid+" ## "+valid);
}

void IntegrationTest::invalidAnnotations() {
    QFETCH(QByteArray,annotation);CyclogramPlan plan;QString error;
    const auto source=QByteArray("START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=0 # Comment ## ")+annotation+'\n';
    QVERIFY(!StandProtocol::parseCyclogram(source,&plan,&error));QVERIFY(!error.isEmpty());
}


void IntegrationTest::workingTxtProvenanceAndReordering() {
    QFile file(QStringLiteral(STAND_TEST_DATA_DIR "/working_20261014.txt"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    CyclogramPlan plan;
    QString error;
    QVERIFY2(StandProtocol::parseCyclogram(bytes, &plan, &error), qPrintable(error));
    QCOMPARE(plan.sourceBytes, bytes);
    QCOMPARE(plan.sha256, QString("ef57e5c2ec5ed9de3ba443d6005173b24d4b954d29fc7449b5543484dcba459f"));
    QCOMPARE(plan.actions.size(), 33);
    QSet<QString> ids;
    int pulseRequests = 0;
    for (const auto &action : plan.actions) {
        QVERIFY(findCommand(action.key));
        pulseRequests += findCommand(action.key)->mask != 0;
        QVERIFY(!ids.contains(action.actionId)); ids.insert(action.actionId);
        QCOMPARE(action.actionId, plan.sha256 + ':' + QString::number(action.sourceLine));
        QVERIFY(action.rawLine.contains(action.key.toUtf8()));
    }
    QCOMPARE(pulseRequests, 7);
    const auto commented = std::find_if(plan.actions.cbegin(),plan.actions.cend(),[](const auto &a){return a.ordinal==4;});
    QCOMPARE(commented->rawValue, QString("-7980"));
    QCOMPARE(commented->comment, QString::fromUtf8(" Первое срабатывание БКД-А"));
    // Reorder the physical file: mappings still refer to the same logical signals.
    auto lines=bytes.split('\n');
    std::swap(lines[2],lines[4]);
    CyclogramPlan reordered;
    QVERIFY(StandProtocol::parseCyclogram(lines.join('\n'),&reordered,&error));
    const auto a=StandProtocol::benchMapping(9,1,9,plan);
    const auto b=StandProtocol::benchMapping(9,1,9,reordered);
    QCOMPARE(a.actions.keys().size(),7);
    for(auto it=a.actions.cbegin();it!=a.actions.cend();++it) {
        QCOMPARE(it->inputs.size(),b.actions.value(it.key()).inputs.size());
        for(int i=0;i<it->inputs.size();++i) {
            QCOMPARE(it->inputs[i].index,b.actions.value(it.key()).inputs[i].index);
            QVERIFY(!it->inputs[i].signalId.isEmpty());
        }
    }
    QVERIFY(plan.sha256 != reordered.sha256);
    CyclogramPlan blank;
    QVERIFY(StandProtocol::parseCyclogram("\nSTART_UTC_DATE=2026-10-16\n\nSTART_UTC_TIME=12:00:00\nLIFT_OFF_CONTACT=0\n",&blank,&error));
    QCOMPARE(blank.actions.first().sourceLine,5);
    QVERIFY(!StandProtocol::parseCyclogram(bytes+QByteArray(1,'\0'),&blank,&error));
    QVERIFY(!StandProtocol::parseCyclogram(QByteArray("\xef\xbb\xbf")+bytes,&blank,&error));
    QVERIFY(!StandProtocol::parseCyclogram(bytes+QByteArray(8192,' '),&blank,&error));
    QVERIFY(!StandProtocol::parseCyclogram(bytes+QByteArray("#\xff"),&blank,&error));
    StandMapping invalidMapping;
    QVERIFY(!StandProtocol::parseMapping(R"({"version":"bad","kp":{"yps":1.5,"channel":8},"actions":[]})",&invalidMapping,&error));
}

void IntegrationTest::kpTimeVectors_data() {
    QTest::addColumn<quint32>("kp"); QTest::addColumn<quint32>("tick");
    QTest::addColumn<int>("kpPlan"); QTest::addColumn<int>("eventPlan");
    QTest::addColumn<double>("actual"); QTest::addColumn<double>("deviation");
    QTest::newRow("before") << quint32(1000000) << quint32(990000) << 5 << -5 << -10.0 << 0.0;
    QTest::newRow("same") << quint32(1000000) << quint32(1000000) << 5 << 5 << 0.0 << 0.0;
    QTest::newRow("after") << quint32(1000000) << quint32(1020000) << 5 << 25 << 20.0 << 0.0;
    QTest::newRow("outside-tolerance") << quint32(1000000) << quint32(1022000) << 5 << 25 << 22.0 << 2.0;
    QTest::newRow("wrap") << quint32(4294960000U) << quint32(20000) << 0 << 20 << 27.296 << 7.296;
}

void IntegrationTest::kpTimeVectors() {
    QFETCH(quint32,kp);QFETCH(quint32,tick);QFETCH(int,kpPlan);QFETCH(int,eventPlan);
    QFETCH(double,actual);QFETCH(double,deviation);
    IntegrationSession::Options options;options.firstStageYps=1;options.kpYps=1;
    IntegrationSession session(options);
    const QByteArray source="START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\nLIFT_OFF_CONTACT="+
        QByteArray::number(kpPlan)+"\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12="+QByteArray::number(eventPlan)+"\n";
    QString error;QVERIFY(StandProtocol::parseCyclogram(source,&session.m_plan,&error));
    session.updateBenchMapping();session.m_hasPlan=true;session.m_uploadState="CONFIRMED";
    session.m_executionObserved=true;session.m_boardArmed=true;session.m_hasBoardFrame=true;
    const auto action=*std::find_if(session.plan().actions.cbegin(),session.plan().actions.cend(),[](const auto &a){return a.key.startsWith("IGNITE");});
    session.m_boardFrame.channels[0]={true,false,tick,0};
    // Pre-KP observations remain available for retrospective evaluation.
    QCOMPARE(session.measureAction(action).state,ActionMeasurement::State::WaitingForKp);
    session.m_boardFrame.channels[7]={true,false,kp,0};
    auto result=session.measureAction(action);
    QCOMPARE(result.state,ActionMeasurement::State::Partial);
    QCOMPARE(result.channels[0].actualMs.value(),actual);
    QVERIFY(qAbs(result.channels[0].deviationMs.value()-deviation)<1e-9);
    session.m_boardFrame.channels[1]={true,false,tick,0};
    result=session.measureAction(action);QCOMPARE(result.state,ActionMeasurement::State::Measured);
    QCOMPARE(result.plannedMs.value(),double(eventPlan-kpPlan));
    QVERIFY(session.actionResult(action).contains(deviation==0?"в допуске":"вне допуска"));
    session.m_boardFrame.channels[0].riseUs=kp+(quint32(1)<<31);
    QCOMPARE(session.measureAction(action).state,ActionMeasurement::State::AmbiguousInterval);
}

void IntegrationTest::missingReferencesAndRepeatedActions() {
    IntegrationSession::Options options;options.firstStageYps=1;options.kpYps=1;
    IntegrationSession session(options);
    session.m_uploadState="CONFIRMED";session.m_executionObserved=true;
    session.m_boardArmed=true;session.m_hasBoardFrame=true;
    const QByteArray header="START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n";
    const QByteArray ignition="IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=10\n";
    QString error;
    auto load=[&](const QByteArray &data){bool ok=StandProtocol::parseCyclogram(header+data,&session.m_plan,&error);session.updateBenchMapping();return ok;};
    QVERIFY(load(ignition));
    QCOMPARE(session.measureAction(session.plan().actions.first()).state,ActionMeasurement::State::PlanReferenceUnknown);
    QVERIFY(load(ignition+"LIFT_OFF_CONTACT=0\nLIFT_OFF_CONTACT=1\n"));
    QCOMPARE(session.measureAction(session.plan().actions.last()).state,ActionMeasurement::State::PlanReferenceUnknown);
    QVERIFY(load(ignition+ignition+"LIFT_OFF_CONTACT=0\n"));
    QCOMPARE(session.plan().actions.size(),3);
    QVERIFY(session.plan().actions[1].actionId!=session.plan().actions[2].actionId);
    QCOMPARE(session.measureAction(session.plan().actions.last()).state,ActionMeasurement::State::RepeatedAction);
    QVERIFY(load("UNKNOWN_KEY=10\nLIFT_OFF_CONTACT=0\n"));
    QVERIFY(!session.mapping().actions.contains("UNKNOWN_KEY"));
    QCOMPARE(session.measureAction(session.plan().actions.last()).state,ActionMeasurement::State::Unmapped);
    QVERIFY(load(ignition+"FIRE_STARTING_CHAMBER_ENGINES_9_TO_12=20\nLIFT_OFF_CONTACT=0\n"));
    auto &other=session.m_mapping.actions["FIRE_STARTING_CHAMBER_ENGINES_9_TO_12"];
    other.inputs[0]=session.m_mapping.actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12").inputs[0];
    QCOMPARE(session.measureAction(session.plan().actions[1]).state,ActionMeasurement::State::InvalidAssignment);
}

void IntegrationTest::profileChangeInvalidatesEvidence() {
    IntegrationSession session(IntegrationSession::Options{});
    session.m_boardArmed=true;session.m_hasBoardFrame=true;
    session.processFrame(frame(1000000,1020000));
    const auto before=session.collectionId();
    QTemporaryFile profile;QVERIFY(profile.open());
    profile.write(R"({"version":"changed","kp":{"yps":1,"channel":8},"actions":[]})");profile.flush();
    QVERIFY(session.loadMapping(profile.fileName()));
    QVERIFY(session.collectionId()!=before);QVERIFY(!session.boardArmed());QVERIFY(!session.hasBoardFrame());
    QCOMPARE(session.acceptedFrames(),quint64(0));
    IntegrationSession::Options selected;
    selected.wiringProfilePath=profile.fileName();selected.firstStageYps=1;
    IntegrationSession configured(selected);
    QCOMPARE(configured.mapping().version,QString("changed"));
    QVERIFY(configured.mapping().actions.isEmpty());
    selected.wiringProfilePath=profile.fileName()+".missing";
    IntegrationSession missing(selected);
    QVERIFY(missing.mapping().actions.isEmpty());
    QString parseError;QVERIFY(StandProtocol::parseCyclogram(benchCyclogram(),&missing.m_plan,&parseError));
    missing.updateBenchMapping();QVERIFY(missing.mapping().actions.isEmpty());
    QTemporaryDir dir;QString error;const auto path=dir.filePath("report.json");
    QVERIFY(session.saveReport(path,&error));QFile report(path);QVERIFY(report.open(QIODevice::ReadOnly));
    const auto json=QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(json["mapping_json"].toObject()["version"].toString(),QString("changed"));
    QCOMPARE(json["mapping_sha256"].toString().size(),64);
    QCOMPARE(json["catalog_version"].toString(),QString(kCommandCatalogVersion));
    QFile evidence(path+".frames.jsonl");QVERIFY(evidence.open(QIODevice::ReadOnly));QVERIFY(evidence.readAll().isEmpty());
}

void IntegrationTest::signalWiringAcrossGroups() {
    const QByteArray profile=R"({"schema":"pyro.signal-wiring.v2","version":"split-groups-1",
        "kp":{"yps":2,"channel":8},"bindings":[
        {"signal_id":"bkhoa.yps_bkd.0.bit.0","yps":1,"channel":2,"edge":"rise"},
        {"signal_id":"bkhoa.yps_bkd.0.bit.1","yps":3,"channel":4,"edge":"fall"},
        {"signal_id":"bkhoa.yps_bkd.0.bit.7","yps":2,"channel":8,"edge":"rise"}]})";
    StandMapping mapping;QString error;
    QVERIFY2(StandProtocol::parseMapping(profile,&mapping,&error),qPrintable(error));
    const auto action=mapping.actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12");
    QCOMPARE(action.inputs.size(),2);QCOMPARE(action.inputs[0].index,1);QCOMPARE(action.inputs[1].index,19);
    QVERIFY(action.inputs[1].falling);
    const auto incomplete=profile.mid(0); // remove one complete binding, leaving a valid but incomplete profile
    auto json=QJsonDocument::fromJson(incomplete).object();auto bindings=json["bindings"].toArray();
    bindings.removeAt(1);json["bindings"]=bindings;
    QVERIFY(StandProtocol::parseMapping(QJsonDocument(json).toJson(),&mapping,&error));
    QVERIFY(!mapping.actions.contains("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"));
    json["bindings"]=QJsonArray{QJsonObject{{"signal_id","invented.output"},{"yps",1},{"channel",1}}};
    QVERIFY(!StandProtocol::parseMapping(QJsonDocument(json).toJson(),&mapping,&error));
    // A KP field without a matching physical reference signal must never grade.
    json=QJsonDocument::fromJson(profile).object();bindings=json["bindings"].toArray();bindings.removeLast();json["bindings"]=bindings;
    QVERIFY(!StandProtocol::parseMapping(QJsonDocument(json).toJson(),&mapping,&error));
    json=QJsonDocument::fromJson(profile).object();bindings=json["bindings"].toArray();
    auto kpCollision=bindings.last().toObject();kpCollision["yps"]=1;kpCollision["channel"]=2;bindings[2]=kpCollision;
    json["bindings"]=bindings;json["kp"]=QJsonObject{{"yps",1},{"channel",2}};
    QVERIFY(!StandProtocol::parseMapping(QJsonDocument(json).toJson(),&mapping,&error));
    // Legacy positional maps are retained for inspection, but are not definitions.
    const auto legacy=R"({"version":"legacy","kp":{"yps":1,"channel":8},"actions":[{"key":"IGNITE_PYRO_CANDLES_ENGINES_1_TO_8","inputs":[{"yps":1,"channel":1}]}]})";
    QVERIFY(StandProtocol::parseMapping(legacy,&mapping,&error));
    QVERIFY(!mapping.actions.cbegin()->definitionConfirmed);
    const auto partial=R"({"version":"partial","kp":{"yps":1,"channel":8},"actions":[{"key":"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12","inputs":[{"yps":1,"channel":1,"signal_id":"bkhoa.yps_bkd.0.bit.0"}]}]})";
    QVERIFY(StandProtocol::parseMapping(partial,&mapping,&error));
    QVERIFY(!mapping.actions.cbegin()->definitionConfirmed);
    auto fakeKp=QJsonDocument::fromJson(partial).object();fakeKp["kp_action_key"]="START_HELIUM_PURGE_A";
    QVERIFY(!StandProtocol::parseMapping(QJsonDocument(fakeKp).toJson(),&mapping,&error));
    auto incompatible=QJsonDocument::fromJson(profile).object();incompatible["catalog_version"]="unverified-new-catalog";
    QVERIFY(!StandProtocol::parseMapping(QJsonDocument(incompatible).toJson(),&mapping,&error));
}

void IntegrationTest::conflictingFileIdentity() {
    IntegrationSession session(IntegrationSession::Options{});
    auto event=contract("CYCLOGRAM_AVAILABLE",1,"run","stream");event["upload_id"]="same-id";
    auto publish=[&](const QByteArray &bytes){
        event["content_base64"]=QString::fromLatin1(bytes.toBase64());event["byte_count"]=bytes.size();
        event["sha256_hex"]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
        session.processDebug(event);
    };
    publish(cyclogram());QVERIFY(session.hasPlan());const auto hash=session.cyclogramSha256();
    session.m_boardArmed=true;session.m_executionObserved=true;event["seq"]="2";
    publish(cyclogram()+"LIFT_OFF_CONTACT=1\n");
    QVERIFY(!session.boardArmed());QVERIFY(!session.executionPermissionObserved());
    QCOMPARE(session.cyclogramSha256(),hash);QVERIFY(session.lastError().contains("upload_id"));
    auto enabled=contract("BCVM_EXECUTION_ENABLED",3,"run","stream");enabled["upload_id"]="same-id";
    session.processDebug(enabled);QVERIFY(!session.executionPermissionObserved());
    auto upload=contract("CYCLOGRAM_UPLOAD_STATE",4,"run","stream");upload["upload_id"]="same-id";upload["upload_state"]="CONFIRMED";
    session.processDebug(upload);enabled["seq"]="5";session.processDebug(enabled);
    session.m_boardArmed=true; // even a fresh physical collection cannot repair corrupt file identity
    QVERIFY(!session.measurementEligible());
    event["seq"]="6";event["upload_id"]="new-id";publish(cyclogram());
    QVERIFY(session.m_fileIdentityValid);QVERIFY(!session.boardArmed());
    session.m_boardArmed=true;session.m_executionObserved=true;event["seq"]="7";event["upload_id"]="invalid-plan";
    publish("not a valid plan");
    QVERIFY(!session.hasPlan());QVERIFY(!session.boardArmed());QVERIFY(!session.measurementEligible());
    QCOMPARE(session.uploadId(),QString("invalid-plan"));
}

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
        if (action.ordinal == 24) action.key = "FAIRING_SEPARATION";
    const auto mismatch = StandProtocol::benchMapping(4, 9, 4, plan);
    QVERIFY(!mismatch.actions.contains("FAIRING_SEPARATION"));
    QVERIFY(!mismatch.actions.contains("SHUTDOWN_ENGINE_BLOCK_B"));
}

void IntegrationTest::currentOwnerCyclogramMapping() {
    QFile file(QStringLiteral(STAND_TEST_DATA_DIR "/bench_20261014.ini"));
    QVERIFY(file.open(QIODevice::ReadOnly));CyclogramPlan plan;QString error;
    QVERIFY2(StandProtocol::parseCyclogram(file.readAll(),&plan,&error),qPrintable(error));
    const QHash<int,QPair<QString,int>> expected={
      {3,{"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12",-8000}},
      {5,{"FIRE_STARTING_CHAMBER_ENGINES_9_TO_12",-7950}},
      {16,{"CLOSE_MAIN_VALVES_ENGINES_9_TO_12",158000}},
      {20,{"IGNITE_PYRO_CANDLE",159800}},
      {21,{"OPEN_OXIDIZER_AND_FUEL_VALVES",160000}},
      {24,{"SHUTDOWN_ENGINE_BLOCK_B",445000}}};
    for(auto it=expected.cbegin();it!=expected.cend();++it){
      auto action=std::find_if(plan.actions.cbegin(),plan.actions.cend(),[&](const auto &a){return a.ordinal==it.key();});
      QVERIFY(action!=plan.actions.cend());QCOMPARE(action->key,it.value().first);QCOMPARE(action->offsetMs,it.value().second);
    }
    const auto newMapping=StandProtocol::benchMapping(9,0,9,plan);
    QVERIFY(newMapping.warnings.isEmpty());QCOMPARE(newMapping.kp.index,71);
    QCOMPARE(newMapping.actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12").inputs.first().index,64);
    QVERIFY(!newMapping.actions.contains("IGNITE_PYRO_CANDLE"));
    IntegrationSession::Options oldOptions;oldOptions.boardTransport=IntegrationSession::BoardTransport::LegacySerial;oldOptions.legacySecondStage=true;oldOptions.legacyHasKp=false;
    IntegrationSession old(oldOptions);old.m_plan=plan;old.updateBenchMapping();
    QVERIFY(old.mapping().warnings.isEmpty());QVERIFY(old.mapping().kp.index<0);
    QCOMPARE(old.mapping().actions.value("SHUTDOWN_ENGINE_BLOCK_B").inputs.first().index,5);
    QVERIFY(!old.mapping().actions.contains("FAIRING_SEPARATION"));
    QVERIFY(!old.mapping().actions.contains("SHUTDOWN_ENGINE_BLOCK_B_AGAIN"));
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
    const auto repeatedFrame=decoder.append(0x81);
    QCOMPARE(repeatedFrame.channels[0].riseUs, quint32(2000));
    QCOMPARE(repeatedFrame.channels[0].observedRises,quint64(2));
    QCOMPARE(repeatedFrame.channels[7].observedRises,quint64(1));
    decoder.reset();
    QCOMPARE(decoder.sampleCount(), quint64(0));
    QVERIFY(!decoder.append(0).channels[0].triggered);
    QCOMPARE(decoder.append(0).channels[0].observedRises,quint64(0));
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
    // Повтор КП второго COM не отменяет независимую опору первого.
    session.processLegacyBytes(QByteArray::fromHex("0080"),1);
    QVERIFY(session.legacyStreamKpObserved(0));
    QVERIFY(!session.legacyStreamKpObserved(1));
    QCOMPARE(session.measureAction(firstAction).state,ActionMeasurement::State::Measured);
    QCOMPARE(session.measureAction(secondAction).state,ActionMeasurement::State::RepeatedInput);
    QTemporaryDir repeatedKpDir;QVERIFY(repeatedKpDir.isValid());QString repeatedKpError;
    const QString repeatedKpPath=repeatedKpDir.filePath("report.json");
    QVERIFY(session.saveReport(repeatedKpPath,&repeatedKpError));
    QFile repeatedKpReport(repeatedKpPath);QVERIFY(repeatedKpReport.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(repeatedKpReport.readAll()).object().value("second_legacy_kp_time_us").isNull());
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


void IntegrationTest::legacyStreamHealth() {
    IntegrationSession::Options options;
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = QStringLiteral("TEST-A");
    options.secondLegacySerialPort = QStringLiteral("TEST-B");
    IntegrationSession session(options);
    session.m_awaitReset = true;

    session.processLegacyBytes(QByteArray(4, char(0)), 0);
    QVERIFY(session.legacyStreamReceiving(0));
    QVERIFY(!session.legacyStreamReceiving(1));
    QVERIFY(!session.boardReceiving());
    QVERIFY(!session.boardArmed());

    session.processLegacyBytes(QByteArray(4, char(0)), 1);
    QVERIFY(session.legacyStreamReceiving(1));
    QVERIFY(session.boardReceiving());
    QVERIFY(session.boardArmed());

    // Only the second serial feed times out. First feed may still appear
    // healthy, but the combined collection is no longer valid.
    session.m_legacyWatchdog[1].setInterval(15);
    session.m_legacyWatchdog[1].start();
    QTRY_VERIFY(!session.legacyStreamReceiving(1));
    QVERIFY(session.legacyStreamReceiving(0));
    QVERIFY(!session.boardReceiving());
    QVERIFY(!session.boardArmed());
    session.processLegacyBytes(QByteArray(4, char(0)), 1);
    QVERIFY(session.boardReceiving());
    QVERIFY(!session.boardArmed()); // receiving again cannot silently re-arm
}

void IntegrationTest::evidenceIsolation() {
    IntegrationSession::Options options;
    options.boardIp = QHostAddress::LocalHost;
    IntegrationSession session(options);
    auto available = [](const QString &id) {
        const QByteArray file = benchCyclogram();
        QJsonObject message = contract("CYCLOGRAM_AVAILABLE", 1, "run", "stream");
        message.insert("upload_id", id);
        message.insert("byte_count", file.size());
        message.insert("sha256_hex", QString::fromLatin1(
            QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex()));
        message.insert("content_base64", QString::fromLatin1(file.toBase64()));
        return message;
    };
    session.processDebug(available("upload-old"));
    const QString oldCollection = session.m_collectionId;
    QVERIFY(!oldCollection.isEmpty());
    session.processFrame(frame(1000000U, 1020000U));
    QCOMPARE(session.acceptedFrames(), quint64(1));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error;
    const QString oldReport = dir.filePath("old.json");
    QVERIFY2(session.saveReport(oldReport, &error), qPrintable(error));
    QFile oldEvidence(oldReport + ".frames.jsonl");
    QVERIFY(oldEvidence.open(QIODevice::ReadOnly));
    QVERIFY(!oldEvidence.readAll().isEmpty());

    // A new collection starts without carrying forward a previous board
    // frame, even if the KASU upload is unchanged.
    QVERIFY(session.resetBoard()); // sends reset to loopback; no hardware ACK required
    const QString freshCollection = session.m_collectionId;
    QVERIFY(!freshCollection.isEmpty());
    QVERIFY(freshCollection != oldCollection);
    QCOMPARE(session.acceptedFrames(), quint64(0));
    QVERIFY(!session.hasBoardFrame());
    const QString freshReport = dir.filePath("fresh.json");
    QVERIFY2(session.saveReport(freshReport, &error), qPrintable(error));
    QFile freshEvidence(freshReport + ".frames.jsonl");
    QVERIFY(freshEvidence.open(QIODevice::ReadOnly));
    QVERIFY(freshEvidence.readAll().isEmpty());

    session.processFrame(frame(2000000U, 2020000U));
    QCOMPARE(session.acceptedFrames(), quint64(1));
    session.processDebug(available("upload-next"));
    QVERIFY(session.m_collectionId != freshCollection);
    QCOMPARE(session.acceptedFrames(), quint64(0));
    QCOMPARE(session.rejectedFrames(), quint64(0));
    QVERIFY(!session.hasBoardFrame());
    QVERIFY(!session.boardArmed());
    QCOMPARE(session.m_debugEvidence.size(), 1);
    QCOMPARE(session.m_debugEvidence.at(0).toObject().value("upload_id").toString(),
             QStringLiteral("upload-next"));
    const QString nextReport = dir.filePath("next.json");
    QVERIFY2(session.saveReport(nextReport, &error), qPrintable(error));
    QFile nextEvidence(nextReport + ".frames.jsonl");
    QVERIFY(nextEvidence.open(QIODevice::ReadOnly));
    QVERIFY(nextEvidence.readAll().isEmpty());

    // The KASU run boundary is also an evidence boundary.
    session.processFrame(frame(3000000U, 3020000U));
    QCOMPARE(session.acceptedFrames(), quint64(1));
    session.resetDebugState();
    QCOMPARE(session.acceptedFrames(), quint64(0));
    QVERIFY(session.m_debugEvidence.isEmpty());
    QVERIFY(session.m_localEvidence.isEmpty());
    QVERIFY(session.m_collectionId != freshCollection);
    QVERIFY(!session.hasBoardFrame());

    // Ошибка архива именно прогона с загруженным ПЗ не должна скрываться.
    session.m_hasPlan = true;
    session.m_frameEvidence.close();
    session.processFrame(frame(4000000U, 4020000U));
    QString writeError;
    QVERIFY(!session.saveReport(dir.filePath("invalid.json"), &writeError));
    QVERIFY(writeError.contains(QStringLiteral("Журнал доказательств")));
}

void IntegrationTest::resetDiscardsPartialSerialFrame() {
    IntegrationSession::Options options;
    options.boardTransport = IntegrationSession::BoardTransport::Serial;
    options.boardIp = QHostAddress::LocalHost;
    IntegrationSession session(options);
    const QByteArray emptyFrame = frame();
    session.m_serialBuffer = emptyFrame.first(500);
    QVERIFY(session.resetBoard());
    QVERIFY(session.m_serialBuffer.isEmpty());
    session.m_serialBuffer.append(emptyFrame.mid(500));
    session.receiveSerial();
    QVERIFY(!session.boardArmed());
    QCOMPARE(session.acceptedFrames(), quint64(0));
    session.m_serialBuffer.append(emptyFrame);
    session.receiveSerial();
    QVERIFY(session.boardArmed());
    QCOMPARE(session.acceptedFrames(), quint64(1));

    session.m_serialBuffer = emptyFrame.first(500);
    session.resetDebugState();
    QVERIFY(session.m_serialBuffer.isEmpty());
}

void IntegrationTest::failedExportPreservesEvidence() {
    IntegrationSession session(IntegrationSession::Options{});
    session.m_hasPlan = true;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray first = frame(1000000, 1020000);
    const QByteArray second = frame(2000000, 2020000);
    session.processFrame(first);
    QString error;
    QVERIFY(!session.saveReport(dir.filePath("missing/report.json"), &error));
    session.processFrame(second);
    const QString report = dir.filePath("report.json");
    QVERIFY2(session.saveReport(report, &error), qPrintable(error));
    QFile evidence(report + ".frames.jsonl");
    QVERIFY(evidence.open(QIODevice::ReadOnly));
    const auto lines = evidence.readAll().trimmed().split('\n');
    QCOMPARE(lines.size(), 2);
    QCOMPARE(QJsonDocument::fromJson(lines[0]).object().value("frame_base64").toString(),
             QString::fromLatin1(first.toBase64()));
    QCOMPARE(QJsonDocument::fromJson(lines[1]).object().value("frame_base64").toString(),
             QString::fromLatin1(second.toBase64()));
}

void IntegrationTest::boardFrameValidation() {
    BoardFrame parsed;
    QString error;
    QByteArray valid = frame(1000000, 1020000);
    QVERIFY(StandProtocol::parseBoardFrame(valid, &parsed, &error));
    QVERIFY(!StandProtocol::parseBoardFrame(valid.first(962), &parsed, &error));
    QVERIFY(!StandProtocol::parseBoardFrame(valid + valid, &parsed, &error));
    auto badHeader = valid;badHeader[0] = 0;
    QVERIFY(!StandProtocol::parseBoardFrame(badHeader, &parsed, &error));
    QVERIFY(error.contains(QStringLiteral("заголовок")));
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
    QCOMPARE(plan.actions.size(), 2);
    QCOMPARE(plan.actions[1].offsetMs, 20);
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
    const QByteArray map = R"({"version":"bench-1","kp":{"yps":1,"channel":1},"actions":[{"key":"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12","inputs":[{"yps":1,"channel":2,"signal_id":"bkhoa.yps_bkd.0.bit.0"},{"yps":1,"channel":3,"signal_id":"bkhoa.yps_bkd.0.bit.1"}],"tolerance_ms":2}]})";
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
    QVERIFY(session.actionResult(session.plan().actions[1]).contains("в допуске"));
    const ActionMeasurement onTime = session.measureAction(session.plan().actions[1]);
    QCOMPARE(int(onTime.state), int(ActionMeasurement::State::Measured));
    QCOMPARE(onTime.channels.size(), 2);
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
    QVERIFY(report.value("actions").toArray().at(1).toObject().value("result").toString().contains("в допуске"));
    const auto recorded=report["actions"].toArray().at(1).toObject();
    QCOMPARE(recorded["action_id"].toString(), session.cyclogramSha256()+":3");
    QCOMPARE(recorded["planned_from_kp_ms"].toDouble(),20.0);
    QCOMPARE(recorded["channels"].toArray().first().toObject()["raw_tick_us"].toDouble(),1020000.0);
    QVERIFY(!recorded["channels"].toArray().first().toObject()["signal_id"].toString().isEmpty());
    QCOMPARE(report["mapping_sha256"].toString().size(),64);
    QFile evidence(reportPath + ".frames.jsonl");
    QVERIFY(evidence.open(QIODevice::ReadOnly));
    const auto evidenceBytes=evidence.readAll();
    QVERIFY(evidenceBytes.contains(measured.toBase64()));
    QCOMPARE(report["frame_evidence_sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(evidenceBytes,QCryptographicHash::Sha256).toHex()));
    const QByteArray wrapped = frame(4294960000U, 20000U);
    QCOMPARE(board.writeDatagram(wrapped, QHostAddress::LocalHost, telemetryPort), qint64(wrapped.size()));
    // Повторная телеметрия с другими отметками в том же сборе не должна
    // переписать первые зарегистрированные фронты. Wrap проверяется отдельно.
    QTRY_COMPARE(session.liveFrame().channels[0].riseUs, quint32(4294960000U));
    const ActionMeasurement late = session.measureAction(session.plan().actions[1]);
    QCOMPARE(int(late.state), int(ActionMeasurement::State::Measured));
    QCOMPARE(late.channels[0].actualMs.value(), 20.0);
    QCOMPARE(late.channels[0].deviationMs.value(), 0.0);
    QCOMPARE(session.boardFrame().channels[0].riseUs, quint32(1000000));
    QCOMPARE(session.boardFrame().channels[1].riseUs, quint32(1020000));

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
    QVERIFY(session.actionResult(session.plan().actions[1]).contains("не подтверждена"));
    QJsonObject falseEnable = contract("BCVM_EXECUTION_ENABLED", 7, run, stream);
    falseEnable.insert("upload_id", id);
    send(client, falseEnable);
    QCoreApplication::processEvents();
    QVERIFY(!session.executionEnabled());
    QVERIFY(changed.count() > 0);
}


void IntegrationTest::legacyTicksFinishCollection() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE = 2026-10-16\nSTART_UTC_TIME = 12:00:00\n"
        "LIFT_OFF_CONTACT = 0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = 20\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_boardArmed = true;
    session.m_hasBoardFrame = true;
    QCOMPARE(session.m_mapping.kp.index, 7);
    session.m_boardFrame.channels[7].triggered = true;
    session.m_boardFrame.channels[7].riseUs = 1000;
    // Окно заканчивается через 5 с после планового отслеживаемого события.
    for (int i = 0; i < 5030; ++i)
        session.m_legacy[0].append(quint8(0x80));
    session.checkCompletion();
    QVERIFY(session.collectionFinished());
    QVERIFY(session.finishReason().contains(QStringLiteral("тикам старой платы")));
}

void IntegrationTest::liveFrameKeepsReceivingAfterCompletion() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(), &session.m_plan, &error));
    session.m_hasPlan = true;
    BoardFrame first;
    QVERIFY(StandProtocol::parseBoardFrame(frame(1000, 2000), &first, &error));
    session.applyFrame(first);
    QCOMPARE(session.boardFrame().channels[0].riseUs, quint32(1000));
    session.finishCollection();
    QVERIFY(session.collectionFinished());
    BoardFrame second;
    QVERIFY(StandProtocol::parseBoardFrame(frame(3000, 4000), &second, &error));
    session.applyFrame(second);
    QCOMPARE(session.boardFrame().channels[0].riseUs, quint32(1000));
    QCOMPARE(session.liveFrame().channels[0].riseUs, quint32(3000));
}

void IntegrationTest::mismatchedUdpSenderIsCounted() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardIp = QHostAddress(QStringLiteral("192.168.17.144"));
    options.bindIp = QHostAddress::LocalHost;
    options.telemetryPort = 0;
    IntegrationSession session(options);
    session.start();
    QVERIFY(session.m_udp.localPort() != 0);
    QUdpSocket client;
    QCOMPARE(client.writeDatagram(frame(), QHostAddress::LocalHost,
                                  session.m_udp.localPort()), qint64(963));
    QTRY_COMPARE(session.discardedUdpSenders(), quint64(1));
    QCOMPARE(session.acceptedFrames(), quint64(0));
    QVERIFY(session.lastError().contains(QStringLiteral("отброшен")));
}


void IntegrationTest::finishedLegacyCollectIsImmutable() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_uploadState = "CONFIRMED";
    session.m_executionObserved = true;
    session.m_boardArmed = true;
    session.m_legacyArmed[0] = true;
    session.processLegacyBytes(QByteArray(1, char(0)), 0);
    session.processLegacyBytes(QByteArray(1, char(0x80)), 0);  // КП
    session.processLegacyBytes(QByteArray(1, char(0x83)), 0);  // Каналы 1, 2
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::Measured);
    session.finishCollection();
    const auto frozenSamples = session.m_samplesAtFinish[0];
    const auto frozenAccepted = session.m_acceptedAtFinish;
    const auto frozenTick = session.boardFrame().channels[0].riseUs;
    const auto frozenResult = session.actionResult(session.m_plan.actions[1]);

    // Приём не выключается: счетчик live растёт, результаты испытания — нет.
    session.processLegacyBytes(QByteArray(8, char(0)), 0);
    QCOMPARE(session.m_legacy[0].sampleCount(), frozenSamples + quint64(8));
    QCOMPARE(session.boardFrame().channels[0].riseUs, frozenTick);
    QCOMPARE(session.m_acceptedAtFinish, frozenAccepted);
    QCOMPARE(session.actionResult(session.m_plan.actions[1]), frozenResult);

    session.m_legacyWatchdog[0].start(5);
    QTest::qWait(30);
    QVERIFY(session.collectionFinished());
    QVERIFY(session.boardArmed());
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::Measured);
}

void IntegrationTest::missingInputsTurnIntoFailureAfterFinish() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_uploadState = "CONFIRMED";
    session.m_executionObserved = true;
    session.m_boardArmed = true;
    session.m_legacyArmed[0] = true;
    session.processLegacyBytes(QByteArray(1, char(0x80)), 0);
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::WaitingForInputs);
    session.finishCollection(QStringLiteral("Окно завершено"), true);
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::MissingAfterFinish);
    QVERIFY(session.actionResult(session.m_plan.actions[1]).contains(QStringLiteral("Не сработало")));

    // После завершения изменение настроек не должно менять записанный отказ.
    session.m_options.legacyHasKp = false;
    session.updateBenchMapping();
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::MissingAfterFinish);
    // При новом сборе без КП возвращается честный статус несвязанных шкал.
    session.resetCollectionEvidence(false);
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::WaitingForClockAlignment);
}

void IntegrationTest::dualLegacyWaitsForBothCounters() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.secondLegacySerialPort = "TEST_SECOND_COM";
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_boardArmed = true;
    session.m_legacyArmed = {true, true};
    session.processLegacyBytes(QByteArray(5030, char(0x80)), 0);
    session.processLegacyBytes(QByteArray(5, char(0x80)), 1);
    session.checkCompletion();
    QVERIFY(!session.collectionFinished());
    session.processLegacyBytes(QByteArray(5025, char(0x80)), 1);
    session.checkCompletion();
    QVERIFY(session.collectionFinished());
}

void IntegrationTest::manualEarlyFinishNeverClaimsMissedPulse() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=30000\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_uploadState = "CONFIRMED";
    session.m_executionObserved = true;
    session.m_boardArmed = true;
    session.m_legacyArmed[0] = true;
    session.processLegacyBytes(QByteArray(2, char(0x80)), 0);
    session.finishCollection();  // Оператор остановил испытание до 30 секунды.
    QVERIFY(session.collectionFinished());
    QVERIFY(!session.plannedWindowComplete());
    QCOMPARE(session.measureAction(session.m_plan.actions[1]).state,
             ActionMeasurement::State::StoppedEarly);
    QVERIFY(!session.actionResult(session.m_plan.actions[1]).contains(QStringLiteral("Не сработало")));
}


void IntegrationTest::actual33ActionCyclogramHasUnambiguousChannels() {
    // Проверяем не упрощённый тестовый пример, а актуальный файл оператора.
    QFile file(QStringLiteral(STAND_TEST_DATA_DIR "/../../cyclogram.txt"));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const QByteArray bytes = file.readAll();
    QVERIFY(!bytes.contains("SET_UTC_TIME"));
    CyclogramPlan plan;
    QString error;
    QVERIFY2(StandProtocol::parseCyclogram(bytes, &plan, &error), qPrintable(error));
    QCOMPARE(plan.actions.size(), 33);
    QCOMPARE(plan.actions.last().offsetMs, 99750);
    for (const auto &action : plan.actions) {
        QVERIFY2(!action.comment.trimmed().isEmpty(),
                 qPrintable(QStringLiteral("Нет комментария: ") + action.key));
    }
    const auto mapping = StandProtocol::benchMapping(9, 1, 9, plan);
    QCOMPARE(mapping.kp.index, 71);
    QCOMPARE(mapping.actions.size(), 7); // 3 импульса А + 3 импульса Б + КП.
    QSet<int> observedInputs;
    int actualInputs = 0;
    for (auto it = mapping.actions.cbegin(); it != mapping.actions.cend(); ++it) {
        const auto &mapped = it.value();
        QVERIFY(mapped.definitionConfirmed);
        QVERIFY(!mapped.inputs.isEmpty());
        for (const auto &input : mapped.inputs) {
            QVERIFY(input.index >= 0 && input.index < 96);
            QVERIFY2(!observedInputs.contains(input.index),
                     qPrintable(QStringLiteral("Дублируется вход %1 (%2)")
                         .arg(input.index + 1).arg(it.key())));
            observedInputs.insert(input.index);
            ++actualInputs;
        }
    }
    QCOMPARE(actualInputs, 15);
    QCOMPARE(mapping.actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12")
             .inputs.first().index, 64);
    QCOMPARE(mapping.actions.value("FIRE_STARTING_CHAMBER_ENGINES_9_TO_12")
             .inputs.first().index, 66);
    QCOMPARE(mapping.actions.value("CLOSE_MAIN_VALVES_ENGINES_9_TO_12")
             .inputs.first().index, 69);
    QCOMPARE(mapping.actions.value("IGNITE_PYRO_CANDLE").inputs.first().index, 0);
    QCOMPARE(mapping.actions.value("OPEN_OXIDIZER_AND_FUEL_VALVES")
             .inputs.first().index, 2);
    QCOMPARE(mapping.actions.value("SHUTDOWN_ENGINE_BLOCK_B")
             .inputs.first().index, 5);
}


void IntegrationTest::completedActionStateIsFrozenAcrossLateDebugChanges() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    IntegrationSession session(options);
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",
        &session.m_plan, &error));
    session.m_hasPlan = true;
    session.updateBenchMapping();
    session.m_uploadState = QStringLiteral("CONFIRMED");
    session.m_executionObserved = true;
    session.m_boardArmed = true;
    session.m_legacyArmed[0] = true;
    session.processLegacyBytes(QByteArray(1, char(0x80)), 0);
    session.processLegacyBytes(QByteArray(1, char(0x83)), 0);
    const auto action = session.m_plan.actions[1];
    QCOMPARE(session.measureAction(action).state, ActionMeasurement::State::Measured);
    session.finishCollection(QStringLiteral("Завершённое окно"), true);
    const auto frozen = session.actionResult(action);
    const auto frozenAction = session.measureAction(action);

    // БЦВМ могла заблокироваться после испытания; это не отменяет его результаты.
    session.m_uploadState = QStringLiteral("FAILED");
    session.m_executionObserved = false;
    session.m_boardArmed = false;
    session.m_fileIdentityValid = false;
    QCOMPARE(session.measureAction(action).state, frozenAction.state);
    QCOMPARE(session.actionResult(action), frozen);
    QVERIFY(session.m_finalActionMeasurements.contains(action.actionId));
    QCOMPARE(session.m_uploadStateAtFinish, QStringLiteral("CONFIRMED"));
    QVERIFY(session.m_executionObservedAtFinish);

    // Новый сбор обязательно сбрасывает все сохранённые оценки.
    session.resetCollectionEvidence(false);
    QVERIFY(session.m_finalActionMeasurements.isEmpty());
    QVERIFY(!session.collectionFinished());
    QVERIFY(session.actionResult(action) != frozen);
}


void IntegrationTest::transientKpRemainsAfterNextUdpFrame() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    options.firstStageYps = 9;
    options.kpYps = 9;
    IntegrationSession session(options);
    QString error;
    const QByteArray cyclo =
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n"
        "LIFT_OFF_CONTACT=0\n";
    QVERIFY2(StandProtocol::parseCyclogram(cyclo, &session.m_plan, &error), qPrintable(error));
    session.m_hasPlan = true;
    session.m_uploadState = "CONFIRMED";
    session.m_executionObserved = true;
    session.m_boardArmed = true;
    session.updateBenchMapping();
    QCOMPARE(session.m_mapping.kp.index, 71);

    auto onePulse = [](int physicalIndex, quint32 riseUs) {
        QByteArray raw(963, char(0));
        raw[0] = char(0xAA); raw[1] = char(0x55);
        const int at = 2 + physicalIndex * 10;
        raw[at] = 1;
        qToLittleEndian<quint32>(riseUs,
            reinterpret_cast<uchar *>(raw.data() + at + 2));
        quint8 crc = 0;
        for (int i = 0; i < raw.size() - 1; ++i) {
            crc ^= quint8(raw[i]);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x31) : quint8(crc << 1);
        }
        raw[962] = char(crc);
        return raw;
    };
    // КП существует только в первом кадре: второй кадр содержит один новый
    // фронт, но не повторяет прежний. Замена снимка целиком теряла опору.
    session.processFrame(onePulse(71, 1'000'000));
    QVERIFY(session.kpTimeUs().has_value());
    QCOMPARE(*session.kpTimeUs(), quint32(1'000'000));
    session.processFrame(onePulse(64, 1'020'000));
    session.processFrame(onePulse(65, 1'020'000));
    QCOMPARE(session.liveFrame().channels[71].triggered, false);
    QVERIFY(session.boardFrame().channels[71].triggered);
    QCOMPARE(session.boardFrame().channels[71].riseUs, quint32(1'000'000));
    QCOMPARE(*session.kpTimeUs(), quint32(1'000'000));
    const ActionMapping mapped = session.mapping().forAction(session.plan().actions[1]);
    QCOMPARE(mapped.inputs.size(), 2);
    QCOMPARE(mapped.inputs[0].index, 64);
    QCOMPARE(mapped.inputs[1].index, 65);
    QCOMPARE(session.boardFrame().channels[64].riseUs, quint32(1'020'000));
    QCOMPARE(session.boardFrame().channels[65].riseUs, quint32(1'020'000));
    QCOMPARE(session.measureAction(session.plan().actions[1]).state,
             ActionMeasurement::State::Measured);
    QCOMPARE(session.measureAction(session.plan().actions[1]).channels[0].actualMs.value(), 20.0);
    session.processFrame(onePulse(64, 1'050'000));
    QCOMPARE(session.boardFrame().channels[64].riseUs, quint32(1'020'000));
    QCOMPARE(session.liveFrame().channels[64].riseUs, quint32(1'050'000));
}

void IntegrationTest::repeatedLegacyKpIsNotAnAnchor() {
    IntegrationSession::Options options;
    options.boardTransport=IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort="TEST";options.legacyHasKp=true;
    IntegrationSession session(options);session.updateBenchMapping();
    session.m_boardArmed=true;session.m_legacyArmed[0]=true;session.m_executionObserved=true;
    session.processLegacyBytes(QByteArray::fromHex("80"),0);
    QVERIFY(session.kpTimeUs().has_value());
    QVERIFY(session.legacyStreamKpObserved(0));
    session.processLegacyBytes(QByteArray::fromHex("0080"),0);
    QVERIFY(!session.kpTimeUs().has_value());
    QVERIFY(!session.legacyStreamKpObserved(0));
}

void IntegrationTest::passiveObserversWithoutPlanDoNotGrowFlightArchive() {
    IntegrationSession session(IntegrationSession::Options{});
    int validated = 0, masks = 0;
    connect(&session, &IntegrationSession::frameObserved, &session,
        [&](const BoardFrame &, const QByteArray &) { ++validated; });
    connect(&session, &IntegrationSession::legacyBytesObserved, &session,
        [&](const QByteArray &, int) { ++masks; });
    for (int i = 0; i < 20; ++i) {
        session.processFrame(frame());
        session.processLegacyBytes(QByteArray::fromHex("000100"), 0);
    }
    QCOMPARE(session.m_frameEvidence.size(), 0);
    QCOMPARE(validated, 20); QCOMPARE(masks, 20);
    session.processFrame("invalid"); QCOMPARE(validated, 20);
    session.m_collectionFinished = true;
    session.processFrame(frame()); session.processLegacyBytes(QByteArray(1, 0), 0);
    QCOMPARE(validated, 21); QCOMPARE(masks, 21);
    QCOMPARE(session.m_frameEvidence.size(), 0);
}

void IntegrationTest::legacyRepeatedPulseIsNotSingleMeasurement() {
    IntegrationSession::Options options;
    options.boardTransport=IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort="TEST";options.legacyHasKp=true;
    IntegrationSession session(options);QString error;
    QVERIFY(StandProtocol::parseCyclogram(cyclogram(),&session.m_plan,&error));
    session.m_hasPlan=true;session.updateBenchMapping();
    session.m_uploadState="CONFIRMED";session.m_executionObserved=true;
    session.m_boardArmed=true;session.m_legacyArmed[0]=true;
    session.processLegacyBytes(QByteArray::fromHex("80838083"),0);
    const auto action=session.m_plan.actions[1];
    const auto result=session.measureAction(action);
    QVERIFY(result.state!=ActionMeasurement::State::Measured);
    QVERIFY(result.reason.contains(QStringLiteral("Повтор")));
    QCOMPARE(session.boardFrame().channels[0].observedRises,quint64(2));
    session.finishCollection(QStringLiteral("Тест повторов"),true);
    session.processLegacyBytes(QByteArray::fromHex("8083"),0);
    QCOMPARE(session.boardFrame().channels[0].observedRises,quint64(2));
    QCOMPARE(session.measureAction(action).state,result.state);
    QTemporaryDir dir;QVERIFY(dir.isValid());
    QVERIFY(session.saveReport(dir.filePath("report.json"),&error));
    QFile report(dir.filePath("report.json"));QVERIFY(report.open(QIODevice::ReadOnly));
    const auto document=QJsonDocument::fromJson(report.readAll()).object();
    QCOMPARE(document.value("legacy_input_observations").toArray()[0].toObject().value("rise_count").toString(),QStringLiteral("2"));
}

void IntegrationTest::resetDoesNotCarryOldFrameIntoNewRun() {
    IntegrationSession::Options options;options.boardIp=QHostAddress::LocalHost;
    IntegrationSession session(options);
    session.m_awaitReset=true;
    session.m_boardArmed=false;
    // Запоздалый пакет предыдущего сбора пришёл после команды reset.
    session.processFrame(frame(900000,920000));
    QVERIFY(!session.boardArmed());
    QVERIFY(session.liveFrame().channels[0].triggered);
    session.processFrame(frame());
    QVERIFY(session.boardArmed());
    QCOMPARE(session.boardFrame().channels[0].riseUs,quint32(0));
    QCOMPARE(session.boardFrame().channels[1].riseUs,quint32(0));
    // Новый сбор должен принять собственные отметки, а не старые первые фронты.
    session.processFrame(frame(2000000,2020000));
    QCOMPARE(session.boardFrame().channels[0].riseUs,quint32(2000000));
    QCOMPARE(session.boardFrame().channels[1].riseUs,quint32(2020000));
}

QTEST_GUILESS_MAIN(IntegrationTest)
#include "tst_integration.moc"
