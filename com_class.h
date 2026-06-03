#ifndef COM_CLASS_H
#define COM_CLASS_H

#include <QString>
#include <QObject>
#include <QtSerialPort/QSerialPort>
#include <mutex>

class com_class : public QObject
{
    Q_OBJECT
public:
    explicit com_class(const QString& PortName, int baudRate = 9600, QObject* parent = nullptr);
    ~com_class();

    bool open();
    void close();
    void sendStartCommand();   // потокобезопасно

    bool isOpen() const;

signals:
    void commandSent(qint64 timestampMs);
    void responseReceived(qint64 timestampMs);

private slots:
    void onReadyRead();

private:
    QSerialPort* serial = nullptr;
    mutable std::mutex serialMutex; // защита доступа к serial
    qint64 lastSendTime = 0;
};

#endif // COM_CLASS_H
