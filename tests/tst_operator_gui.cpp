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
#include <QToolButton>
#include <QScrollBar>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QFileDialog>
#include <QtEndian>

namespace {
void acceptSaveDialog(const QString &path, QObject *context) {
    // Ожидаем видимый диалог, а не предполагаем фиксированное время его создания.
    auto *timer = new QTimer(context);
    QObject::connect(timer, &QTimer::timeout, context, [timer, path] {
        auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        if (!picker || !picker->isVisible()) return;
        timer->stop(); timer->deleteLater();
        auto *name = picker->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
        if (!name) return;
        name->setText(path);
        QMetaObject::invokeMethod(picker, "accept", Qt::QueuedConnection);
    });
    timer->start(20);
}
}

class OperatorGuiTest : public QObject {
    Q_OBJECT
private slots:
    void freeRegistrationRetainsRejectedFrames();
    void freeRegistrationWithoutPlan();
    void startupAndLiveKpCalculation();
    void initTestCase(){
#ifdef Q_OS_WIN
      QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
      QFontDatabase::addApplicationFont("C:/Windows/Fonts/consola.ttf");
#endif
    }
    void fullWorkingPlanVisible();
    void compactLayout_data();
    void compactLayout();
    void tableKeepsWorstChannelDeviation();
    void recentJournalFollowsLastMessage();
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
    void kpOnDifferentYpsIsShownAsMappingDiagnostic();
    void rawChannelsAreVisibleWithoutFlightPermission();
    void timelineShowsFullPlan();
    void peerClockDoesNotTruncateTimeline();
    void finishWithoutKpFreezesVisualUtcClock();
};

void OperatorGuiTest::freeRegistrationRetainsRejectedFrames() {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTemporaryDir archive; QVERIFY(archive.isValid());
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort = 0;
    options.boardIp = options.bindIp = QHostAddress::LocalHost;
    IntegrationWindow window(options); window.show(); bool checked = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            if (auto *yes = message->button(QMessageBox::Yes)) yes->click();
    }); answer.start(20);
    QTimer::singleShot(30, &window, [&] {
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("freeRecordingDialog")); QVERIFY(dialog);
        auto *source = dialog->findChild<QComboBox *>(QStringLiteral("freeSource"));
        source->setCurrentIndex(source->findData(QStringLiteral("main:0")));
        dialog->findChild<QPushButton *>(QStringLiteral("freeStart"))->click();
        auto packet = [](quint32 tick) {
            QByteArray bytes(963, 0); bytes[0] = char(0xaa); bytes[1] = char(0x55);
            bytes[2] = tick ? 1 : 0;
            qToLittleEndian(tick, reinterpret_cast<uchar *>(bytes.data() + 4));
            quint8 crc = 0;
            for (int i = 0; i < 962; ++i) {
                crc ^= quint8(bytes[i]);
                for (int bit = 0; bit < 8; ++bit)
                    crc = crc & 0x80 ? quint8((crc << 1) ^ 0x31) : quint8(crc << 1);
            }
            bytes[962] = char(crc); return bytes;
        };
        const auto valid = packet(1000);
        auto invalid = valid; invalid[962] = char(quint8(invalid[962]) ^ 1);
        window.m_session.processFrame(packet(0)); window.m_session.processFrame(valid);
        window.m_session.processFrame(invalid); window.m_session.processFrame(valid);
        dialog->findChild<QPushButton *>(QStringLiteral("freeStop"))->click();
        acceptSaveDialog(archive.filePath(QStringLiteral("rejected.json")), dialog);
        dialog->findChild<QPushButton *>(QStringLiteral("freeSave"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>(QStringLiteral("freeSave"))->isEnabled(), 2000);
        QFile file(archive.filePath(QStringLiteral("rejected.json"))); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = QJsonDocument::fromJson(file.readAll()).object();
        const auto records = report.value("raw_records").toArray();
        QCOMPARE(records.size(), 4);
        QVERIFY(!records[2].toObject().value("valid").toBool());
        QVERIFY(!records[2].toObject().value("error").toString().isEmpty());
        QCOMPARE(records[2].toObject().value("raw_base64").toString(), QString::fromLatin1(invalid.toBase64()));
        QCOMPARE(report.value("rejected_frames").toInt(), 1);
        QCOMPARE(report.value("edges").toArray().size(), 1);
        checked = true; dialog->reject();
    });
    QTimer::singleShot(3000, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>(QStringLiteral("freeRecordingDialog"))) dialog->reject();
    });
    window.openFreeRecording(); QVERIFY(checked);
}

