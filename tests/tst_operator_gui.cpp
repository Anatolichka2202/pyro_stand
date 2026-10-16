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
#include <QFontDatabase>
#include <QUuid>
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>

class OperatorGuiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){
#ifdef Q_OS_WIN
      QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
      QFontDatabase::addApplicationFont("C:/Windows/Fonts/consola.ttf");
#endif
    }
    void fullWorkingPlanVisible();
    void channelLabelsUseSourceComments();
    void nonzeroPlannedKpUsesOneScale();
    void historicalPresetClearsExternalProfiles();
    void invalidIpShowsSpecificError_data();
    void invalidIpShowsSpecificError();
    void showsMappedEventsNotRawTesterChannels();
    void showsDualLegacyComStatuses();
    void fullPlanKeepsTableUsableAndHardwareClockStable();
    void kpClusterDoesNotHideFailure();
    void combinedKeepsCountersSeparate();
    void commonKpOnlyOnNewBoard();
    void rawChannelsAreVisibleWithoutFlightPermission();
    void timelineShowsFullPlan();
};



void OperatorGuiTest::invalidIpShowsSpecificError_data() {
    QTest::addColumn<QString>("invalidIp");
    QTest::newRow("trailing-comma") << QStringLiteral("192.168.17.144,");
    QTest::newRow("abbreviated-address") << QStringLiteral("17.144");
}

void OperatorGuiTest::invalidIpShowsSpecificError() {
    QFETCH(QString, invalidIp);
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost;
    IntegrationWindow window(options);
    bool correctError=false;
    QTimer::singleShot(0,&window,[&] {
        auto *dialog=window.findChild<QDialog*>("testerConfigurationDialog");
        if(!dialog) return;
        auto *ip=dialog->findChild<QLineEdit*>("testerBoardIp");
        auto *buttons=dialog->findChild<QDialogButtonBox*>();
        if(ip && buttons) {
            ip->setText(invalidIp);
            buttons->button(QDialogButtonBox::Ok)->click();
            auto *error=dialog->findChild<QLabel*>("testerConfigurationError");
            correctError=dialog->isVisible() && error && error->isVisible() &&
                error->text().contains(invalidIp) && error->text().contains(QStringLiteral("Некорректный IP")) &&
                !error->text().contains(QStringLiteral("разные группы ЯПС"));
            dialog->grab().save(QCoreApplication::applicationDirPath()+"/operator-invalid-ip.png");
        }
        // Force rejection even if validation regresses: never apply ports or change user settings.
        dialog->reject();
    });
    window.configureTester();QVERIFY(correctError);
    QCOMPARE(window.m_session.options().boardIp,QHostAddress(QHostAddress::LocalHost));
}

void OperatorGuiTest::historicalPresetClearsExternalProfiles() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.wiringProfilePath="NONEXISTENT_PROFILE_A";options.comparisonWiringProfilePath="NONEXISTENT_PROFILE_B";
    options.boardIp=QHostAddress(QStringLiteral("17.0.0.144")); // reproduces the saved-address error
    IntegrationWindow window(options);
    bool cleared=false, collapsed=false, correctScheme=false;
    QTimer::singleShot(0,&window,[&] {
        auto *dialog=window.findChild<QDialog*>("testerConfigurationDialog");
        if (!dialog) return;
        auto *preset=dialog->findChild<QPushButton*>("historicalPresetBtn");
        auto *primary=dialog->findChild<QLineEdit*>("primaryWiringProfile");
        auto *secondary=dialog->findChild<QLineEdit*>("comparisonWiringProfile");
        auto *advanced=dialog->findChild<QWidget*>("advancedConnections");
        collapsed=advanced && advanced->isHidden();
        if(preset && primary && secondary) {
            preset->click();cleared=primary->text().isEmpty() && secondary->text().isEmpty();
            const auto *summary=dialog->findChild<QLabel*>("testerConfigurationSummary");
            const auto *transport=dialog->findChild<QComboBox*>("testerTransportChoice");
            const auto *port=dialog->findChild<QComboBox*>("comparisonLegacyPortChoice");
            const auto *ip=dialog->findChild<QLineEdit*>("testerBoardIp");
            correctScheme=summary && transport && port &&
                ip && ip->text()=="192.168.17.144" && summary->text().contains("192.168.17.144") &&
                transport->currentData().toInt()==int(IntegrationSession::BoardTransport::Udp) &&
                port->currentText()=="COM9" && summary->text().contains(QStringLiteral("БКД-А, ЯПС 8")) &&
                summary->text().contains(QStringLiteral("БКД-Б · одна ЯПС · без КП"));
            dialog->grab().save(QCoreApplication::applicationDirPath()+"/operator-connection-bench.png");
        }
        dialog->reject(); // Never apply COM9/YPS or access physical equipment in this test.
    });
    window.configureTester();QVERIFY(cleared);QVERIFY(collapsed);QVERIFY(correctScheme);
    QCOMPARE(window.m_session.options().wiringProfilePath,QString("NONEXISTENT_PROFILE_A"));
}

