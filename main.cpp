#include <QApplication>
#include <QCommandLineParser>
#include <QScreen>
#include <QSettings>
#include <QFileInfo>
#include <utility>
#include "integration_window.h"

int main(int argc, char *argv[])
{
    // Дробные DPI (125 %, 150 %) на Windows: передаём коэффициент без округления
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication a(argc, argv);
    a.setApplicationName("pyro_stand");
    // Portable distributions keep their preferences next to the executable.
    if (QFileInfo::exists(QCoreApplication::applicationDirPath() + QStringLiteral("/portable.mode"))) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                          QCoreApplication::applicationDirPath() + QStringLiteral("/settings"));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription("Пиростенд — план КАСУ ПП и измерения платы пиротестера");
    parser.addHelpOption();
    parser.addOption({"debug-socket", "Имя локального сокета Debug КАСУ ПП", "name",
                      "kasupp-debug-events-v1"});
    parser.addOption({"board-ip", "IP платы пиротестера", "ip", "192.168.17.144"});
    parser.addOption({"bind-ip", "Локальный IP приёма UDP", "ip", "0.0.0.0"});
    parser.addOption({"board-port", "UDP-порт телеметрии платы", "port", "5000"});
    parser.addOption({"board-serial", "COM-порт платы вместо UDP", "port"});
    parser.addOption({"tester", "Тип пиротестера: udp, rs485, legacy", "type"});
    parser.addOption({"legacy-serial", "Старый пиротестер: 8-битная маска, 1 байт/мс", "port"});
    parser.addOption({"legacy-second-serial", "Второй старый пиротестер БКД-Б (первый COM — А-1)", "port"});
    parser.addOption({"legacy-block", "БКД при одном старом COM: A1 или B", "block"});
    parser.addOption({"first-stage-yps", "ЯПС новой платы, подключённый к БКД-А-1 (1..12)", "number"});
    parser.addOption({"second-stage-yps", "ЯПС новой платы, подключённый к БКД-Б (1..12)", "number"});
    parser.addOption({"kp-yps", "ЯПС новой платы с КП на входе 8 (1..12)", "number"});
    parser.process(a);

    IntegrationSession::Options options;
    QSettings settings(QStringLiteral("PyroStand"), QStringLiteral("Operator"));
    const int savedTransport = settings.value(QStringLiteral("tester/transport"), 0).toInt();
    if (savedTransport >= 0 && savedTransport <= 2)
        options.boardTransport = IntegrationSession::BoardTransport(savedTransport);
    options.serialPort = settings.value(QStringLiteral("tester/serial")).toString();
    options.secondLegacySerialPort = settings.value(QStringLiteral("tester/secondSerial")).toString();
    options.firstStageYps = settings.value(QStringLiteral("tester/firstYps"), 0).toInt();
    options.secondStageYps = settings.value(QStringLiteral("tester/secondYps"), 0).toInt();
    options.kpYps = settings.value(QStringLiteral("tester/kpYps"), 0).toInt();
    options.legacySecondStage = settings.value(QStringLiteral("tester/legacySecondStage"), false).toBool();
    if (parser.isSet("tester")) {
        const QString type = parser.value("tester");
        if (type == QStringLiteral("udp")) options.boardTransport = IntegrationSession::BoardTransport::Udp;
        else if (type == QStringLiteral("rs485")) options.boardTransport = IntegrationSession::BoardTransport::Serial;
        else if (type == QStringLiteral("legacy")) options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
        else parser.showHelp(2);
    }
    options.debugSocket = parser.value("debug-socket");
    options.boardIp = QHostAddress(parser.isSet("board-ip") ? parser.value("board-ip")
        : settings.value(QStringLiteral("tester/ip"), parser.value("board-ip")).toString());
    options.bindIp = QHostAddress(parser.value("bind-ip"));
    bool portOk = false;
    const uint port = parser.value("board-port").toUInt(&portOk);
    if (options.boardIp.isNull() || options.bindIp.isNull() || !portOk || port == 0 || port > 65535)
        parser.showHelp(2);
    options.telemetryPort = quint16(port);
    if (parser.isSet("board-serial")) {
        options.boardTransport = IntegrationSession::BoardTransport::Serial;
        options.serialPort = parser.value("board-serial");
        if (options.serialPort.isEmpty()) parser.showHelp(2);
    }
    if (parser.isSet("legacy-serial")) {
        if (parser.isSet("board-serial")) parser.showHelp(2);
        options.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
        options.serialPort = parser.value("legacy-serial");
        options.secondLegacySerialPort.clear();
        if (options.serialPort.isEmpty()) parser.showHelp(2);
    }
    if (parser.isSet("legacy-second-serial")) {
        if (options.boardTransport != IntegrationSession::BoardTransport::LegacySerial) parser.showHelp(2);
        options.secondLegacySerialPort = parser.value("legacy-second-serial");
        if (options.secondLegacySerialPort.isEmpty() || options.secondLegacySerialPort.compare(options.serialPort, Qt::CaseInsensitive) == 0)
            parser.showHelp(2);
    }
    if (parser.isSet("legacy-block")) {
        const QString block = parser.value("legacy-block").toUpper();
        if (block != QStringLiteral("A1") && block != QStringLiteral("B")) parser.showHelp(2);
        options.legacySecondStage = block == QStringLiteral("B");
    }
    for (auto pair : {std::pair{"first-stage-yps", &options.firstStageYps},
                      std::pair{"second-stage-yps", &options.secondStageYps},
                      std::pair{"kp-yps", &options.kpYps}}) {
        if (parser.isSet(pair.first)) {
            bool ok = false;
            *pair.second = parser.value(pair.first).toInt(&ok);
            if (!ok || *pair.second < 1 || *pair.second > 12) parser.showHelp(2);
        }
        if (*pair.second < 0 || *pair.second > 12) *pair.second = 0;
    }
    if (options.firstStageYps != 0 && options.firstStageYps == options.secondStageYps) parser.showHelp(2);

    // Глобальные стили (QSS) – тёмная тема, шрифты, цвета из макета
    a.setStyleSheet(R"(
        QMainWindow {
            background-color: #0a0c10;
        }
        QLabel, QPushButton, QTableWidget, QTextEdit, QLineEdit {
            font-family: 'JetBrains Mono', 'Courier New', monospace;
            color: #c8d0dc;
        }
        QPushButton {
            background-color: #0f1318;
            border: 1px solid #1e2530;
            padding: 8px 16px;
            font-size: 13px;
            letter-spacing: 0.12em;
            text-align: left;
        }
        QPushButton:hover:!disabled {
            background-color: #1a2028;
            border-color: #388bfd;
        }
        QPushButton:disabled {
            color: #484f58;
            border-color: #1e2530;
        }
        QPushButton#stopBtn {
            border-color: #ef444466;
        }
        QPushButton#stopBtn:hover:!disabled {
            border-color: #ef4444;
        }
        QTableWidget {
            background-color: #0d1117;
            border: 1px solid #1c2333;
            gridline-color: #1c2333;
            alternate-background-color: #0a0d13;
            font-size: 13px;
        }
        QTableWidget::item {
            padding: 6px 12px;
        }
        QTableWidget::item:selected {
            background-color: #1f2d3d;
            color: #e6edf3;
        }
        QHeaderView::section {
            background-color: #0a0d13;
            color: #6e7681;
            border: none;
            border-bottom: 1px solid #21262d;
            padding: 7px 12px;
            font-size: 12px;
            font-weight: 600;
            letter-spacing: 0.1em;
        }
        QTextEdit {
            background-color: #0d1117;
            border: 1px solid #1c2333;
            font-size: 13px;
            line-height: 1.5;
            padding: 4px 8px;
        }
        QLineEdit {
            background-color: #0a0d13;
            border: 1px solid #30363d;
            color: #e3b341;
            font-size: 14px;
            padding: 4px 8px;
            width: 100px;
        }
    )");

    IntegrationWindow w(options);
    w.show();
    if (qEnvironmentVariableIsSet("PYRO_STAND_SMOKE_TEST"))
        QTimer::singleShot(150, &a, &QCoreApplication::quit);
    return a.exec();
}