void OperatorGuiTest::freeRegistrationWithoutPlan() {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QTemporaryDir archive;
    QVERIFY(archive.isValid());
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort = 0;
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = QStringLiteral("COM-test");
    IntegrationWindow window(options); window.show();
    bool checked = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            if (auto *yes = message->button(QMessageBox::Yes)) yes->click();
    });
    answer.start(20);
    QTimer::singleShot(30, &window, [&] {
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("freeRecordingDialog"));
        QVERIFY(dialog);
        auto *kp = dialog->findChild<QComboBox *>(QStringLiteral("freeKp"));
        kp->setCurrentIndex(kp->findData(7));
        auto *start = dialog->findChild<QPushButton *>(QStringLiteral("freeStart"));
        start->click();
        QVERIFY(!window.m_session.hasPlan());
        window.m_session.processLegacyBytes(QByteArray::fromHex("000100808100"), 0);
        QTest::qWait(150);
        auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("freeInputs"));
        QCOMPARE(table->rowCount(), 8);
        QCOMPARE(table->item(0, 4)->text(), QStringLiteral("-2.000"));
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("1.000"));
        window.m_session.resetCollectionEvidence(false);
        QTest::qWait(150);
        auto *status = dialog->findChild<QLabel *>(QStringLiteral("freeStatus"));
        QVERIFY(status->text().contains(QStringLiteral("граница сбора")));
        QVERIFY(!dialog->findChild<QComboBox *>(QStringLiteral("freeSource"))->isEnabled());
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("freeStop"))->isEnabled());
        acceptSaveDialog(archive.filePath(QStringLiteral("free.json")), dialog);
        dialog->findChild<QPushButton *>(QStringLiteral("freeSave"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>(QStringLiteral("freeSave"))->isEnabled(), 2000);
        QFile file(archive.filePath(QStringLiteral("free.json"))); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto report = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(report.value("format").toString(), QStringLiteral("pyro.free-recording.v1"));
        QVERIFY(!report.value("active").toBool());
        QCOMPARE(report.value("kp_tick_us").toString(), QStringLiteral("4000"));
        QCOMPARE(report.value("edges").toArray().size(), 6);
        QCOMPARE(report.value("raw_records").toArray().size(), 1);
        QVERIFY(dialog->grab().save(QStringLiteral("free-recording.png")));
        checked = true; dialog->reject();
    });
    // Страховка от зависания модального окна при неуспешной проверке.
    QTimer::singleShot(3000, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>(QStringLiteral("freeRecordingDialog"))) dialog->reject();
    });
    window.openFreeRecording();
    QVERIFY(checked);
}

