#pragma once
#include "serial_port.h"
#include <QSerialPort>
#include <memory>

// QSerialPort is not thread-safe and must be used from the thread that created it.
// We create it lazily inside open() so it always lives on the calling thread.
// Stand calls open() from readingThread, which is the correct owner.
class RealSerialPort : public ISerialPort {
public:
    bool open(const QString &portName, int baudRate) override {
        m_port = std::make_unique<QSerialPort>();
        m_port->setPortName(portName);
        m_port->setBaudRate(baudRate);
        m_port->setDataBits(QSerialPort::Data8);
        m_port->setParity(QSerialPort::NoParity);
        m_port->setStopBits(QSerialPort::OneStop);
        m_port->setFlowControl(QSerialPort::NoFlowControl);
        return m_port->open(QIODevice::ReadOnly);
    }
    void close() override { if (m_port) { m_port->close(); m_port.reset(); } }
    bool isOpen() const override { return m_port && m_port->isOpen(); }
    bool waitForReadyRead(int msecs) override { return m_port && m_port->waitForReadyRead(msecs); }
    qint64 read(char *data, qint64 maxSize) override { return m_port ? m_port->read(data, maxSize) : 0; }
    bool hasError() const override { return m_port && m_port->error() != QSerialPort::NoError; }
    QString errorString() const override { return m_port ? m_port->errorString() : QString(); }
    void clearBuffers() override { if (m_port) m_port->clear(QSerialPort::AllDirections); }
private:
    std::unique_ptr<QSerialPort> m_port;
};
