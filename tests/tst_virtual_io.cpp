#include "integration.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtEndian>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

namespace {
QByteArray packet(quint32 kp = 0, quint32 pulse = 0) {
    QByteArray raw(963, '\0');
    raw[0] = char(0xAA); raw[1] = char(0x55);
    const auto stamp = [&](int index, quint32 value) {
        if (!value) return;
        const int offset = 2 + 10 * index;
        raw[offset] = 1;
        qToLittleEndian<quint32>(value, reinterpret_cast<uchar *>(raw.data() + offset + 2));
    };
    stamp(7, kp); stamp(0, pulse); stamp(1, pulse);
    quint8 crc = 0;
    for (int i = 0; i < 962; ++i) {
        crc ^= quint8(raw[i]);
        for (int b = 0; b < 8; ++b)
            crc = crc & 0x80 ? quint8((crc << 1) ^ 0x31) : quint8(crc << 1);
    }
    raw[962] = char(crc);
    return raw;
}

QJsonObject debugEvent(const char *kind, int seq) {
    return {{"contract", "kasu.pp.debug.stand.v1"}, {"type", kind},
            {"seq", QString::number(seq)}, {"run_id", "virtual-run"},
            {"stream_id", "virtual-stream"}, {"upload_id", "virtual-upload"}};
}
void sendDebug(QLocalSocket *socket, const QJsonObject &object) {
    const auto line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    QCOMPARE(socket->write(line), qint64(line.size()));
    QVERIFY(socket->flush());
}
}

class VirtualIoTest : public QObject {
    Q_OBJECT
private slots:
    void udpAndKasuDebugToReport();
    void udpCorruptionAndLoss();
    void realQtSerialOnLinuxPty();
    void sustainedUdpKeepsEventLoopAndArchive();
    void queuedUdpDrainSurvivesReconfigure();
};

void VirtualIoTest::queuedUdpDrainSurvivesReconfigure() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost;options.bindIp=QHostAddress::LocalHost;
    IntegrationSession stand(options);stand.start();QUdpSocket simulator;
    QVERIFY(simulator.bind(QHostAddress::LocalHost,0));const auto raw=packet();
    for(int i=0;i<200;++i)
        QCOMPARE(simulator.writeDatagram(raw,QHostAddress::LocalHost,stand.m_udp.localPort()),qint64(raw.size()));
    stand.receiveUdp();QCOMPARE(stand.acceptedFrames(),quint64(64));
    QVERIFY(stand.m_udpDrainScheduled);
    stand.configureBoard(options);
    const auto accepted=stand.acceptedFrames();
    QCoreApplication::processEvents();QCOMPARE(stand.acceptedFrames(),accepted);
    QCOMPARE(simulator.writeDatagram(raw,QHostAddress::LocalHost,stand.m_udp.localPort()),qint64(raw.size()));
    QTRY_COMPARE(stand.acceptedFrames(),accepted+1);
}

