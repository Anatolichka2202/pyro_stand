#include "mainwindow.h"
#include "real_serial_port.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
#include <atomic>

// Повторяет только чтение эталона; ни запись в COM, ни сетевое управление не нужны.
class ReplayPort final : public ISerialPort {
public:
    QByteArray bytes = QByteArray(10, '\0') + QByteArray(10, '\x03') + QByteArray(10, '\x1c') + QByteArray(2000, '\0');
    std::atomic<bool> consumed{false};
    bool open(const QString &, int baud) override { opened = baud == 115200; return opened; }
    void close() override { opened = false; }
    bool isOpen() const override { return opened; }
    bool waitForReadyRead(int) override {
        if (!consumed) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return false;
    }
    qint64 read(char *data, qint64 size) override {
        if (consumed) return 0;
        const auto count = qMin(size, qint64(bytes.size()));
        memcpy(data, bytes.constData(), size_t(count));
        consumed = true;
        return count;
    }
    bool hasError() const override { return false; }
    QString errorString() const override { return {}; }
    void clearBuffers() override {}
private:
    bool opened = false;
};

int selfTest(QApplication &app, const QString &fixture)
{
    QTemporaryDir directory;
    const QString path = directory.filePath("capture.bin");
    QUdpSocket listener;
    if (!listener.bind(QHostAddress::AnyIPv4, 4000)) return 2;
    auto port = std::make_unique<ReplayPort>();
    auto *replay = port.get();
    Stand stand(nullptr, {}, std::move(port));
    stand.setCapturePath(path);
    if (!stand.loadCyclogram(fixture)) return 3;
    int analyses = 0, completions = 0, errors = 0, heartbeats = 0;
    QObject::connect(&stand, &Stand::logMessage, &app, [&](const QString &message, const QString &) {
        if (message.startsWith("COM:")) ++heartbeats;
    });
    QObject::connect(&stand, &Stand::analysisDone, &app, [&] { ++analyses; });
    QObject::connect(&stand, &Stand::flightComplete, &app, [&] { ++completions; });
    QObject::connect(&stand, &Stand::portError, &app, [&](const QString &error) { ++errors; qWarning().noquote() << error; });
    stand.sendToBoard();
    stand.startReadingForTest();
    QTimer::singleShot(150, &app, [&] { stand.stop(); app.quit(); });
    app.exec();
    app.processEvents();
    QFile raw(path);
    if (!raw.open(QIODevice::ReadOnly) || raw.readAll() != replay->bytes) { qWarning() << path << raw.size() << replay->consumed.load(); return 4; }
    if (analyses || completions || errors || heartbeats != 2 || stand.getSyncIndex() != -1 || listener.hasPendingDatagrams()) return 5;
    int observed = 0;
    for (const auto &event : stand.getEvents()) {
        if (event.firedTick == 10 || event.firedTick == 20) ++observed;
        if (event.status == "fail" || event.status == "late") return 6;
    }
    if (observed != 2) return 7;
    // Повторное открытие существующего архива должно завершиться ошибкой, не затерев байты.
    stand.startReadingForTest();
    QTimer::singleShot(150, &app, [&] { stand.stop(); app.quit(); });
    app.exec();
    app.processEvents();
    raw.close();
    if (errors != 1 || !raw.open(QIODevice::ReadOnly) || raw.readAll() != replay->bytes) return 8;
    // Загруженный план не должен порождать зелёный итог допуска в летнем GUI.
    auto guiStand = std::make_unique<Stand>(nullptr, QString(), std::make_unique<ReplayPort>());
    MainWindow window(std::move(guiStand), fixture);
    window.show();
    app.processEvents();
    auto *summary = window.findChild<QLabel *>("summaryLabel");
    if (!summary || summary->isVisible()) return 9;
    return 0;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    qRegisterMetaType<TimerState>();
    qRegisterMetaType<NextEventInfo>();
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"self-test", "Программная проверка без оборудования"});
    parser.addOption({"port", "COM-порт", "port", "COM9"});
    parser.addOption({"capture", "Новый файл сырых байтов", "path"});
    parser.addOption({"screenshot", "Новый снимок окна после пяти секунд приёма", "path"});
    parser.addOption({"seconds", "Локально завершить приложение через 1–180 секунд", "seconds", "180"});
    parser.process(app);
    const QString fixture = QCoreApplication::applicationDirPath() + "/reference/cyclogram.ini";
    if (parser.isSet("self-test")) return selfTest(app, fixture);
    bool ok = false;
    const int seconds = parser.value("seconds").toInt(&ok);
    if (!ok || seconds < 1 || seconds > 180) return 2;
    const auto startedUtc = QDateTime::currentDateTimeUtc();
    const QString capturePath = parser.isSet("capture") ? parser.value("capture") :
        QCoreApplication::applicationDirPath() + "/COM9-" + startedUtc.toString("yyyyMMdd-HHmmss-zzz") + ".bin";
    if (QFile::exists(capturePath)) return 4;
    QFile log(capturePath + ".log");
    if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 3;
    auto stand = std::make_unique<Stand>(nullptr, parser.value("port"));
    stand->setCapturePath(capturePath);
    auto *receiver = stand.get();
    MainWindow window(std::move(stand), fixture);
    for (auto *label : window.findChildren<QLabel *>())
        if (label->text() == "COM") label->setText(parser.value("port"));
    window.setWindowTitle("Летний Пиростенд bf13333 — только приём COM, без оценки");
    app.setStyleSheet("QMainWindow,QWidget {background:#10171d;color:#c8d0dc;} QPushButton,QTableWidget,QTextEdit {background:#17232d;color:#c8d0dc;}");
    for (const char *name : {"loadBtn", "resetBtn", "setTimeBtn", "exportCsvBtn"})
        if (auto *button = window.findChild<QPushButton *>(name)) button->hide();
    auto *stop = window.findChild<QPushButton *>("stopBtn");
    if (stop) { stop->setText("Остановить приём локально"); stop->setEnabled(true); }
    if (auto *caption = window.findChild<QLabel *>("timerCaption"))
        caption->setText("Только COM · 115200 8N1 · без синхронизации UTC и оценки допуска");
    if (auto *phase = window.findChild<QLabel *>("phaseLabel")) phase->setText("ПАССИВНЫЙ ПРИЁМ COM");
    // В этой проверке важны сырые тики; плановые столбцы и дельта неприменимы.
    if (auto *table = window.findChild<QTableWidget *>()) {
        table->hideColumn(3);
        table->hideColumn(5);
        table->horizontalHeaderItem(4)->setText("ТИК ПО БАЙТАМ");
    }
    QObject::connect(receiver, &Stand::logMessage, &window, [&, stop](const QString &message, const QString &) {
        log.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " UTC PC | " + message + "\n").toUtf8());
        log.flush();
        if (message.startsWith("COM:"))
            if (auto *phase = window.findChild<QLabel *>("phaseLabel")) phase->setText(message);
        if (message.startsWith("Приём завершён") || message.startsWith("Приём остановлен")) {
            if (stop) stop->setEnabled(false);
            if (auto *phase = window.findChild<QLabel *>("phaseLabel")) phase->setText("ПРИЁМ ОСТАНОВЛЕН · ОЦЕНКИ НЕТ");
        }
    });
    bool captureError = false;
    QObject::connect(receiver, &Stand::portError, &window, [&](const QString &message) {
        captureError = true;
        log.write(("ERROR: " + message + "\n").toUtf8());
        log.flush();
    });
    window.show();
    QTimer::singleShot(0, &window, [receiver] { receiver->startReadingForTest(); });
    if (parser.isSet("screenshot")) {
        QTimer::singleShot(5000, &window, [&] {
            const auto path = parser.value("screenshot");
            if (!QFile::exists(path)) window.grab().save(path);
        });
    }
    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    const int result = app.exec();
    receiver->stop();
    app.processEvents();
    if (captureError) return 6;
    QFile raw(capturePath);
    if (raw.open(QIODevice::ReadOnly)) {
        const auto bytes = raw.readAll();
        QJsonObject histogram;
        for (unsigned char value : bytes) {
            const auto key = QString::number(value, 16).rightJustified(2, '0');
            histogram[key] = histogram[key].toInt() + 1;
        }
        QJsonObject evidence{{"reference_revision", "bf13333dec4897accfb6012c958818ed17b6d6cc"},
            {"mode", "adapted_receive_only"}, {"port", parser.value("port")}, {"baud", 115200},
            {"started_utc_pc", startedUtc.toString(Qt::ISODateWithMs)},
            {"ended_utc_pc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"time_scale_verified", false}, {"tolerance_evaluated", false},
            {"byte_count", bytes.size()}, {"histogram_hex", histogram},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        QFile metadata(capturePath + ".json");
        const auto encoded = QJsonDocument(evidence).toJson();
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            metadata.write(encoded) != encoded.size() || !metadata.flush()) return 4;
    } else return 5;
    return result;
}