void OperatorGuiTest::startupAndLiveKpCalculation() {
    IntegrationSession::Options options; options.debugSocket.clear(); options.telemetryPort=0;
    options.firstStageYps=8; options.secondStageYps=7; options.kpYps=8;
    IntegrationWindow window(options); window.resize(1280,800); window.show();
    auto &session=window.m_session; QString error;
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8 # Зажигание А\n"
        "LIFT_OFF_CONTACT=0 # КП\nIGNITE_PYRO_CANDLE=20 # Зажигание Б\n",
        &session.m_plan,&error));
    session.m_hasPlan=true; session.m_uploadState="CONFIRMED"; session.updateBenchMapping();
    BoardFrame stale; stale.channels[56]={true,false,12,0}; session.applyFrame(stale);
    window.refresh(); window.updatePlannedClock();
    QCOMPARE(window.m_timerLabel->text(),QStringLiteral("—"));
    QVERIFY(window.m_clockSource->text().contains(QStringLiteral("Новый сбор")));
    session.applyFrame(BoardFrame{}); session.m_executionObserved=true; session.m_executionEnabled=true;
    BoardFrame a; a.channels[56]={true,false,992000,0}; a.channels[57]=a.channels[56];
    session.applyFrame(a); window.refresh();
    auto eventRow=[&](const QString &id) {
        for(int row=0;row<window.m_eventTable->rowCount();++row)
            if(window.m_eventTable->item(row,0)->text()==id) return row;
        return -1;
    };
    const int rowA=eventRow("1"), rowB=eventRow("3");
    QVERIFY(rowA>=0 && rowB>=0);
    QCOMPARE(window.m_eventTable->item(rowA,4)->text(),QStringLiteral("—"));
    BoardFrame kp; kp.channels[63]={true,false,1000000,0}; session.applyFrame(kp);
    window.refresh();
    QVERIFY(!session.collectionFinished());
    QCOMPARE(window.m_eventTable->item(rowA,4)->text(),QString("-8.000"));
    QCOMPARE(window.m_eventTable->item(rowA,5)->text(),QString("0.000"));
    BoardFrame b; b.channels[48]={true,false,1020500,0}; b.channels[49]=b.channels[48];
    session.applyFrame(b); window.refresh(); window.updatePlannedClock();
    QVERIFY(!session.collectionFinished());
    QCOMPARE(window.m_eventTable->item(rowB,4)->text(),QString("20.500"));
    QCOMPARE(window.m_eventTable->item(rowB,5)->text(),QString("+0.500"));
    QVERIFY(window.m_clockCaption->text().contains(QStringLiteral("ФИЗИЧЕСКОГО КП")));
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-live-kp.png");
}

void OperatorGuiTest::recentJournalFollowsLastMessage() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    IntegrationWindow window(options);window.show();
    for(int index=0;index<30;++index) window.appendMessage(QStringLiteral("Строка %1").arg(index));
    auto *toggle=window.findChild<QToolButton*>("recentJournalToggle");QVERIFY(toggle);toggle->click();
    auto *recent=window.findChild<QTextEdit*>("recentJournal");QVERIFY(recent);QVERIFY(recent->isVisible());
    QTest::qWait(10);QVERIFY(recent->verticalScrollBar()->maximum()>0);
    QCOMPARE(recent->verticalScrollBar()->value(),recent->verticalScrollBar()->maximum());
    window.appendMessage("LAST_MESSAGE");
    QCOMPARE(recent->verticalScrollBar()->value(),recent->verticalScrollBar()->maximum());
    QCOMPARE(recent->document(),window.m_eventLog->document());
}

void OperatorGuiTest::tableKeepsWorstChannelDeviation() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost;options.firstStageYps=1;options.kpYps=1;
    IntegrationWindow window(options);window.show();auto &session=window.m_session;
    QString error;QVERIFY(StandProtocol::parseCyclogram("START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20\n",&session.m_plan,&error));
    session.m_hasPlan=true;session.m_uploadState="CONFIRMED";session.m_executionObserved=true;
    session.m_boardArmed=true;session.m_hasBoardFrame=true;session.updateBenchMapping();
    session.m_boardFrame.channels[7]={true,false,1000000,0};
    session.m_boardFrame.channels[0]={true,false,1018000,0};
    session.m_boardFrame.channels[1]={true,false,1020000,0};window.refresh();
    auto *table=window.m_eventTable;int group=-1;
    for(int row=0;row<table->rowCount();++row) if(table->item(row,0)->text()=="2") group=row;
    QVERIFY(group>=0);QCOMPARE(table->item(group,4)->text(),QString("20.000"));
    QCOMPARE(table->item(group,5)->text(),QString("0.000"));
    QVERIFY(table->item(group,5)->toolTip().contains("-2.000"));
    QCOMPARE(table->item(group,6)->text(),QStringLiteral("Отклонение"));
    QVERIFY(table->isRowHidden(group+1));
    QTest::mouseClick(table->viewport(),Qt::LeftButton,Qt::NoModifier,table->visualItemRect(table->item(group,1)).center());
    QVERIFY(!table->isRowHidden(group+1));
    session.m_boardFrame.channels[0]={};window.refresh();
    QCOMPARE(table->item(group,6)->text(),QStringLiteral("Не все входы"));
    session.finishCollection();window.refresh();
    QCOMPARE(table->item(group+1,6)->text(),QStringLiteral("не измерено"));
    QVERIFY(table->item(group,6)->foreground().color()!=QColor("#f85149"));
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-partial-early-finish.png");
}

