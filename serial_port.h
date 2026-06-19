#pragma once
#include <QString>
#include <cstdint>

class ISerialPort {
public:
    virtual ~ISerialPort() = default;
    virtual bool open(const QString &portName, int baudRate) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool waitForReadyRead(int msecs) = 0;
    virtual qint64 read(char *data, qint64 maxSize) = 0;
    virtual bool hasError() const = 0;
    virtual QString errorString() const = 0;
    virtual void clearBuffers() = 0;
};
