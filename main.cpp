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
    parser.addOption({"wiring-profile", "Профиль сигналов основного источника (необязательно)", "json"});
    parser.addOption({"compare-wiring-profile", "Профиль сигналов дополнительного COM (необязательно)", "json"});
    parser.addOption({"board-ip", "IP платы пиротестера", "ip", "192.168.17.144"});
    parser.addOption({"bind-ip", "Локальный IP приёма UDP", "ip", "0.0.0.0"});
    parser.addOption({"board-port", "UDP-порт телеметрии платы", "port", "5000"});
    parser.addOption({"board-serial", "COM-порт платы вместо UDP", "port"});
    parser.addOption({"tester", "Тип пиротестера: udp, rs485, legacy", "type"});
    parser.addOption({"compare-legacy-serial", "Старый COM одновременно с новой платой; независимая опора КП", "port"});
    parser.addOption({"compare-legacy-no-kp", "КП только на новой плате, в старом потоке отсутствует"});
    parser.addOption({"compare-legacy-has-kp", "КП подключён к старой плате в совместном режиме"});
    parser.addOption({"legacy-no-kp", "КП отсутствует в старом потоке; относительные измерения не подтверждаются"});
    parser.addOption({"first-stage-yps-index", "Физический индекс ЯПС БКД-А новой платы (0..11)", "index"});
    parser.addOption({"second-stage-yps-index", "Физический индекс ЯПС БКД-Б новой платы (0..11)", "index"});
    parser.addOption({"kp-yps-index", "Физический индекс ЯПС с КП новой платы (0..11)", "index"});
    parser.addOption({"compare-legacy-block", "БКД дополнительного старого COM: A1 или B", "block"});
    parser.addOption({"legacy-serial", "Старый пиротестер: 8-битная маска, 1 байт/мс", "port"});
    parser.addOption({"legacy-second-serial", "Второй старый пиротестер БКД-Б (первый COM — А-1)", "port"});
    parser.addOption({"legacy-block", "БКД при одном старом COM: A1 или B", "block"});
    parser.addOption({"first-stage-yps", "ЯПС новой платы, подключённый к БКД-А-1 (0 = не назначен, 1..12)", "number"});
    parser.addOption({"second-stage-yps", "ЯПС новой платы, подключённый к БКД-Б (0 = не назначен, 1..12)", "number"});
    parser.addOption({"kp-yps", "ЯПС новой платы с КП на входе 8 (0 = не назначен, 1..12)", "number"});
    parser.process(a);

    IntegrationSession::Options options;
    QSettings settings(QStringLiteral("PyroStand"), QStringLiteral("Operator"));
    options.wiringProfilePath = parser.isSet("wiring-profile") ? parser.value("wiring-profile")
        : settings.value(QStringLiteral("tester/wiringProfile")).toString();
    options.comparisonWiringProfilePath = parser.isSet("compare-wiring-profile") ? parser.value("compare-wiring-profile")
        : settings.value(QStringLiteral("tester/comparisonWiringProfile")).toString();
    const int savedTransport = settings.value(QStringLiteral("tester/transport"), 0).toInt();
    if (savedTransport >= 0 && savedTransport <= 2)
        options.boardTransport = IntegrationSession::BoardTransport(savedTransport);
    options.comparisonLegacyPort=settings.value("tester/comparisonLegacyPort").toString();
    options.comparisonLegacySecondStage=settings.value("tester/comparisonLegacySecondStage",true).toBool();
    options.comparisonLegacyHasKp=settings.value("tester/comparisonLegacyHasKp",false).toBool();
    options.legacyHasKp=settings.value("tester/legacyHasKp",true).toBool();
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
            if (!ok || *pair.second < 0 || *pair.second > 12) parser.showHelp(2);
        }
        if (*pair.second < 0 || *pair.second > 12) *pair.second = 0;
    }
    if (options.firstStageYps != 0 && options.firstStageYps == options.secondStageYps) parser.showHelp(2);

    for (auto pair : {std::pair{"first-stage-yps-index", &options.firstStageYps}, std::pair{"second-stage-yps-index", &options.secondStageYps}, std::pair{"kp-yps-index", &options.kpYps}}) {
        if (parser.isSet(pair.first)) {bool ok=false;const int index=parser.value(pair.first).toInt(&ok);if(!ok||index<0||index>11)parser.showHelp(2);*pair.second=index+1;}
    }
    if(options.firstStageYps != 0 && options.firstStageYps == options.secondStageYps)parser.showHelp(2);
    if(parser.isSet("compare-legacy-has-kp") && parser.isSet("compare-legacy-no-kp"))parser.showHelp(2);
    if(parser.isSet("compare-legacy-has-kp"))options.comparisonLegacyHasKp=true;
    if(parser.isSet("compare-legacy-no-kp"))options.comparisonLegacyHasKp=false;
    if(parser.isSet("legacy-no-kp"))options.legacyHasKp=false;
    if(parser.isSet("compare-legacy-serial"))options.comparisonLegacyPort=parser.value("compare-legacy-serial");
    if(parser.isSet("compare-legacy-block")){const auto block=parser.value("compare-legacy-block").toUpper();if(block!="A1"&&block!="B")parser.showHelp(2);options.comparisonLegacySecondStage=block=="B";}
    if(options.boardTransport==IntegrationSession::BoardTransport::LegacySerial)options.comparisonLegacyPort.clear();
    if(options.boardTransport==IntegrationSession::BoardTransport::Serial&&!options.comparisonLegacyPort.isEmpty()&&options.comparisonLegacyPort.compare(options.serialPort,Qt::CaseInsensitive)==0)parser.showHelp(2);
    // Smoke-проверка никогда не наследует реальные порты из настроек оператора.
    if(qEnvironmentVariableIsSet("PYRO_STAND_SMOKE_TEST")) {
        options.boardTransport=IntegrationSession::BoardTransport::Udp;
        options.boardIp=QHostAddress::LocalHost;options.bindIp=QHostAddress::LocalHost;
        options.telemetryPort=0;options.debugSocket.clear();
        options.serialPort.clear();options.secondLegacySerialPort.clear();options.comparisonLegacyPort.clear();
        options.wiringProfilePath.clear();options.comparisonWiringProfilePath.clear();
    }
    // Глобальные стили (QSS) – тёмная тема, шрифты, цвета из макета
    a.setStyleSheet(R"(
        QMainWindow, QWidget#operatorSurface {
            background-color: #0d1117;
            color: #e6edf3;
            font-family: 'Inter', 'Segoe UI', sans-serif;
        }
        QLabel, QPushButton, QTableWidget, QTextEdit, QLineEdit {
            font-family: 'Inter', 'Segoe UI', sans-serif;
            color: #e6edf3;
        }
        QPushButton {
            background-color: #0d1117;
            border: 1px solid #30363d;
            border-radius: 4px;
            color: #e6edf3;
            padding: 7px 12px;
            font-size: 12px;
            text-align: left;
        }
        QPushButton:hover:!disabled {
            background-color: #1c2330;
            border-color: #58a6ff;
        }
        QPushButton:disabled {
            color: #484f58;
            background-color: #161b22;
            border-color: #30363d;
        }
        QPushButton#resetBtn {
            color: #f85149;
            border-color: #f85149;
        }
        QPushButton#resetBtn:hover:!disabled {
            background-color: #281a1d;
            border-color: #f85149;
        }
        QTableWidget {
            background-color: #0d1117;
            border: 1px solid #30363d;
            gridline-color: #30363d;
            alternate-background-color: #161b22;
            font-size: 11px;
        }
        QTableWidget::item { padding: 4px 8px; }
        QTableWidget::item:selected {
            background-color: #1c2d3a;
            color: #e6edf3;
        }
        QHeaderView::section {
            background-color: #161b22;
            color: #8b949e;
            border: 0;
            border-bottom: 1px solid #30363d;
            padding: 7px 8px;
            font-size: 11px;
            font-weight: 600;
        }
        QTextEdit {
            background-color: #161b22;
            border: 1px solid #30363d;
            color: #c8d0dc;
            font-size: 11px;
            padding: 4px 8px;
        }
        QToolTip {
            background-color: #161b22;
            color: #e6edf3;
            border: 1px solid #30363d;
        }
    )");

    IntegrationWindow w(options);
    w.show();
    if (qEnvironmentVariableIsSet("PYRO_STAND_SMOKE_TEST"))
        QTimer::singleShot(150, &a, &QCoreApplication::quit);
    return a.exec();
}