void OperatorGuiTest::compactLayout_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("1100")<<QSize(1100,700);
    QTest::newRow("1280")<<QSize(1280,800);
    QTest::newRow("1920")<<QSize(1920,1080);
}

void OperatorGuiTest::compactLayout() {
    QFETCH(QSize,size);
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.bindIp=QHostAddress::LocalHost;options.boardIp=QHostAddress::LocalHost;
    options.firstStageYps=9;options.secondStageYps=1;options.kpYps=9;
    IntegrationWindow window(options);window.resize(size);window.show();
    QFile file(QFINDTESTDATA("data/working_20261014.txt"));QVERIFY(file.open(QIODevice::ReadOnly));
    QString error;QVERIFY(StandProtocol::parseCyclogram(file.readAll(),&window.m_session.m_plan,&error));
    window.m_session.m_hasPlan=true;window.m_session.updateBenchMapping();window.refresh();
    QTest::qWait(20);
    QVERIFY(window.m_eventTable->isVisible());QCOMPARE(window.size(),size);
    const auto canvasRect=QRect(window.m_canvas->mapTo(&window,QPoint(0,0)),window.m_canvas->size());
    const auto tableRect=QRect(window.m_eventTable->mapTo(&window,QPoint(0,0)),window.m_eventTable->size());
    QVERIFY(canvasRect.left()<tableRect.left());QVERIFY(tableRect.right()<window.width());
    QVERIFY(!window.m_eventTable->isColumnHidden(1));QVERIFY(!window.m_eventTable->isColumnHidden(4));
    QVERIFY(!window.m_eventTable->isColumnHidden(5));QVERIFY(!window.m_eventTable->isColumnHidden(6));
    window.grab().save(QCoreApplication::applicationDirPath()+QString("/operator-compact-%1.png").arg(size.width()));
}



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
        // Отклоняем диалог даже при ошибке проверки: тест не меняет порты и пользовательские настройки.
        dialog->reject();
    });
    window.configureTester();QVERIFY(correctError);
    QCOMPARE(window.m_session.options().boardIp,QHostAddress(QHostAddress::LocalHost));
}

void OperatorGuiTest::historicalPresetClearsExternalProfiles() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.wiringProfilePath="NONEXISTENT_PROFILE_A";options.comparisonWiringProfilePath="NONEXISTENT_PROFILE_B";
    options.boardIp=QHostAddress(QStringLiteral("17.0.0.144")); // Воспроизводит ошибку сохранённого адреса.
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
                port->currentText().isEmpty() && summary->text().contains(QStringLiteral("БКХО-А, ЯПС 8")) &&
                summary->text().contains(QStringLiteral("БКХО-Б на новой плате: ЯПС 7")) &&
                summary->text().contains(QStringLiteral("КП: ЯПС 8, вход 8"));
            dialog->grab().save(QCoreApplication::applicationDirPath()+"/operator-connection-bench.png");
        }
        dialog->reject(); // Тест не применяет настройки и не обращается к оборудованию.
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
    options.firstStageYps=8;options.secondStageYps=7;options.kpYps=8;
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
            if(group.id=="1") {QCOMPARE(row.role,comment);QVERIFY(row.input.startsWith("8:"));++commented;}
            if(group.id=="2") {QCOMPARE(row.role,QStringLiteral("Зажигание пиросвечей"));QVERIFY(row.input.startsWith("7:"));++fallback;}
        }
    }
    QCOMPARE(commented,2);QCOMPARE(fallback,2);
    // Отображение убирает лишние пробелы, не меняя исходный файл и идентичность действия.
    QCOMPARE(session.m_plan.sourceBytes,source);
    QVERIFY(session.m_plan.actions[0].comment.contains(comment));
    // Явная карта файла имеет приоритет: заголовок не должен повторять пресет 8/7.
    QVERIFY(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-19\nSTART_UTC_TIME=12:00:00\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8 # Ignition ## board=NEW; link=ETH; bkd=A1; yps=9; out=1,2; in=1,2; edge=rise\n"
        "LIFT_OFF_CONTACT=0 # KP ## board=NEW; link=ETH; bkd=A1; yps=9; out=8; in=8; edge=rise; role=kp\n",
        &session.m_plan,&error));
    session.updateBenchMapping();window.refresh();
    QVERIFY(session.mapping().occurrenceBased);
    QVERIFY(!window.m_mappingStatus->text().contains(QStringLiteral("ЯПС 8 — БКХО-А")));
}

