#include "free_recording.h"
#include <QUuid>

namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
constexpr qsizetype maxBytes = 16 * 1024 * 1024;
constexpr qsizetype maxRecords = 16000;
// Полуоборот счётчика; за этим пределом знак разности уже неоднозначен.
constexpr qint64 maxDurationMs = 2147000;
}

bool FreeRecording::start(const Config &config) {
    if (m_active || config.group < 0 || config.group > (config.legacy ? 0 : 11)
        || config.selected == 0 || config.kp < -1
        || config.kp >= (config.legacy ? 8 : 96)) return false;
    m_config = config;
    m_protocol.clear();
    m_frame = {};
    m_decoder.reset();
    m_kp.reset();
    m_raw = {}; m_edges = {}; m_baseline = {}; m_bytes = 0;
    m_rejectedFrames = 0;
    m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_started = utcNow(); m_stopped.clear();
    m_active = true; m_armed = false;
    m_status = QStringLiteral("Ожидание чистого исходного состояния; команды плате не отправляются");
    m_elapsed.start();
    return true;
}

bool FreeRecording::relevant(int index) const {
    const int local = index - m_config.group * 8;
    return index == m_config.kp || (local >= 0 && local < 8 && (m_config.selected & (1 << local)));
}

void FreeRecording::stop(const QString &reason) {
    if (!m_active) return;
    m_active = false; m_status = reason; m_stopped = utcNow();
}

void FreeRecording::checkDeadline() {
    if (m_active && m_elapsed.elapsed() >= maxDurationMs)
        stop(QStringLiteral("Остановлено: достигнут предел однозначного интервала счётчика"));
}

QString FreeRecording::status() const {
    return m_rejectedFrames ? m_status + QStringLiteral(" · отклонённых кадров: %1").arg(m_rejectedFrames) : m_status;
}

bool FreeRecording::retain(const QByteArray &raw, const QString &error) {
    checkDeadline();
    if (!m_active) return false;
    if (raw.size() > maxBytes - m_bytes || m_raw.size() >= maxRecords) {
        stop(QStringLiteral("Остановлено: лимит архива; следующая порция данных не записана"));
        return false;
    }
    m_raw.append(QJsonObject{{"received_at_utc", utcNow()},
        {"valid", error.isEmpty()}, {"error", error},
        {"raw_base64", QString::fromLatin1(raw.toBase64())},
        {"next_sample_index", QString::number(m_decoder.sampleCount() + 1)}});
    m_bytes += raw.size();
    return true;
}

void FreeRecording::acceptRejected(const QByteArray &raw, const QString &error) {
    // Повреждённый кадр сохраняется, но не вооружает сбор и не создаёт измерений.
    if (!m_active || m_config.legacy) return;
    if (retain(raw, error.isEmpty() ? QStringLiteral("Кадр отклонён декодером") : error))
        ++m_rejectedFrames;
}

void FreeRecording::observe(const BoardFrame &frame, bool legacy, quint32 sampleTick) {
    for (int i = 0; i < (legacy ? 8 : 96); ++i) {
        if (!relevant(i)) continue;
        const auto &before = m_frame.channels[size_t(i)];
        const auto &now = frame.channels[size_t(i)];
        const auto riseTick = frame.timestampUs(i), fallTick = frame.timestampUs(i, true);
        const bool rise = legacy ? now.ready && !before.ready : riseTick && !m_frame.timestampUs(i);
        const bool fall = legacy ? !now.ready && before.ready : fallTick && !m_frame.timestampUs(i, true);
        for (bool falling : {false, true}) {
            if (!(falling ? fall : rise)) continue;
            if (m_edges.size() >= maxRecords) {
                stop(QStringLiteral("Остановлено: лимит истории фронтов"));
                return;
            }
            const quint64 tick = legacy ? sampleTick : *(falling ? fallTick : riseTick);
            m_edges.append(QJsonObject{{"input", i + 1}, {"edge", falling ? "fall" : "rise"},
                {"tick_us", QString::number(tick)}, {"sample_index", QString::number(m_decoder.sampleCount())}});
            if (!m_kp && i == m_config.kp && falling == m_config.fallingKp) m_kp = tick;
        }
    }
    m_frame.isAa58 = frame.isAa58;
    m_frame.localNowUs = frame.localNowUs;
    m_frame.timeFlags = frame.timeFlags;
    for (int i = 0; i < (legacy ? 8 : 96); ++i)
        if (relevant(i)) m_frame.channels[size_t(i)] = frame.channels[size_t(i)];
}