void OperatorGuiTest::nonzeroPlannedKpUsesOneScale() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;options.firstStageYps=1;options.kpYps=1;
    IntegrationWindow window(options);window.show();auto &session=window.m_session;
    QString error;QVERIFY(StandProtocol::parseCyclogram("START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\nLIFT_OFF_CONTACT=5\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=25\n",&session.m_plan,&error));
    session.m_hasPlan=true;session.m_uploadState="CONFIRMED";session.m_executionObserved=true;
    session.m_boardArmed=true;session.m_hasBoardFrame=true;session.updateBenchMapping();
    session.m_boardFrame.channels[7]={true,false,1000000,0};
    session.m_boardFrame.channels[0]={true,false,1020000,0};session.m_boardFrame.channels[1]={true,false,1020000,0};
    window.refresh();
    bool found=false;
    for(const auto &group:window.m_canvas->events()) if(group.id=="2") {
        found=true;QCOMPARE(group.offsetMs,qint64(20));QCOMPARE(group.plan,QString::fromUtf8("ПЛАН 20"));
        for(const auto &row:group.channels) {QCOMPARE(row.actual,QString("20.000"));QCOMPARE(row.delta,QString("0.000"));}
    }
    QVERIFY(found);QCOMPARE(window.m_eventTable->item(1,3)->text(),QString("20"));
}

void OperatorGuiTest::channelLabelsUseSourceComments() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost;options.bindIp=QHostAddress::LocalHost;
    options.firstStageYps=9;options.secondStageYps=1;options.kpYps=9;
    IntegrationWindow window(options);auto &session=window.m_session;QString error;
    const auto comment=QStringLiteral("Зажигание по комментарию ПЗ");
    const auto source=QStringLiteral("START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8000 #   %1  \n"
        "IGNITE_PYRO_CANDLE=159800\nLIFT_OFF_CONTACT=0\n").arg(comment).toUtf8();
    QVERIFY(StandProtocol::parseCyclogram(source,&session.m_plan,&error));
    session.m_hasPlan=true;session.updateBenchMapping();window.refresh();
    int commented=0,fallback=0;
    for(const auto &group:window.m_canvas->events()) {
        for(const auto &row:group.channels) {
            QVERIFY(!row.role.contains(".yps_bkd."));
            if(group.id=="1") {QCOMPARE(row.role,comment);++commented;}
            if(group.id=="2") {QCOMPARE(row.role,QStringLiteral("Зажигание пиросвечей"));++fallback;}
        }
    }
    QCOMPARE(commented,2);QCOMPARE(fallback,2);
    // Presentation trims whitespace without modifying the source or matching identity.
    QCOMPARE(session.m_plan.sourceBytes,source);
    QVERIFY(session.m_plan.actions[0].comment.contains(comment));
}