void OperatorGuiTest::fullWorkingPlanVisible() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.firstStageYps=8;options.secondStageYps=7;options.kpYps=8;
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
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 3);
    QCOMPARE(window.findChildren<QTextEdit *>().size(), 2);
    QCOMPARE(table->columnCount(), 7);
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QStringLiteral("КАНАЛЫ"));
    QCOMPARE(table->horizontalHeaderItem(1)->text(), QStringLiteral("СОБЫТИЕ"));
    QCOMPARE(table->horizontalHeaderItem(4)->text(), QStringLiteral("ОТ КП, МС"));
    QCOMPARE(table->horizontalHeaderItem(5)->text(), QStringLiteral("Δ, МС"));
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
        "CLOSE_MAIN_VALVES_ENGINES_1_TO_8 = 40000\n"; // Команда намеренно оставлена без назначения.
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
    // Неназначенные команды не должны появляться в таблице измерений.
    QTRY_COMPARE(table->rowCount(), 4);
    QStringList names;
    for (int row = 0; row < table->rowCount(); ++row)
        names << table->item(row, 1)->text();
    QVERIFY(names.join(QStringLiteral("|")).contains(
        QStringLiteral("Зажигание пиросвечей двигателей 9–12")));
    QVERIFY(names.join(QStringLiteral("|")).contains(QStringLiteral("Контакт подъёма")));
    QVERIFY(!names.join(QStringLiteral("|")).contains(QStringLiteral("CLOSE_MAIN_VALVES")));
    QCOMPARE(window.findChildren<QTableWidget *>().size(), 3);
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
    QCOMPARE(table->rowCount(), 22);
    QTRY_COMPARE(window.m_canvas->channelCount(), 16);
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-full-plan.png");
    QTRY_VERIFY(window.m_canvas->fits());
    QCOMPARE(window.m_canvas->columnCount(), 1);
    QCOMPARE(window.size(), QSize(1280, 800));
    window.grab().save(QCoreApplication::applicationDirPath()+"/operator-full-plan.png");
    QVERIFY(table->isVisible());

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
    QCOMPARE(window.m_timerLabel->text(), QStringLiteral("≈ +01:01.0"));
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
    kp.id = 1; kp.key = "LIFT_OFF_CONTACT"; kp.time_ms = 0; kp.hasChannels = true;
    EventRow failed;
    failed.id = 2; failed.key = "OTHER"; failed.time_ms = 1; failed.hasChannels = true;
    timeline.setEvents({kp, failed});
    timeline.markEventFired(1, "ok");
    timeline.markEventFired(2, "fail");
    timeline.show();
    QCoreApplication::processEvents();
    const QImage image = timeline.grab().toImage();
    QVERIFY(!image.isNull());
    // Отказ остаётся виден, даже если его узел совпадает на шкале с КП.
    bool visibleFailure = false;
    // Сравнение по доминирующему красному каналу устойчиво к сглаживанию Qt.
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            visibleFailure |= pixel.red() >= 140 &&
                              pixel.red() > pixel.green() * 1.5 &&
                              pixel.red() > pixel.blue() * 1.5;
        }
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
    QStringList raw,facts;for(const auto &group:window.m_canvas->events())for(const auto &row:group.channels)if(row.input=="1" || row.input=="1:1"){raw<<row.rawTick;facts<<row.actual;}
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
    // Случайный бит 8 блока Б не является физическим КП блока А.
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