void VirtualIoTest::sustainedUdpKeepsEventLoopAndArchive() {
    IntegrationSession::Options options;options.debugSocket.clear();options.telemetryPort=0;
    options.boardIp=QHostAddress::LocalHost;options.bindIp=QHostAddress::LocalHost;
    options.firstStageYps=1;options.kpYps=1;
    IntegrationSession stand(options);stand.start();
    QString parseError;
    QVERIFY(StandProtocol::parseCyclogram("START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0\nIGNITE_PYRO_CANDLES_ENGINES_9_TO_12=600000\n",&stand.m_plan,&parseError));
    stand.m_hasPlan=true;stand.updateBenchMapping();
    stand.m_uploadState="CONFIRMED";stand.m_executionObserved=true;stand.m_boardArmed=true;
    QUdpSocket simulator;QVERIFY(simulator.bind(QHostAddress::LocalHost,0));
    QElapsedTimer clock,lastBeat;clock.start();lastBeat.start();qint64 maxGap=0;int beats=0;
    QTimer heartbeat;heartbeat.setInterval(5);
    connect(&heartbeat,&QTimer::timeout,this,[&]{maxGap=std::max(maxGap,lastBeat.restart());++beats;});
    heartbeat.start();
    const int seconds=std::clamp(qEnvironmentVariableIntValue("PYRO_STAND_SOAK_SECONDS"),2,300);
    const auto raw=packet(1000000,1020000);quint64 sent=0;
    while(clock.elapsed()<seconds*1000) {
        for(int i=0;i<20;++i) {
            QCOMPARE(simulator.writeDatagram(raw,QHostAddress::LocalHost,stand.m_udp.localPort()),qint64(raw.size()));++sent;
        }
        QTest::qWait(20);
    }
    QTRY_COMPARE(stand.acceptedFrames(),sent);QCOMPARE(stand.rejectedFrames(),quint64(0));
    QVERIFY(beats>seconds*20);QVERIFY2(maxGap<500,qPrintable(QString::number(maxGap)));
    heartbeat.stop();
    stand.finishCollection();QVERIFY(stand.collectionFinished());
    QTemporaryDir directory;QVERIFY(directory.isValid());QString error;
    const auto path=directory.filePath("soak.json");QVERIFY(stand.saveReport(path,&error));
    QFile archived(path+".frames.jsonl");QVERIFY(archived.open(QIODevice::ReadOnly));
    const auto before=QCryptographicHash::hash(archived.readAll(),QCryptographicHash::Sha256);
    const auto size=stand.m_frameEvidence.size();const auto frozen=stand.boardFrame().channels[0].riseUs;
    QCOMPARE(frozen,quint32(1020000));
    QCOMPARE(stand.measureAction(stand.plan().actions[1]).state,ActionMeasurement::State::Measured);
    const auto late=packet(2000000,2040000);
    for(int i=0;i<100;++i) {
        simulator.writeDatagram(late,QHostAddress::LocalHost,stand.m_udp.localPort());
        if(i%10==0) QCoreApplication::processEvents();
    }
    QTRY_COMPARE(stand.acceptedFrames(),sent+100);
    QCOMPARE(stand.boardFrame().channels[0].riseUs,frozen);
    QCOMPARE(stand.liveFrame().channels[0].riseUs,quint32(2040000));
    QCOMPARE(stand.m_frameEvidence.size(),size);
    archived.seek(0);QCOMPARE(QCryptographicHash::hash(archived.readAll(),QCryptographicHash::Sha256),before);
    qInfo("SOAK seconds=%d packets=%llu heartbeat=%d max_gap_ms=%lld evidence_bytes=%lld",
          seconds,static_cast<unsigned long long>(sent),beats,static_cast<long long>(maxGap),static_cast<long long>(size));
}

void VirtualIoTest::udpAndKasuDebugToReport() {
    const QString socketName = "virtual-pyro-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    QLocalServer debug;
    QVERIFY(debug.listen(socketName));
    QUdpSocket simulator;
    QVERIFY(simulator.bind(QHostAddress::LocalHost, 0));
    IntegrationSession::Options options;
    options.debugSocket = socketName;
    options.boardIp = QHostAddress::LocalHost;
    options.bindIp = QHostAddress::LocalHost;
    options.telemetryPort = 0;
    options.commandPort = simulator.localPort();
    options.firstStageYps = 1; options.kpYps = 1;
    IntegrationSession stand(options);
    stand.start();
    QVERIFY(stand.m_udp.localPort() != 0);
    QTRY_VERIFY(debug.hasPendingConnections());
    QLocalSocket *debugClient = debug.nextPendingConnection();
    QVERIFY(debugClient);
    const QByteArray file =
        "START_UTC_DATE=2026-10-16\nSTART_UTC_TIME=12:00:00\n"
        "LIFT_OFF_CONTACT=0 # КП\n"
        "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12=20 # Тестовый импульс\n";
    QJsonObject plan = debugEvent("CYCLOGRAM_AVAILABLE", 1);
    plan["byte_count"] = file.size();
    plan["sha256_hex"] = QString::fromLatin1(
        QCryptographicHash::hash(file, QCryptographicHash::Sha256).toHex());
    plan["content_base64"] = QString::fromLatin1(file.toBase64());
    sendDebug(debugClient, plan);
    QTRY_VERIFY(stand.hasPlan());
    QJsonObject upload = debugEvent("CYCLOGRAM_UPLOAD_STATE", 2);
    upload["upload_state"] = "CONFIRMED";
    sendDebug(debugClient, upload);
    QTRY_COMPARE(stand.uploadState(), QStringLiteral("CONFIRMED"));
    sendDebug(debugClient, debugEvent("BCVM_EXECUTION_ENABLED", 3));
    QTRY_VERIFY(stand.executionPermissionObserved());

    // Сброс уходит только на локальный UDP-сокет эмулятора, не на реальную плату.
    QVERIFY(stand.resetBoard());
    QTRY_VERIFY(simulator.hasPendingDatagrams());
    QByteArray reset(int(simulator.pendingDatagramSize()), '\0');
    QCOMPARE(simulator.readDatagram(reset.data(), reset.size()), qint64(reset.size()));
    QCOMPARE(reset, StandProtocol::resetCommand());
    const quint16 port = stand.m_udp.localPort();
    const QByteArray zero = packet();
    QCOMPARE(simulator.writeDatagram(zero, QHostAddress::LocalHost, port), qint64(zero.size()));
    QTRY_VERIFY(stand.boardArmed());
    const QByteArray pulse = packet(1'000'000, 1'020'000);
    QCOMPARE(simulator.writeDatagram(pulse, QHostAddress::LocalHost, port), qint64(pulse.size()));
    QTRY_COMPARE(stand.acceptedFrames(), quint64(2));
    QCOMPARE(stand.kpTimeUs().value(), quint32(1'000'000));
    const auto measured = stand.measureAction(stand.plan().actions[1]);
    QCOMPARE(measured.state, ActionMeasurement::State::Measured);
    QCOMPARE(measured.channels.size(), 2);
    QCOMPARE(measured.channels[0].actualMs.value(), 20.0);
    QCOMPARE(measured.channels[0].deviationMs.value(), 0.0);

    stand.finishCollection(QStringLiteral("Виртуальная приёмка"), true);
    const auto later = packet(2'000'000, 2'100'000);
    QCOMPARE(simulator.writeDatagram(later, QHostAddress::LocalHost, port),
             qint64(later.size()));
    QTRY_COMPARE(stand.acceptedFrames(), quint64(3));
    QCOMPARE(stand.boardFrame().channels[0].riseUs, quint32(1'020'000));
    QCOMPARE(stand.liveFrame().channels[0].riseUs, quint32(2'100'000));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error;
    const auto path = dir.filePath("report.json");
    QVERIFY2(stand.saveReport(path, &error), qPrintable(error));
    QFile report(path);
    QVERIFY(report.open(QIODevice::ReadOnly));
    const auto data = QJsonDocument::fromJson(report.readAll()).object();
    QVERIFY(data["collection_finished"].toBool());
    QCOMPARE(data["accepted_frames"].toString(), QStringLiteral("2"));
    QCOMPARE(data["actions"].toArray()[1].toObject()["measurement_state"].toInt(),
             int(ActionMeasurement::State::Measured));
    QFile trace(path + ".frames.jsonl");
    QVERIFY(trace.open(QIODevice::ReadOnly));
    const auto bytes = trace.readAll();
    QVERIFY(bytes.contains(pulse.toBase64()));
    QVERIFY(!bytes.contains(later.toBase64()));
}

