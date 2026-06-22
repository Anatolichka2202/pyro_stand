#include <QApplication>
#include <QCommandLineParser>
#include <QScreen>
#include "mainwindow.h"
#include "types.h"

int main(int argc, char *argv[])
{
    // Дробные DPI (125 %, 150 %) на Windows: передаём коэффициент без округления
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // Register custom types for cross-thread queued signal/slot delivery
    qRegisterMetaType<TimerState>();
    qRegisterMetaType<NextEventInfo>();

    QApplication a(argc, argv);
    a.setApplicationName("pyro_stand");

    QCommandLineParser parser;
    parser.setApplicationDescription("Пиростенд — мониторинг пирособытий при пуске ракеты");
    parser.addHelpOption();
    parser.addOption({{"p", "port"}, "COM-порт (напр. /dev/ttyUSB0, COM7)", "port"});
    parser.addOption({{"l", "log-dir"}, "Папка для лог-файла сессии", "dir"});
    parser.process(a);

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

    MainWindow w(parser.value("port"), parser.value("log-dir"));
    w.show();
    return a.exec();
}