void OperatorGuiTest::fullWorkingPlanVisible() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.firstStageYps=9;options.kpYps=9;
    IntegrationWindow window(options);window.show();
    QFile input(QFINDTESTDATA("data/working_20261014.txt"));QVERIFY(input.open(QIODevice::ReadOnly));
    const auto bytes=input.readAll();auto &session=window.m_session;QString error;
    QVERIFY(StandProtocol::parseCyclogram(bytes,&session.m_plan,&error));
    session.m_hasPlan=true;session.updateBenchMapping();window.refresh();
    auto *button=window.findChild<QPushButton*>("fullPlanBtn");QVERIFY(button);QTest::mouseClick(button,Qt::LeftButton);
    QVERIFY(window.m_planTable->isVisible());QCOMPARE(window.m_planTable->rowCount(),33);
    QSet<QString> ids;
    for(int row=0;row<33;++row) {
        auto *id=window.m_planTable->item(row,7);QVERIFY(id);QVERIFY(!ids.contains(id->text()));ids.insert(id->text());
    }
    QVERIFY(window.m_canvas->fits());
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-working-plan.png");
    window.m_planTable->window()->grab().save(QCoreApplication::applicationDirPath()+"/operator-complete-task.png");
}

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
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 2);
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
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 2);
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
        {24, "SHUTDOWN_ENGINE_BLOCK_B"}};
    for (int i = 1; i <= 24; ++i)
        file += keys.value(i, "OTHER_" + QByteArray::number(i)) + " = " + QByteArray::number(i == 7 ? 0 : i) + '\n';
    QJsonObject event{{"contract", "kasu.pp.debug.stand.v1"}, {"type", "CYCLOGRAM_AVAILABLE"},
        {"seq", "1"}, {"run_id", "run"}, {"stream_id", "stream"}, {"upload_id", "upload"},
        {"byte_count", file.size()}, {"sha256_hex", QString::fromLatin1(
            QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex())},
        {"content_base64", QString::fromLatin1(file.toBase64())}};
    window.m_session.processDebug(event);
    window.refresh();
    auto *table = window.m_eventTable;
    QCOMPARE(table->rowCount(), 21);
    QTRY_COMPARE(window.m_canvas->channelCount(), 16);
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-full-plan.png");
    QTRY_VERIFY(window.m_canvas->fits());
    QCOMPARE(window.m_canvas->columnCount(), 2);
    QCOMPARE(window.size(), QSize(1280, 800));
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-full-plan.png");
    QVERIFY(!table->isVisible());

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
    QCOMPARE(window.m_timerLabel->text(), QStringLiteral("+01:01.0"));
    QVERIFY(window.m_timerLabel->styleSheet().contains("28px"));
    const auto hardwarePosition=window.m_session.measuredPositionMs();
    QTest::qWait(30);
    window.updatePlannedClock();
    QCOMPARE(window.m_session.measuredPositionMs(), hardwarePosition);
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

void OperatorGuiTest::combinedKeepsCountersSeparate(){
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;options.firstStageYps=1;options.kpYps=1;options.comparisonLegacyPort="NONEXISTENT_TEST_COMPARE";options.comparisonLegacySecondStage=false;options.comparisonLegacyHasKp=true;
    IntegrationWindow window(options);QVERIFY(window.m_peer);window.resize(1600,900);window.show();
    const QByteArray file="START_UTC_DATE = 2026-10-14\nSTART_UTC_TIME = 12:00:00\nOTHER1=0\nOTHER2=1\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-20\nLIFT_OFF_CONTACT=0\n";
    for(auto *session:{&window.m_session,window.m_peer.get()}){
      QString error;QVERIFY(StandProtocol::parseCyclogram(file,&session->m_plan,&error));session->m_hasPlan=true;session->m_uploadState="CONFIRMED";session->m_executionObserved=true;session->m_boardArmed=true;session->m_hasBoardFrame=true;session->updateBenchMapping();
    }
    auto &newFrame=window.m_session.m_boardFrame;newFrame.channels[7]={true,false,500000,0};newFrame.channels[0]={true,false,480000,0};newFrame.channels[1]={true,false,481000,0};
    auto &old=*window.m_peer;old.m_legacyArmed[0]=true;old.m_boardFrame.channels[7]={true,false,100000,0};old.m_boardFrame.channels[0]={true,false,80000,0};old.m_boardFrame.channels[1]={true,false,81000,0};
    window.refresh();QCOMPARE(window.m_canvas->channelCount(),6);QTRY_VERIFY(window.m_canvas->fits());
    QStringList raw,facts;for(const auto &group:window.m_canvas->events())for(const auto &row:group.channels)if(row.input=="1" || row.input=="0:1"){raw<<row.rawTick;facts<<row.actual;}
    QCOMPARE(raw,QStringList({"480000","80000"}));QCOMPARE(facts,QStringList({"-20.000","-20.000"}));
    old.m_boardFrame.channels[7]={};window.refresh();
    for(const auto &group:window.m_canvas->events())if(group.source.contains("NONEXISTENT_TEST_COMPARE"))for(const auto &row:group.channels)QVERIFY(row.status!=QStringLiteral("в допуске"));
    window.m_session.m_mapping = {};
    window.refresh();
    QCOMPARE(&window.clockSession(), window.m_peer.get());
    QVERIFY(!window.m_timeline->isHidden());
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-combined.png");
}

