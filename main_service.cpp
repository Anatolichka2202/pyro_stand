#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QDir>
#include <cstdio>
#include "stand.h"
#include "session_logger.h"
#include "types.h"

static void emit_json(const QJsonObject &obj)
{
    fprintf(stdout, "%s\n",
            QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    qRegisterMetaType<TimerState>();
    qRegisterMetaType<NextEventInfo>();
    qRegisterMetaType<QVector<EventRow>>();

    QCoreApplication app(argc, argv);
    app.setApplicationName("pyro_service");
    app.setApplicationVersion("0.1");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption cyclogramOpt({"c", "cyclogram"},
                                    "Path to cyclogram.ini", "path");
    QCommandLineOption logDirOpt({"l", "log-dir"},
                                 "Directory for session log file (default: current dir)",
                                 "dir",
                                 QDir::currentPath());
    QCommandLineOption portOpt({"p", "port"},
                               "Serial port device (e.g. /dev/ttyUSB0, COM7)",
                               "port",
                               DEFAULT_SERIAL_PORT);
    parser.addOption(cyclogramOpt);
    parser.addOption(logDirOpt);
    parser.addOption(portOpt);
    parser.process(app);

    const QString cyclogramPath = parser.value(cyclogramOpt);
    const QString logDir        = parser.value(logDirOpt);
    const QString portName      = parser.value(portOpt);

    // Session logger — mandatory for telemetry/aerospace stands
    SessionLogger logger(logDir);
    if (!logger.isOpen()) {
        fprintf(stderr, "WARNING: could not open log file in '%s'\n",
                logDir.toUtf8().constData());
    }
    emit_json({{"type", "log"}, {"level", "INFO"},
               {"msg", QString("Log file: %1").arg(logger.filePath())}});

    Stand stand(nullptr, portName, nullptr, &logger);

    QObject::connect(&stand, &Stand::logMessage,
                     [](const QString &msg, const QString &type) {
        emit_json({{"type", "log"}, {"level", type}, {"msg", msg}});
    });

    QObject::connect(&stand, &Stand::timerTick,
                     [](const TimerState &s) {
        emit_json({
            {"type",        "timer"},
            {"ms_to_start", (qint64)s.msToStart},
            {"phase",       (int)s.phase}
        });
    });

    QObject::connect(&stand, &Stand::nextEventChanged,
                     [](const NextEventInfo &info) {
        QJsonObject obj{{"type", "next_event"}, {"event_id", info.eventId}};
        if (info.msRemaining != INT64_MAX)
            obj["ms_remaining"] = (qint64)info.msRemaining;
        if (!info.description.isEmpty())
            obj["description"] = info.description;
        emit_json(obj);
    });

    QObject::connect(&stand, &Stand::eventFired,
                     [](int id, int tick) {
        emit_json({{"type", "event_fired"}, {"event_id", id}, {"tick", tick}});
    });

    QObject::connect(&stand, &Stand::analysisDone,
                     [&app](const QVector<EventRow> &events) {
        QJsonArray arr;
        for (const auto &ev : events) {
            if (!ev.hasChannels) continue;
            arr.append(QJsonObject{
                {"id",            ev.id},
                {"key",           ev.key},
                {"description",   ev.description},
                {"time_ms",       ev.time_ms},
                {"status",        ev.status},
                {"fired_tick",    ev.firedTick},
                {"calculated_ms", ev.calculatedMs},
                {"deviation_ms",  ev.deviationMs}
            });
        }
        emit_json({{"type", "analysis"}, {"events", arr}});
        app.quit();
    });

    QObject::connect(&stand, &Stand::portError,
                     [&app](const QString &msg) {
        emit_json({{"type", "error"}, {"msg", msg}});
        app.exit(1);
    });

    if (!stand.loadCyclogram(cyclogramPath)) {
        emit_json({{"type", "error"}, {"msg", "Не удалось загрузить циклограмму"}});
        return 1;
    }

    QTimer::singleShot(0, &stand, &Stand::sendToBoard);

    return app.exec();
}