void FreeRecording::acceptFrame(const BoardFrame &frame, const QByteArray &raw) {
    if (!m_active || m_config.legacy || !retain(raw)) return;
    const auto protocol = frame.protocol();
    m_protocol = m_protocol.isEmpty() || m_protocol == protocol ? protocol : QStringLiteral("mixed");
    if (frame.isAa58 && (frame.timeFlags & 0x80)) {
        stop(QStringLiteral("Остановлено: переполнение времени платы")); return;
    }
    if (m_armed && (frame.isAa58 != m_frame.isAa58 ||
        (frame.isAa58 && frame.localNowUs < m_frame.localNowUs))) {
        stop(QStringLiteral("Остановлено: смена протокола или перезапуск платы")); return;
    }
    if (!m_armed) {
        for (int i = 0; i < 96; ++i) {
            if (!relevant(i)) continue;
            const auto &c = frame.channels[size_t(i)];
            if (c.triggered || c.ready || c.riseUs || c.fallUs || c.riseLocalUs || c.fallLocalUs || c.initialActive || c.fallSeen) return;
        }
        m_armed = true;
        m_frame.isAa58 = frame.isAa58;
        m_frame.localNowUs = frame.localNowUs;
        m_frame.timeFlags = frame.timeFlags;
        m_baseline = {{"raw_record", m_raw.size() - 1}};
        m_status = QStringLiteral("Регистрация · чистое состояние получено");
    }
    for (int i = 0; i < 96; ++i) {
        if (!relevant(i)) continue;
        const auto &old = m_frame.channels[size_t(i)];
        const auto &now = frame.channels[size_t(i)];
        if (frame.isAa58) {
            const bool changed = (old.riseLocalUs && old.riseLocalUs != now.riseLocalUs) ||
                (old.fallSeen && (!now.fallSeen || old.fallLocalUs != now.fallLocalUs));
            const bool invalid = now.initialActive || now.triggered != (now.riseLocalUs != 0) ||
                (now.ready && !now.triggered) || (now.fallSeen && !now.triggered) ||
                (now.fallSeen != (now.fallLocalUs != 0)) ||
                now.riseLocalUs > frame.localNowUs || now.fallLocalUs > frame.localNowUs ||
                (now.fallSeen && now.fallLocalUs < now.riseLocalUs);
            if (changed || invalid) {
                stop(QStringLiteral("Остановлено: изменившиеся или недостоверные отметки AA58")); return;
            }
            continue;
        }
        if ((old.riseUs && old.riseUs != now.riseUs) || (old.fallUs && old.fallUs != now.fallUs)) {
            stop(QStringLiteral("Остановлено: сохранённые отметки изменились; эпоха измерения неоднозначна"));
            return;
        }
        if (now.triggered != (now.riseUs != 0) || ((now.ready || now.fallUs) && !now.riseUs)) {
            stop(QStringLiteral("Остановлено: нулевая или противоречивая аппаратная отметка"));
            return;
        }
        if (now.riseUs && now.fallUs && quint32(now.fallUs - now.riseUs) >= 0x80000000u) {
            stop(QStringLiteral("Остановлено: неоднозначная длительность импульса"));
            return;
        }
    }
    observe(frame, false);
}

void FreeRecording::acceptLegacy(const QByteArray &raw) {
    if (!m_active || !m_config.legacy || raw.isEmpty() || !retain(raw)) return;
    int byteOffset = -1;
    for (char byte : raw) {
        ++byteOffset;
        if (!m_active) break;
        if (!m_armed) {
            const quint8 relevantMask = m_config.selected | (m_config.kp < 0 ? 0 : 1 << m_config.kp);
            if ((quint8(byte) & relevantMask) != 0) continue;
            m_armed = true;
            m_baseline = {{"raw_record", m_raw.size() - 1}, {"byte_offset", byteOffset}, {"sample_index", "1"}};
            m_status = QStringLiteral("Регистрация · нулевая маска получена");
        }
        const auto frame = m_decoder.append(quint8(byte));
        // Ограничиваем также аппаратное время: доставка может идти быстрее реального времени.
        if (m_decoder.sampleCount() >= 2147000) {
            stop(QStringLiteral("Остановлено: предел интервала аппаратных отсчётов"));
            break;
        }
        observe(frame, true, quint32(m_decoder.sampleCount() * 1000));
    }
}

std::optional<double> FreeRecording::relativeMs(quint64 tick) const {
    if (!m_kp) return std::nullopt;
    if (m_frame.isAa58) return double(qint64(tick) - qint64(*m_kp)) / 1000.0;
    const quint32 difference = quint32(tick - *m_kp);
    if (difference == 0x80000000u) return std::nullopt;
    const qint64 signedDifference = difference < 0x80000000u ? qint64(difference)
        : qint64(difference) - 0x100000000LL;
    return double(signedDifference) / 1000.0;
}

QJsonObject FreeRecording::report() const {
    return {{"format", "pyro.free-recording.v1"}, {"recording_id", m_id},
        {"started_at_utc", m_started}, {"stopped_at_utc", m_stopped},
        {"active", m_active}, {"status", status()}, {"baseline_observed", m_armed},
        {"rejected_frames", m_rejectedFrames},
        {"raw_scope", m_config.legacy ? "Raw mask chunks of the selected COM"
            : "Validated frames and rejected UDP datagrams / RS485 candidates; not a complete serial byte stream"},
        {"baseline", m_baseline},
        {"source", m_config.source}, {"protocol", m_config.legacy ? QStringLiteral("legacy-mask-1ms") : m_protocol.isEmpty() ? QStringLiteral("unknown") : m_protocol},
        {"yps", m_config.legacy ? 0 : m_config.group + 1}, {"selected_mask", int(m_config.selected)},
        {"kp_input", m_config.kp + 1}, {"kp_edge", m_config.fallingKp ? "fall" : "rise"},
        {"kp_tick_us", m_kp ? QJsonValue(QString::number(*m_kp)) : QJsonValue()},
        {"tick_unit", "microsecond"}, {"tolerance_evaluated", false},
        {"input_numbering", m_config.legacy ? "1..8 in selected COM" : "1..96 in selected device"},
        {"relative_time", m_frame.isAa58 ? "(event_local_us - kp_local_us) / 1000 ms; overflow excluded" : "signed modulo-2^32(event_tick - kp_tick) / 1000 ms; half-turn excluded"},
        {"limitations", m_config.legacy ? "No CRC/sequence; missing samples cannot be excluded"
            : "First stored rise/fall only; no complete repeated pulse history or sequence"},
        {"raw_records", m_raw}, {"edges", m_edges}};
}
