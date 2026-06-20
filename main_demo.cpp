#include <QApplication>
#include <QTemporaryFile>
#include <QTextStream>
#include "mainwindow.h"
#include "stand.h"
#include "types.h"
#include "mocks/mock_serial.h"

// Demo cyclogram: 5-second countdown, 3 tracked events across 6 seconds of flight.
static QString writeDemoCyclogram()
{
    auto f = std::make_unique<QTemporaryFile>();
    f->setAutoRemove(false);
    f->open();
    QTextStream out(f.get());
    out << "SET_UTC_TIME = 10:00:00\n";
    out << "START_UTC_TIME = 10:00:05\n";        // T0 at +5s
    out << "IGNITE_PYRO_CANDLES_ENGINES_9_TO_12 = -2000 # Зажигание пиросвечей 9-12\n";
    out << "LIFT_OFF_CONTACT = 0 # Контакт подъёма\n";
    out << "IGNITE_PYRO_CANDLES_ENGINES_1_TO_8 = 1000 # Зажигание пиросвечей 1-8\n";
    out << "CLOSE_MAIN_VALVES_ENGINES_9_TO_12 = 3000 # Закрытие клапанов 9-12\n";
    f->close();
    return f->fileName();
}

int main(int argc, char *argv[])
{
    qRegisterMetaType<TimerState>();
    qRegisterMetaType<NextEventInfo>();

    QApplication a(argc, argv);
    a.setStyleSheet(R"(
        QMainWindow { background-color: #0a0c10; }
        QLabel, QPushButton, QTableWidget, QTextEdit, QLineEdit {
            font-family: 'JetBrains Mono', 'Courier New', monospace;
            color: #c8d0dc;
        }
        QPushButton {
            background-color: #0f1318; border: 1px solid #1e2530;
            padding: 8px 16px; font-size: 11px; letter-spacing: 0.12em; text-align: left;
        }
        QPushButton:hover:!disabled { background-color: #1a2028; border-color: #00b4d8; }
        QPushButton:disabled { color: #484f58; border-color: #1e2530; }
        QPushButton#stopBtn { border-color: #ef444466; }
        QPushButton#stopBtn:hover:!disabled { border-color: #ef4444; }
        QTableWidget {
            background-color: #0d1117; border: 1px solid #1c2333;
            gridline-color: #1c2333; alternate-background-color: #0a0d13;
        }
        QTableWidget::item { padding: 6px 12px; }
        QHeaderView::section {
            background-color: #0a0d13; color: #484f58; border: none;
            padding: 6px 12px; font-size: 10px; letter-spacing: 0.12em;
        }
        QTextEdit {
            background-color: #0d1117; border: 1px solid #1c2333;
            font-size: 12px; padding: 4px 8px;
        }
        QLineEdit {
            background-color: #0a0d13; border: 1px solid #30363d;
            color: #e3b341; font-size: 14px; padding: 4px 8px;
        }
    )");

    // Demo flight ticks (relative to absoluteIndex=0):
    // T0 = 5000ms, so IGNITE at tick 3000, LIFTOFF at 5000, ENGINES_1_8 at 6000, VALVES at 8000
    static constexpr int64_t T0    = 5000;
    auto *mock = new MockSerial;
    mock->fireAt(T0 - 2000, 0x03); // IGNITE_9_TO_12
    mock->fireAt(T0,        0x80); // LIFT_OFF_CONTACT (sync)
    mock->fireAt(T0 + 1000, 0x1C); // IGNITE_1_TO_8
    mock->fireAt(T0 + 3000, 0x60); // CLOSE_VALVES
    mock->dropAfter(T0 + 5000);    // end stream
    mock->setRealtime(true);        // 1 ms per byte — realistic playback

    auto stand = std::make_unique<Stand>(nullptr, "",
                                         std::unique_ptr<ISerialPort>(mock));

    const QString cyclogramPath = writeDemoCyclogram();

    MainWindow w(std::move(stand), cyclogramPath);
    w.setWindowTitle("pyro_stand  [ДЕМО-РЕЖИМ — реальное железо не подключено]");
    w.show();

    const int ret = a.exec();
    QFile::remove(cyclogramPath);
    return ret;
}
