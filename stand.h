#pragma once

#include <QObject>
#include <QSerialPort>
#include <atomic>
#include <thread>
#include <vector>
#include "cyclogram_data.h"
#include <QTime>
#include <QDate>
struct MaskRecord {
    int64_t absoluteIndex;   // номер принятого байта с начала сессии
    uint8_t mask;            // значение маски
};

class Stand : public QObject {
    Q_OBJECT
public:
    explicit Stand(QObject* parent = nullptr);
    ~Stand();

    void setStartMskTime(const QTime& mskTime);  // задаём время старта МСК
    bool start();   // запускаем чтение COM-порта и отправляем UDP
    void stop();    // останавливаем чтение и запускаем анализ

signals:
    void logMessage(const QString& msg);               // для лога в GUI
    void analysisComplete(const QVector<MaskRecord>& records, int64_t syncIndex); // для отображения результатов

private:
    void readingThread();           // поток чтения из COM-порта
    void sendUdpToBcvm();           // отправить UDP с временем старта
    void performAnalysis();         // сравнить маски с циклограммой

    QSerialPort* serial;
    std::atomic<bool> running;
    std::thread worker;

    QVector<MaskRecord> masks;       // все принятые маски
    int64_t syncIndex;               // индекс байта, в котором обнаружен канал 8 (бит 7)
    bool syncFound;                  // найден ли синхроимпульс

    QTime startMskTime;              // введённое пользователем время старта (МСК, без миллисекунд)
    QDateTime startUtcDateTime;      // соответствующее UTC

    QVector<CycleEvent> events;      // копия циклограммы
    static constexpr int testBlock = 9;
    static constexpr int totalDurationMs = 1'818'000; // длительность полётного задания (мс)
};