void OperatorGuiTest::peerClockDoesNotTruncateTimeline() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    options.firstStageYps = 0;
    options.kpYps = 0;
    options.comparisonLegacyPort = "NONEXISTENT_TEST_PEER";
    options.comparisonLegacySecondStage = false;
    options.comparisonLegacyHasKp = true;
    IntegrationWindow window(options);
    window.show();
    QVERIFY(window.m_peer);
    const QByteArray file =
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "START_HELIUM_PURGE_A=-10000\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=-8000\n"
        "LIFT_OFF_CONTACT=0\nPITCH_PROGRAM_BEGIN=5000\n";
    for (auto *session : {&window.m_session, window.m_peer.get()}) {
        QString error;
        QVERIFY(StandProtocol::parseCyclogram(file, &session->m_plan, &error));
        session->m_hasPlan = true;
        session->updateBenchMapping();
    }
    QVERIFY(window.m_session.mapping().actions.isEmpty());
    QVERIFY(!window.m_peer->mapping().actions.isEmpty());
    QCOMPARE(&window.clockSession(), window.m_peer.get());
    window.refresh();
    // Ранее refreshCanvas оставлял только назначенные действия и КП.
    QCOMPARE(window.m_timeline->eventCount(), 4);
    QVERIFY(window.m_timeline->isVisible());
}


void OperatorGuiTest::finishWithoutKpFreezesVisualUtcClock() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    IntegrationWindow window(options);
    window.show();
    QString error;
    const QByteArray input =
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=1000\n";
    QVERIFY2(StandProtocol::parseCyclogram(input, &window.m_session.m_plan, &error),
             qPrintable(error));
    window.m_session.m_hasPlan = true;
    window.m_session.finishCollection();
    window.refresh();
    const QString frozen = window.m_timerLabel->text();
    QVERIFY(window.m_clockSource->text().contains(QStringLiteral("Зафиксировано")));
    QVERIFY(window.m_phaseLabel->text().contains(QStringLiteral("ЗАВЕРШЁН")));
    QTest::qWait(240);
    window.updatePlannedClock();
    QCOMPARE(window.m_timerLabel->text(), frozen);
    QVERIFY(window.m_session.finishedAtUtc().isValid());
}


void OperatorGuiTest::kpOnDifferentYpsIsShownAsMappingDiagnostic() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.telemetryPort = 0;
    options.firstStageYps = 9;
    options.kpYps = 9;
    IntegrationWindow window(options);
    QString error;
    QVERIFY2(StandProtocol::parseCyclogram(
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\n", &window.m_session.m_plan, &error),
        qPrintable(error));
    window.m_session.m_hasPlan = true;
    window.m_session.updateBenchMapping();
    window.m_session.m_hasBoardFrame = true;
    window.m_session.m_boardArmed = true;
    // Сигнал есть на входе 8 ЯПС 8, но карта ## / опции ожидают ЯПС 9.
    window.m_session.m_boardFrame.channels[63] = {true, false, 5'000'000, 0};
    QVERIFY(!window.m_session.kpTimeUs().has_value());
    window.refresh();
    QVERIFY(window.m_kpStatus->text().contains(QStringLiteral("ЯПС 9/8")));
    QVERIFY(window.m_kpStatus->text().contains(QStringLiteral("ЯПС 8/8")));
    QVERIFY(!window.m_session.kpTimeUs().has_value());
    // Сырой фронт на назначенном канале тоже должен быть виден оператору
    // даже тогда, когда разрешение испытания ещё не подтверждено.
    window.m_session.m_boardFrame.channels[71] = {true, false, 5'020'000, 0};
    window.refresh();
    QVERIFY(window.m_kpStatus->text().contains(QStringLiteral("Сырой фронт КП")));
    QVERIFY(window.m_kpStatus->text().contains(QStringLiteral("не подтверждена загрузка")));
}

QTEST_MAIN(OperatorGuiTest)
#include "tst_operator_gui.moc"
