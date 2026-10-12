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

    IntegrationWindow w(options, parser.value("mapping"));
    w.show();
    return a.exec();
}
