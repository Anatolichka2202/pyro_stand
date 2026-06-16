#pragma once

#include <QObject>
#include <QSerialPort>
#include <atomic>
#include <thread>
#include <vector>
//#include "cyclogram_data.h"
#include <QTime>
#include <QDate>

struct CycleEvent {
    int time_ms;
    QString key;          // имя события (переменная)
    QString description;   // описания
    int block;       // для отслеживания блоков
    bool needTester; // для отслеживания
};

struct MaskRecord {
    int64_t absoluteIndex;   // такт от пуска
    uint8_t mask;            // значение маски
};

class Stand : public QObject {
    Q_OBJECT
public:
    explicit Stand(QObject* parent = nullptr);
    ~Stand();

    void setStartMskTime(const QTime& mskTime);
    bool start();           // запуск сбора (очистка буфера, сброс индекса)
    void stop();            // штатная остановка с анализом
    void shutdown();        // тихая остановка для деструктора

signals:
    void logMessage(const QString& msg);
    void analysisComplete(const QVector<MaskRecord>& records, int64_t syncIndex);

private:
    // Поток чтения
    void readingThread();
    // Отправка UDP с временем старта
    void sendUdpToBcvm();
    // Анализ собранных данных
    void performAnalysis();
    // Общая остановка (withAnalysis = true -> анализ)
    void doStop(bool withAnalysis);

    // Вспомогательные функции для анализа
    struct LogEntry {
        int64_t relTime;
        QString text;
    };
    int64_t getRelTime(int64_t absIndex) const;
    QDateTime utcFromRelTime(int64_t relTime) const;
    void addEventEntries(QVector<LogEntry>& entries) const;
    void addFiredChannelsEntries(QVector<LogEntry>& entries) const;
    void sortAndEmit(QVector<LogEntry>& entries);

    void updateTotalDuration(); //обновление общего времени

    // Члены класса
    QSerialPort* serial;
    std::atomic<bool> running;
    std::thread worker;

    QVector<MaskRecord> masks;     // все полученные маски (для полноты)
    int64_t syncIndex;             // абсолютный индекс, где впервые появился бит 7
    bool syncFound;                //  найден ли синхромаркер

    QTime startMskTime;
    QDateTime startUtcDateTime;

    QVector<CycleEvent> events;    // копия циклограммы
    bool analysisDone;

    bool loadCyclogramFromFile(const QString& filePatch);

    QString cyclogramFilePath = "cyclogram.ini";
    QByteArray generateDatagram();

    int totalDurationMs;  // вычисляется из файла (макс. время + запас)
    // Константы
    static constexpr const char* SERIAL_PORT = "COM7";
    static constexpr int SERIAL_BAUDRATE = 115200;
    static constexpr quint16 UDP_PORT = 4000;
    static constexpr const char* BCVM_IP = "192.168.17.246";
    static constexpr uint8_t SYNC_MASK = 0x80;
    static constexpr uint8_t UDP_COMMAND_START = 0x03;
    static constexpr int TOTAL_DURATION_MS = 1'818'000;//1'818'000;
};
