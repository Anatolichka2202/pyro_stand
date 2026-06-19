#pragma once
#include "../serial_port.h"
#include <QMap>
#include <QString>
#include <cstdint>

class MockSerial : public ISerialPort {
public:
    // Установить конкретный байт на конкретном тике (абсолютный индекс)
    void fireAt(int64_t tick, uint8_t mask) { m_data[tick] = mask; }

    // После этого тика waitForReadyRead вернёт false и isOpen() вернёт false
    void dropAfter(int64_t tick) { m_dropTick = tick; }

    // --- ISerialPort ---
    bool open(const QString &, int) override { m_open = true; return true; }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open && m_tick <= m_dropTick; }

    bool waitForReadyRead(int) override {
        if (m_tick > m_dropTick) { m_open = false; return false; }
        return true;
    }

    qint64 read(char *data, qint64) override {
        if (!m_open) return -1;
        uint8_t b = m_data.value(m_tick, 0x00);
        *data = static_cast<char>(b);
        ++m_tick;
        return 1;
    }

    bool hasError() const override { return !m_open && m_tick > 0; }
    QString errorString() const override {
        return m_open ? QString() : QStringLiteral("MockSerial: port dropped");
    }
    void clearBuffers() override {}

    int64_t currentTick() const { return m_tick; }

private:
    QMap<int64_t, uint8_t> m_data;
    int64_t m_dropTick = INT64_MAX;
    int64_t m_tick     = 0;
    bool    m_open     = false;
};
