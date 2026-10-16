#include "integration.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSerialPortInfo>
#include <QSaveFile>
#include <cstdio>
#include <QMap>
// Passive, opt-in equipment check: no reset, upload, permission or outbound commands.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"legacy-port", "Old tester port", "port", "COM9"});
    parser.addOption({"debug-socket", "KASU Debug socket", "name", "kasupp-debug-events-v1"});
    parser.addOption({"output", "Evidence output", "path", "live-io.json"});
    parser.process(app);
    IntegrationSession::Options options;
    options.debugSocket = parser.value("debug-socket");
    IntegrationSession modern(options);
    options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    options.serialPort = parser.value("legacy-port");
    IntegrationSession legacy(options);
    QJsonArray messages;
    QObject::connect(&modern, &IntegrationSession::message, &app,
                     [&](const QString &s) { messages.append("new: " + s); });
    QObject::connect(&legacy, &IntegrationSession::message, &app,
                     [&](const QString &s) { messages.append("old: " + s); });
    QMap<int,BoardChannel> modernSeen,legacySeen;
    const auto observe=[](IntegrationSession &session,QMap<int,BoardChannel> &seen){
        // Пассивная диагностика читает сырые входы без требования сброса, КП или разрешения БЦВМ.
        if (!session.hasBoardFrame()) return;
        for (int i = 0; i < 96; ++i) {
            const auto &channel = session.liveFrame().channels[size_t(i)];
            if (channel.triggered || channel.ready || channel.riseUs || channel.fallUs)
                seen.insert(i, channel);
        }
    };
    QObject::connect(&modern,&IntegrationSession::changed,&app,[&]{observe(modern,modernSeen);});
    QObject::connect(&legacy,&IntegrationSession::changed,&app,[&]{observe(legacy,legacySeen);});
    modern.start();
    legacy.start();
    QTimer::singleShot(6000, &app, [&] {
        const auto state = [](const IntegrationSession &s) {
            return QJsonObject{
                {"receiving", s.boardReceiving()},
                {"accepted_frames_or_legacy_bytes", QString::number(s.acceptedFrames())},
                {"rejected_frames", QString::number(s.rejectedFrames())},
                {"debug_connected", s.debugConnected()},
                {"has_plan", s.hasPlan()},
                {"upload_id", s.uploadId()},
                {"upload_state", s.uploadState()},
                {"execution_permission_observed", s.executionPermissionObserved()},
                {"armed", s.boardArmed()},
                {"error", s.lastError()}};
        };
        QJsonArray ports;
        for (const auto &p : QSerialPortInfo::availablePorts())
            ports.append(p.portName());
        QJsonObject result{
            {"checked_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"receive_window_ms", 6000},
            {"passive_only", true},
            {"ports", ports},
            {"new", state(modern)},
            {"legacy", state(legacy)},
            {"messages", messages}};
        const auto records=[](const QMap<int,BoardChannel> &seen){QJsonArray active;
            for(auto it=seen.cbegin();it!=seen.cend();++it){const auto &c=it.value();active.append(QJsonObject{{"yps_index",it.key()/8},{"channel",it.key()%8+1},{"triggered",c.triggered},{"ready",c.ready},{"rise_us",QString::number(c.riseUs)},{"fall_us",QString::number(c.fallUs)}});}
            return active;
        };
        result.insert("new_active_records",records(modernSeen));
        result.insert("legacy_active_records",records(legacySeen));
        result.insert("records_observed_over_entire_window",true);
        result.insert("channel_identity_verified",false);
        const auto bytes = QJsonDocument(result).toJson();
        QSaveFile file(parser.value("output"));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            app.exit(2);
            return;
        }
        fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        app.quit();
    });
    return app.exec();
}
