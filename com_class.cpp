#include "com_class.h"
#include <QDebug>
#include <QDateTime>

com_class::com_class(const QString& portName, int baudRate, QObject* parent)
    : QObject(parent) {
    std::lock_guard<std::mutex> lock(serialMutex);
    serial = new QSerialPort(portName, this);
    serial->setBaudRate(baudRate);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);
    connect(serial, &QSerialPort::readyRead, this, &com_class::onReadyRead);
}

com_class::~com_class() {
    close();
    delete serial;
}

bool com_class::open() {
    std::lock_guard<std::mutex> lock(serialMutex);
    if (serial->isOpen()) return true;
    if (!serial->open(QIODevice::ReadWrite)) {
        qWarning() << "Не удалось открыть порт" << serial->portName();
        return false;
    }
    return true;
}

void com_class::close() {
    std::lock_guard<std::mutex> lock(serialMutex);
    if (serial->isOpen()) serial->close();
}

bool com_class::isOpen() const {
    std::lock_guard<std::mutex> lock(serialMutex);
    return serial && serial->isOpen();
}

void com_class::sendStartCommand() {
    std::lock_guard<std::mutex> lock(serialMutex);
    if (!serial || !serial->isOpen()) return;
    QByteArray cmd;
    cmd.append(char(0xFF));
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (serial->write(cmd) == cmd.size()) {
        lastSendTime = now;
        // Сигнал испускаем из этого же потока (рабочий поток стенда)
        // Если сигнал подключен к слоту в главном потоке, Qt автоматически перенаправит через очередь
        emit commandSent(now);
        qDebug() << "Команда тестеру отправлена в" << now;
    }
}

void com_class::onReadyRead() {
    std::lock_guard<std::mutex> lock(serialMutex);
    QByteArray data = serial->readAll();
    if (data.size() > 0) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        emit responseReceived(now);
        qDebug() << "Ответ тестера получен в" << now;
    }
}
