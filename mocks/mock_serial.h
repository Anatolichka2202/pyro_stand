#pragma once
#include "../serial_port.h"
#include <QMap>
#include <QString>
#include <QThread>
#include <cstdint>

// Fast mock: returns bytes at CPU speed — use for unit/integration tests.
// Set setRealtime(true) for 1-ms-per-byte playback speed — use for demo mode.
class MockSerial : public ISerialPort {
public:
    void fireAt(int64_t tick, uint8_t mask) { m_data[tick] = mask; }
    void dropAfter(int64_t tick)            { m_dropTick = tick; }
    void setRealtime(bool rt)               { m_realtime = rt; }

    bool open(const QString &, int) override { m_open = true; return true; }
    void close() override                    { m_open = false; }
    bool isOpen() const override             { return m_open && m_tick <= m_dropTick; }

    bool waitForReadyRead(int) override {
        if (m_tick > m_dropTick) { m_open = false; return false; }
        if (m_realtime) QThread::msleep(1);
        return true;
    }

    qint64 read(char *data, qint64) override {
        if (!m_open) return -1;
        *data = static_cast<char>(m_data.value(m_tick, 0x00));
        ++m_tick;
        return 1;
    }

    bool hasError() const override    { return !m_open && m_tick > 0; }
    QString errorString() const override {
        return m_open ? QString() : QStringLiteral("MockSerial: port dropped");
    }
    // Сбрасывает состояние для повторного прогона (reset + re-run в demo/test)
    void clearBuffers() override { m_tick = 0; m_open = false; }

    int64_t currentTick() const { return m_tick; }

private:
    QMap<int64_t, uint8_t> m_data;
    int64_t m_dropTick = INT64_MAX;
    int64_t m_tick     = 0;
    bool    m_open     = false;
    bool    m_realtime = false;
};
