#pragma once
#include "serial_port.h"
#include <QSerialPort>

class RealSerialPort : public ISerialPort {
public:
    bool open(const QString &portName, int baudRate) override {
        m_port.setPortName(portName);
        m_port.setBaudRate(baudRate);
        m_port.setDataBits(QSerialPort::Data8);
        m_port.setParity(QSerialPort::NoParity);
        m_port.setStopBits(QSerialPort::OneStop);
        m_port.setFlowControl(QSerialPort::NoFlowControl);
        return m_port.open(QIODevice::ReadOnly);
    }
    void close() override { m_port.close(); }
    bool isOpen() const override { return m_port.isOpen(); }
    bool waitForReadyRead(int msecs) override { return m_port.waitForReadyRead(msecs); }
    qint64 read(char *data, qint64 maxSize) override { return m_port.read(data, maxSize); }
    bool hasError() const override { return m_port.error() != QSerialPort::NoError; }
    QString errorString() const override { return m_port.errorString(); }
    void clearBuffers() override { m_port.clear(QSerialPort::AllDirections); }
private:
    QSerialPort m_port;
};