void VirtualIoTest::udpCorruptionAndLoss() {
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardIp = QHostAddress::LocalHost;
    options.bindIp = QHostAddress::LocalHost;
    options.telemetryPort = 0;
    IntegrationSession stand(options);
    stand.start();
    QUdpSocket simulator;
    QVERIFY(simulator.bind(QHostAddress::LocalHost, 0));
    const QByteArray valid = packet();
    auto bad = valid;
    bad[24] = char(quint8(bad[24]) ^ 1);
    // 90 из 100 UDP-кадров: без seq нельзя установить потерю в реальной телеметрии.
    for (int i = 0; i < 100; ++i) {
        if (i % 10 == 0) continue;
        QCOMPARE(simulator.writeDatagram(valid, QHostAddress::LocalHost,
                                         stand.m_udp.localPort()), qint64(valid.size()));
    }
    QCOMPARE(simulator.writeDatagram(bad, QHostAddress::LocalHost, stand.m_udp.localPort()),
             qint64(bad.size()));
    QTRY_COMPARE(stand.acceptedFrames(), quint64(90));
    QTRY_COMPARE(stand.rejectedFrames(), quint64(1));
    QVERIFY(stand.lastError().contains("CRC8"));
}

void VirtualIoTest::realQtSerialOnLinuxPty() {
#ifdef Q_OS_UNIX
    // Проверка QSerialPort и байтовых тиков через реальное псевдоустройство ОС.
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    QVERIFY(master >= 0);
    QVERIFY(grantpt(master) == 0);
    QVERIFY(unlockpt(master) == 0);
    const char *slave = ptsname(master);
    QVERIFY(slave != nullptr);
    IntegrationSession::Options options;
    options.debugSocket.clear();
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = QString::fromLocal8Bit(slave);
    IntegrationSession stand(options);
    stand.start();
    QVERIFY2(stand.m_serial.isOpen(), qPrintable(stand.m_serial.errorString()));
    const char masks[] = {0, char(0x80), char(0x83), char(0x83), char(0x80), 0};
    QCOMPARE(write(master, masks, sizeof(masks)), ssize_t(sizeof(masks)));
    QTRY_COMPARE(stand.acceptedFrames(), quint64(6));
    QCOMPARE(stand.liveFrame().channels[7].riseUs, quint32(2000));
    QCOMPARE(stand.liveFrame().channels[0].riseUs, quint32(3000));
    QCOMPARE(stand.liveFrame().channels[0].fallUs, quint32(5000));
    QVERIFY(!stand.liveFrame().channels[0].ready);
    close(master);
#else
    QSKIP("Для последовательного порта используется PTY Linux/POSIX");
#endif
}

QTEST_GUILESS_MAIN(VirtualIoTest)
#include "tst_virtual_io.moc"
