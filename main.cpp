#include <QApplication>
#include <QCommandLineParser>
#include <QScreen>
#include "integration_window.h"

int main(int argc, char *argv[])
{
    // Дробные DPI (125 %, 150 %) на Windows: передаём коэффициент без округления
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication a(argc, argv);
    a.setApplicationName("pyro_stand");

    QCommandLineParser parser;
    parser.setApplicationDescription("Пиростенд — план КАСУ ПП и измерения платы пиротестера");
    parser.addHelpOption();
    parser.addOption({"debug-socket", "Имя локального сокета Debug КАСУ ПП", "name",
                      "kasupp-debug-events-v1"});
    parser.addOption({"board-ip", "IP платы пиротестера", "ip", "192.168.17.144"});
    parser.addOption({"bind-ip", "Локальный IP приёма UDP", "ip", "0.0.0.0"});
    parser.addOption({"board-port", "UDP-порт телеметрии платы", "port", "5000"});
    parser.addOption({"board-serial", "COM-порт платы вместо UDP", "port"});
    parser.addOption({"mapping", "Утверждённая карта каналов JSON", "path"});
    parser.process(a);

    IntegrationSession::Options options;
    options.debugSocket = parser.value("debug-socket");
    options.boardIp = QHostAddress(parser.value("board-ip"));
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

    IntegrationWindow w(options, parser.value("mapping"));
    w.show();
    return a.exec();
}