void OperatorGuiTest::commonKpOnlyOnNewBoard() {
    IntegrationSession::Options options;
    options.debugSocket.clear();options.telemetryPort=0;
    options.firstStageYps=9;options.kpYps=9;
    options.comparisonLegacyPort="NONEXISTENT_B";
    options.comparisonLegacySecondStage=true;
    options.comparisonLegacyHasKp=false;
    IntegrationWindow window(options);window.show();
    QByteArray file="START_UTC_DATE = 2026-10-14\nSTART_UTC_TIME = 12:00:00\n";
    const QHash<int,QByteArray> keys={{3,"IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"},{5,"FIRE_STARTING_CHAMBER_ENGINES_9_TO_12"},{16,"CLOSE_MAIN_VALVES_ENGINES_9_TO_12"},{20,"IGNITE_PYRO_CANDLE"},{21,"OPEN_OXIDIZER_AND_FUEL_VALVES"},{24,"SHUTDOWN_ENGINE_BLOCK_B"},{7,"LIFT_OFF_CONTACT"}};
    for(int i=1;i<=24;++i)file+=keys.value(i,"OTHER_"+QByteArray::number(i))+"="+QByteArray::number(i*10)+'\n';
    for(auto *session:{&window.m_session,window.m_peer.get()}) {
        QString error;QVERIFY(StandProtocol::parseCyclogram(file,&session->m_plan,&error));
        session->m_hasPlan=true;session->m_uploadState="CONFIRMED";session->m_executionObserved=true;
        session->m_boardArmed=true;session->m_hasBoardFrame=true;session->updateBenchMapping();
    }
    window.m_session.m_boardFrame.channels[71]={true,false,500000,0};
    QCOMPARE(window.m_session.mapping().kp.index,71);
    QCOMPARE(window.m_session.mapping().actions.value("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12").inputs.first().index,64);
    auto &old=*window.m_peer;old.m_legacyArmed[0]=true;
    old.m_boardFrame.channels[0]={true,false,1000000,0};old.m_boardFrame.channels[1]={true,false,1001000,0};
    // An incidental bit 8 in B must never be mistaken for the physical KP on A.
    old.m_boardFrame.channels[7]={true,false,999000,0};
    QVERIFY(!old.kpTimeUs());QVERIFY(!old.legacyStreamKpObserved(0));
    const auto action=std::find_if(old.plan().actions.cbegin(),old.plan().actions.cend(),[](const auto &a){return a.ordinal==20;});
    QVERIFY(action!=old.plan().actions.cend());
    QCOMPARE(old.measureAction(*action).state,ActionMeasurement::State::WaitingForClockAlignment);
    window.refresh();QCOMPARE(window.m_canvas->channelCount(),15);
    int kpRows=0;for(const auto &g:window.m_canvas->events()) {
        if(g.id==QStringLiteral("КП"))++kpRows;
        if(g.source.contains("NONEXISTENT_B"))for(const auto &row:g.channels) {
            QVERIFY(row.status!=QStringLiteral("в допуске"));
            if(row.input=="1"&&g.id=="20")QVERIFY(row.actual.startsWith(QStringLiteral("≈")));
        }
    }
    QCOMPARE(kpRows,1);QTRY_VERIFY(window.m_canvas->fits());
}


void OperatorGuiTest::rawChannelsAreVisibleWithoutFlightPermission() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    options.firstStageYps = 1;
    IntegrationWindow window(options);
    window.show();
    auto *button = window.findChild<QPushButton *>("liveChannelsBtn");
    QVERIFY(button);
    QTest::mouseClick(button, Qt::LeftButton);
    auto *table = window.findChild<QTableWidget *>("liveChannelsTable");
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 8);
    // Без КАСУПП и КП факт фронта всё равно показывается в отдельном окне.
    window.m_session.m_liveFrame.channels[0].triggered = true;
    window.m_session.m_liveFrame.channels[0].riseUs = 1234000;
    window.refreshLive();
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("ДА"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("1234.000"));
    QVERIFY(!window.m_session.measurementEligible());
    window.m_liveDialog->close();
}

void OperatorGuiTest::timelineShowsFullPlan() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    options.firstStageYps = 1;
    options.kpYps = 1;
    IntegrationWindow window(options);
    QFile file(QStringLiteral(STAND_TEST_DATA_DIR "/working_20261014.txt"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString error;
    QVERIFY(StandProtocol::parseCyclogram(file.readAll(), &window.m_session.m_plan, &error));
    window.m_session.m_hasPlan = true;
    window.m_session.updateBenchMapping();
    window.refresh();
    QCOMPARE(window.m_timeline->eventCount(), 33);
    window.m_session.finishCollection();
    window.refresh();
    QVERIFY(window.m_phaseLabel->text().contains(QStringLiteral("ЗАВЕРШЁН")));
}

QTEST_MAIN(OperatorGuiTest)
#include "tst_operator_gui.moc"
