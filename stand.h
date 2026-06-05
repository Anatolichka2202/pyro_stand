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
    int64_t absoluteIndex;   // номер принятого байта с начала сессии (от момента пуска)
    uint8_t mask;            // значение маски
};

class Stand : public QObject {
    Q_OBJECT
public:
    explicit Stand(QObject* parent = nullptr);
    ~Stand();

    void setStartMskTime(const QTime& mskTime);
    bool start();   // запуск сбора (очистка буфера, сброс индекса)
    void stop();    // штатная остановка с анализом
    void shutdown(); // тихая остановка для деструктора

signals:
    void logMessage(const QString& msg);                                            //
    void analysisComplete(const QVector<MaskRecord>& records, int64_t syncIndex);   //

private:
    void readingThread();               //
    void sendUdpToBcvm();               //
    void performAnalysis();             //
    void doStop(bool withAnalysis);     //

    QSerialPort* serial;                //
    std::atomic<bool> running;          //
    std::thread worker;                 //

    //структура
    struct LogEntry {
        int64_t relTime;
        QString text;
    };                          //
    QVector<LogEntry> entries;  //

    QVector<MaskRecord> masks;  //
    int64_t syncIndex;          //
    bool syncFound;             //

    QTime startMskTime;         //
    QDateTime startUtcDateTime; //

    QVector<CycleEvent> events;                             //
    bool analysisDone;                                      //

    int64_t getRelTime(int64_t absIndex) const;             //
    QDateTime utcFromRelTime(int64_t relTime) const;        //
    void addEventEntries(QVector<LogEntry>& entries) const; //  добавление события
    void addMaskEntries(QVector<LogEntry>& entries) const;  //  добавление маски
    void sortAndEmit(QVector<LogEntry>& entries);           //  подготовка окончательного сообщения для анализа, хроно сортировка

    // Константы
    static constexpr const char* SERIAL_PORT = "COM7";          // ком порт
    static constexpr int SERIAL_BAUDRATE = 115200;              // скорость
    static constexpr quint16 UDP_PORT = 4000;                   // порт БЦВМ
    static constexpr const char* BCVM_IP = "192.168.17.246";    // айпи бцвм
    static constexpr uint8_t SYNC_MASK = 0x80;                  // канал синхронизации
    static constexpr uint8_t UDP_COMMAND_START = 0x03;          // команда старта
    static constexpr int TOTAL_DURATION_MS = 1'818'000;         // время полетного задания в мс

};
