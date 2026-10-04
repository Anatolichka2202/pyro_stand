#include "integration.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace {
constexpr int kFrameSize = 2 + 96 * 10 + 1;
constexpr int kMaxDebugLine = 4 * 1024 * 1024;

bool fail(QString *error, const QString &reason) {
    if (error) *error = reason;
    return false;
}

quint8 crc8(const QByteArray &bytes) {
    quint8 crc = 0;
    for (char byte : bytes) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = crc & 0x80 ? quint8((crc << 1) ^ 0x31) : quint8(crc << 1);
    }
    return crc;
}

std::optional<InputRef> parseInput(const QJsonValue &value) {
    if (!value.isObject()) return std::nullopt;
    const auto object = value.toObject();
    if (!object.value(QStringLiteral("yps")).isDouble() ||
        !object.value(QStringLiteral("channel")).isDouble()) return std::nullopt;
    const int yps = object.value(QStringLiteral("yps")).toInt(-1);
    const int channel = object.value(QStringLiteral("channel")).toInt(-1);
    const QString edge = object.value(QStringLiteral("edge")).toString(QStringLiteral("rise"));
    if (yps < 1 || yps > 12 || channel < 1 || channel > 8 ||
        (edge != QStringLiteral("rise") && edge != QStringLiteral("fall"))) return std::nullopt;
    return InputRef{(yps - 1) * 8 + channel - 1, edge == QStringLiteral("fall")};
}

std::optional<quint32> timestamp(const BoardChannel &channel, const InputRef &input) {
    if (input.falling) return channel.fallUs == 0 ? std::nullopt
                                                : std::optional<quint32>(channel.fallUs);
    return channel.triggered && channel.riseUs != 0 ? std::optional<quint32>(channel.riseUs)
                                                     : std::nullopt;
}

bool blankFrame(const BoardFrame &frame) {
    return std::all_of(frame.channels.cbegin(), frame.channels.cend(), [](const BoardChannel &ch) {
        return !ch.triggered && !ch.ready && ch.riseUs == 0 && ch.fallUs == 0;
    });
}
}

bool StandProtocol::parseCyclogram(const QByteArray &bytes, CyclogramPlan *plan, QString *error) {
    if (!plan || bytes.isEmpty()) return fail(error, QStringLiteral("Пустая циклограмма"));
    CyclogramPlan parsed;
    QDate date;
    QTime time;
    const QStringList lines = QString::fromUtf8(bytes).split(QRegularExpression(QStringLiteral("[\\r\\n]+")));
    const QRegularExpression integer(QStringLiteral("^[+-]?[0-9]+$"));
    int ordinal = 0;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';')) continue;
        const int eq = line.indexOf(u'=');
        if (eq <= 0) return fail(error, QStringLiteral("Строка %1: нет ключа или '='").arg(i + 1));
        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        const int hash = value.indexOf(u'#');
        if (hash >= 0) value = value.left(hash).trimmed();
        if (key == QStringLiteral("START_UTC_DATE")) {
            value.replace(u'/', u'-');
            value.replace(u'.', u'-');
            date = QDate::fromString(value, Qt::ISODate);
            if (!date.isValid() || date.year() < 2020 || date.year() > 2099)
                return fail(error, QStringLiteral("Строка %1: неверная START_UTC_DATE").arg(i + 1));
        } else if (key == QStringLiteral("START_UTC_TIME")) {
            const QRegularExpression clock(QStringLiteral("^([0-9]{1,2}):([0-9]{1,2}):([0-9]{1,2})$"));
            const auto match = clock.match(value);
            time = QTime();
            if (match.hasMatch())
                time = QTime(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
            if (!time.isValid())
                return fail(error, QStringLiteral("Строка %1: неверная START_UTC_TIME").arg(i + 1));
        } else {
            if (key.isEmpty() || !integer.match(value).hasMatch())
                return fail(error, QStringLiteral("Строка %1: неверное действие/смещение").arg(i + 1));
            bool ok = false;
            const qint64 offset = value.toLongLong(&ok);
            if (!ok || offset < std::numeric_limits<qint32>::min() ||
                offset > std::numeric_limits<qint32>::max())
                return fail(error, QStringLiteral("Строка %1: смещение вне диапазона").arg(i + 1));
            parsed.actions.append({key, qint32(offset), ++ordinal});
            if (parsed.actions.size() > 256)
                return fail(error, QStringLiteral("Слишком много действий"));
        }
    }
    if (!date.isValid() || !time.isValid() || parsed.actions.isEmpty())
        return fail(error, QStringLiteral("Нет START_UTC_DATE, START_UTC_TIME или действий"));
    parsed.t0Utc = QDateTime(date, time, QTimeZone::UTC);
    std::stable_sort(parsed.actions.begin(), parsed.actions.end(),
                     [](const PlannedAction &a, const PlannedAction &b) { return a.offsetMs < b.offsetMs; });
    *plan = parsed;
    return true;
}

bool StandProtocol::parseBoardFrame(const QByteArray &bytes, BoardFrame *frame, QString *error) {
    if (!frame || bytes.size() != kFrameSize)
        return fail(error, QStringLiteral("Неверная длина кадра платы"));
    if (quint8(bytes[0]) != 0xAA || quint8(bytes[1]) != 0x55)
        return fail(error, QStringLiteral("Неверный заголовок кадра платы"));
    if (crc8(bytes.first(kFrameSize - 1)) != quint8(bytes[kFrameSize - 1]))
        return fail(error, QStringLiteral("Ошибка CRC8 кадра платы"));
    BoardFrame parsed;
    const auto *raw = reinterpret_cast<const uchar *>(bytes.constData());
    for (int i = 0; i < 96; ++i) {
        const int offset = 2 + i * 10;
        parsed.channels[i].triggered = raw[offset] != 0;
        parsed.channels[i].ready = raw[offset + 1] != 0;
        parsed.channels[i].riseUs = qFromLittleEndian<quint32>(raw + offset + 2);
        parsed.channels[i].fallUs = qFromLittleEndian<quint32>(raw + offset + 6);
    }
    *frame = parsed;
    return true;
}

bool StandProtocol::parseMapping(const QByteArray &bytes, StandMapping *mapping, QString *error) {
    if (!mapping) return fail(error, QStringLiteral("Нет приёмника карты"));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (!doc.isObject()) return fail(error, QStringLiteral("Карта каналов: неверный JSON"));
    const QJsonObject root = doc.object();
    StandMapping parsed;
    parsed.version = root.value(QStringLiteral("version")).toString().trimmed();
    const auto kp = parseInput(root.value(QStringLiteral("kp")));
    if (parsed.version.isEmpty() || !kp || !root.value(QStringLiteral("actions")).isArray())
        return fail(error, QStringLiteral("Нужны version, kp и actions"));
    parsed.kp = *kp;
    QSet<int> assignedSignals;
    for (const QJsonValue &value : root.value(QStringLiteral("actions")).toArray()) {
        if (!value.isObject()) return fail(error, QStringLiteral("Неверная запись действия"));
        const auto object = value.toObject();
        const QString key = object.value(QStringLiteral("key")).toString().trimmed();
        const QJsonValue inputs = object.value(QStringLiteral("inputs"));
        if (key.isEmpty() || parsed.actions.contains(key) || !inputs.isArray() || inputs.toArray().isEmpty())
            return fail(error, QStringLiteral("Неверный или повторный ключ действия"));
        ActionMapping action;
        for (const QJsonValue &input : inputs.toArray()) {
            const auto parsedInput = parseInput(input);
            if (!parsedInput)
                return fail(error, QStringLiteral("Неверный вход карты каналов"));
            const int signal = parsedInput->index * 2 + int(parsedInput->falling);
            if (assignedSignals.contains(signal))
                return fail(error, QStringLiteral("Один фронт/спад назначен нескольким действиям"));
            assignedSignals.insert(signal);
            action.inputs.append(*parsedInput);
        }
        if (object.contains(QStringLiteral("tolerance_ms"))) {
            const QJsonValue tolerance = object.value(QStringLiteral("tolerance_ms"));
            if (!tolerance.isDouble() || tolerance.toDouble() < 0)
                return fail(error, QStringLiteral("Неверный tolerance_ms"));
            action.toleranceMs = tolerance.toDouble();
        }
        parsed.actions.insert(key, action);
    }
    *mapping = parsed;
    return true;
}

QByteArray StandProtocol::resetCommand() {
    QByteArray command;
    command.append(char(0xC0));
    command.append(char(0xC1));
    command.append(char(0x01));
    command.append(char(crc8(command)));
    return command;
}

IntegrationSession::IntegrationSession(Options options, QObject *parent)
    : QObject(parent), m_options(std::move(options)) {
    m_reconnect.setInterval(1000);
    connect(&m_reconnect, &QTimer::timeout, this, &IntegrationSession::connectDebug);
    connect(&m_debug, &QLocalSocket::readyRead, this, &IntegrationSession::readDebug);
    connect(&m_debug, &QLocalSocket::disconnected, this, [this] {
        m_debugBuffer.clear();
        m_streamId.clear();
        m_nextSeq = 0;
        m_lockState = QStringLiteral("UNKNOWN");
        m_executionEnabled = false;
        emit message(QStringLiteral("Соединение с Debug КАСУ ПП потеряно"));
        emit changed();
    });
    connect(&m_udp, &QUdpSocket::readyRead, this, &IntegrationSession::receiveUdp);
    connect(&m_serial, &QSerialPort::readyRead, this, &IntegrationSession::receiveSerial);
    m_boardWatchdog.setInterval(1500);
    connect(&m_boardWatchdog, &QTimer::timeout, this, [this] {
        if (m_boardReceiving) { m_boardReceiving = false; emit changed(); }
    });
    m_resetWatchdog.setSingleShot(true);
    connect(&m_resetWatchdog, &QTimer::timeout, this, [this] {
        if (m_awaitReset) {
            m_awaitReset = false;
            setError(QStringLiteral("После команды сброса не получен пустой кадр платы"));
        }
    });
    m_frameEvidence.setAutoRemove(true);
    if (!m_frameEvidence.open()) setError(QStringLiteral("Не удалось создать журнал кадров платы"));
}

IntegrationSession::~IntegrationSession() {
    m_reconnect.stop();
    m_boardWatchdog.stop();
    m_resetWatchdog.stop();
    m_debug.disconnect(this);
    m_udp.disconnect(this);
    m_serial.disconnect(this);
    m_debug.abort();
    m_udp.close();
    m_serial.close();
}

void IntegrationSession::start() {
    if (!m_reconnect.isActive()) m_reconnect.start();
    connectDebug();
    if (m_options.boardTransport == BoardTransport::Udp) {
        if (!m_udp.bind(m_options.bindIp, m_options.telemetryPort,
                        QUdpSocket::DontShareAddress))
            setError(QStringLiteral("UDP %1: %2").arg(m_options.telemetryPort).arg(m_udp.errorString()));
    } else {
        m_serial.setPortName(m_options.serialPort);
        m_serial.setBaudRate(115200);
        if (!m_serial.open(QIODevice::ReadOnly))
            setError(QStringLiteral("RS-485 %1: %2").arg(m_options.serialPort, m_serial.errorString()));
    }
    emit changed();
}

void IntegrationSession::connectDebug() {
    if (m_debug.state() == QLocalSocket::UnconnectedState && !m_options.debugSocket.isEmpty())
        m_debug.connectToServer(m_options.debugSocket, QIODevice::ReadOnly);
}

void IntegrationSession::resetDebugState() {
    m_uploadId.clear();
    m_uploadState = QStringLiteral("NONE");
    m_lockState = QStringLiteral("UNKNOWN");
    m_bcvmUtc.clear();
    m_executionEnabled = false;
    m_executionObserved = false;
    m_hasPlan = false;
    m_cyclogramBytes.clear();
    m_cyclogramSha.clear();
    m_plan = {};
    m_boardArmed = false;
    m_prePermissionActivity = false;
    m_awaitReset = false;
    m_resetWatchdog.stop();
    emit changed();
}

void IntegrationSession::readDebug() {
    m_debugBuffer.append(m_debug.readAll());
    if (m_debugBuffer.size() > kMaxDebugLine && !m_debugBuffer.contains('\n')) {
        setError(QStringLiteral("Слишком длинная строка Debug"));
        m_debug.abort();
        return;
    }
    int newline = 0;
    while ((newline = m_debugBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_debugBuffer.left(newline);
        m_debugBuffer.remove(0, newline + 1);
        if (line.size() > kMaxDebugLine) { setError(QStringLiteral("Слишком длинная строка Debug")); m_debug.abort(); return; }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (!document.isObject()) { setError(QStringLiteral("Неверный JSON от Debug")); m_debug.abort(); return; }
        const QJsonObject object = document.object();
        if (object.value(QStringLiteral("contract")).toString() != QStringLiteral("kasu.pp.debug.stand.v1"))
            continue; // диагностические строки без контракта идут через тот же сокет
        bool seqOk = false;
        const quint64 seq = object.value(QStringLiteral("seq")).toString().toULongLong(&seqOk);
        const QString stream = object.value(QStringLiteral("stream_id")).toString();
        const QString run = object.value(QStringLiteral("run_id")).toString();
        if (!seqOk || seq == 0 || stream.isEmpty() || run.isEmpty()) {
            setError(QStringLiteral("Неверная последовательность Debug")); m_debug.abort(); return;
        }
        if (m_streamId.isEmpty()) {
            if (seq != 1) {
                setError(QStringLiteral("Первое сообщение потока Debug имеет seq != 1"));
                m_debug.abort(); return;
            }
            m_streamId = stream;
            m_nextSeq = seq;
        }
        if (stream != m_streamId || seq != m_nextSeq) {
            setError(QStringLiteral("Пропуск или смена потока Debug; переподключение"));
            m_debug.abort(); return;
        }
        ++m_nextSeq;
        if (!m_runId.isEmpty() && run != m_runId) resetDebugState();
        m_runId = run;
        m_debugEvidence.append(object);
        processDebug(object);
    }
}

void IntegrationSession::processDebug(const QJsonObject &object) {
    const QString type = object.value(QStringLiteral("type")).toString();
    const QString id = object.value(QStringLiteral("upload_id")).toString();
    if (type == QStringLiteral("CYCLOGRAM_AVAILABLE")) {
        const QByteArray encoded = object.value(QStringLiteral("content_base64")).toString().toLatin1();
        const QByteArray bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const int length = object.value(QStringLiteral("byte_count")).toInt(-1);
        if (id.isEmpty() || length < 0 || bytes.size() != length ||
            hash.compare(object.value(QStringLiteral("sha256_hex")).toString(), Qt::CaseInsensitive) != 0) {
            setError(QStringLiteral("Файл Debug: неверные размер, SHA-256 или upload_id")); return;
        }
        CyclogramPlan plan;
        QString error;
        if (!StandProtocol::parseCyclogram(bytes, &plan, &error)) {
            setError(QStringLiteral("Файл Debug: %1").arg(error)); return;
        }
        if (id != m_uploadId) {
            m_uploadId = id;
            m_uploadState = QStringLiteral("PREPARED");
            m_executionEnabled = false;
            m_executionObserved = false;
            m_lockState = QStringLiteral("UNKNOWN");
            m_bcvmUtc.clear();
            m_boardArmed = false;
            m_prePermissionActivity = false;
            m_awaitReset = false;
            m_resetWatchdog.stop();
            m_hasBoardFrame = false;
            emit message(QStringLiteral("Новая циклограмма; сбросьте плату перед прогоном"));
        }
        m_cyclogramBytes = bytes;
        m_cyclogramSha = hash;
        m_plan = plan;
        m_hasPlan = true;
        emit changed();
    } else if (type == QStringLiteral("CYCLOGRAM_UPLOAD_STATE") && id == m_uploadId) {
        const QString state = object.value(QStringLiteral("upload_state")).toString();
        if (state != QStringLiteral("CONFIRMED") && state != QStringLiteral("FAILED")) return;
        m_uploadState = state;
        if (state == QStringLiteral("FAILED")) {
            m_executionEnabled = false;
            m_executionObserved = false;
        }
        m_bcvmUtc = object.value(QStringLiteral("bcvm_time_valid")).toBool()
                        ? object.value(QStringLiteral("bcvm_utc")).toString() : QString();
        emit changed();
    } else if (type == QStringLiteral("BCVM_EXECUTION_ENABLED") && id == m_uploadId &&
               m_uploadState == QStringLiteral("CONFIRMED")) {
        m_executionEnabled = true;
        m_executionObserved = true;
        m_lockState = QStringLiteral("UNLOCKED");
        emit changed();
    } else if (type == QStringLiteral("BCVM_LOCKED")) {
        m_executionEnabled = false;
        m_lockState = QStringLiteral("LOCKED");
        emit changed();
    } else if (type == QStringLiteral("BCVM_STATE")) {
        const QString state = object.value(QStringLiteral("lock_state")).toString();
        m_lockState = object.value(QStringLiteral("fresh")).toBool() &&
                              (state == QStringLiteral("LOCKED") || state == QStringLiteral("UNLOCKED"))
                          ? state : QStringLiteral("UNKNOWN");
        m_executionEnabled = m_lockState == QStringLiteral("UNLOCKED") &&
            m_uploadState == QStringLiteral("CONFIRMED") && id == m_uploadId &&
            object.value(QStringLiteral("execution_enabled_confirmed")).toBool();
        if (m_executionEnabled) m_executionObserved = true;
        emit changed();
    } else if (type == QStringLiteral("OPERATION_ERROR")) {
        setError(QStringLiteral("КАСУ ПП: %1 — %2").arg(
            object.value(QStringLiteral("operation")).toString(),
            object.value(QStringLiteral("result")).toString()));
    }
}

void IntegrationSession::receiveUdp() {
    while (m_udp.hasPendingDatagrams()) {
        QByteArray raw;
        raw.resize(int(m_udp.pendingDatagramSize()));
        QHostAddress sender;
        m_udp.readDatagram(raw.data(), raw.size(), &sender);
        if (sender != m_options.boardIp) continue;
        processFrame(raw);
    }
}

void IntegrationSession::receiveSerial() {
    m_serialBuffer.append(m_serial.readAll());
    while (true) {
        const int header = m_serialBuffer.indexOf(QByteArray::fromHex("aa55"));
        if (header < 0) {
            m_serialBuffer = m_serialBuffer.endsWith(char(0xAA)) ? QByteArray(1, char(0xAA)) : QByteArray();
            return;
        }
        if (header > 0) m_serialBuffer.remove(0, header);
        if (m_serialBuffer.size() < kFrameSize) return;
        const QByteArray candidate = m_serialBuffer.first(kFrameSize);
        BoardFrame parsed;
        if (StandProtocol::parseBoardFrame(candidate, &parsed, nullptr)) {
            m_serialBuffer.remove(0, kFrameSize);
            processFrame(candidate);
        } else {
            ++m_rejectedFrames;
            recordFrame(candidate, QStringLiteral("Неверный кадр RS-485"));
            m_serialBuffer.remove(0, 1);
            emit changed();
        }
    }
}

void IntegrationSession::processFrame(const QByteArray &raw) {
    BoardFrame parsed;
    QString error;
    if (!StandProtocol::parseBoardFrame(raw, &parsed, &error)) {
        ++m_rejectedFrames;
        recordFrame(raw, error);
        setError(error);
        return;
    }
    m_boardFrame = parsed;
    m_hasBoardFrame = true;
    m_boardReceiving = true;
    m_boardWatchdog.start();
    ++m_acceptedFrames;
    recordFrame(raw);
    if (m_awaitReset && blankFrame(parsed)) {
        m_awaitReset = false;
        m_resetWatchdog.stop();
        m_boardArmed = true;
        QJsonObject evidence;
        evidence.insert(QStringLiteral("type"), QStringLiteral("BOARD_EMPTY_FRAME_AFTER_RESET"));
        evidence.insert(QStringLiteral("at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        evidence.insert(QStringLiteral("upload_id"), m_uploadId);
        m_localEvidence.append(evidence);
        emit message(QStringLiteral("После команды сброса получен пустой кадр; отдельного ACK у платы нет"));
    }
    if (m_boardArmed && !m_executionObserved && !blankFrame(parsed))
        m_prePermissionActivity = true;
    emit changed();
}

void IntegrationSession::recordFrame(const QByteArray &raw, const QString &error) {
    if (!m_frameEvidence.isOpen()) return;
    QJsonObject object;
    object.insert(QStringLiteral("received_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("upload_id"), m_uploadId);
    object.insert(QStringLiteral("board_armed"), m_boardArmed);
    object.insert(QStringLiteral("valid"), error.isEmpty());
    if (!error.isEmpty()) object.insert(QStringLiteral("error"), error);
    object.insert(QStringLiteral("frame_base64"), QString::fromLatin1(raw.toBase64()));
    m_frameEvidence.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}

bool IntegrationSession::loadMapping(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { setError(QStringLiteral("Карта каналов: %1").arg(file.errorString())); return false; }
    const QByteArray bytes = file.readAll();
    StandMapping mapping;
    QString error;
    if (!StandProtocol::parseMapping(bytes, &mapping, &error)) { setError(error); return false; }
    m_mapping = mapping;
    m_mappingBytes = bytes;
    emit message(QStringLiteral("Загружена карта каналов: %1").arg(mapping.version));
    emit changed();
    return true;
}

bool IntegrationSession::resetBoard() {
    const QByteArray command = StandProtocol::resetCommand();
    QUdpSocket commandSocket;
    if (commandSocket.writeDatagram(command, m_options.boardIp, m_options.commandPort) != command.size()) {
        setError(QStringLiteral("Не удалось отправить сброс: %1").arg(commandSocket.errorString()));
        return false;
    }
    m_boardArmed = false;
    m_prePermissionActivity = false;
    m_awaitReset = true;
    m_resetWatchdog.start(2000);
    QJsonObject evidence;
    evidence.insert(QStringLiteral("type"), QStringLiteral("BOARD_RESET_REQUESTED"));
    evidence.insert(QStringLiteral("at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    evidence.insert(QStringLiteral("upload_id"), m_uploadId);
    evidence.insert(QStringLiteral("command_hex"), QString::fromLatin1(command.toHex()));
    m_localEvidence.append(evidence);
    emit message(QStringLiteral("Команда сброса отправлена; ожидание пустого кадра"));
    emit changed();
    return true;
}

std::optional<quint32> IntegrationSession::kpTimeUs() const {
    if (!m_boardArmed || !m_hasBoardFrame || m_mapping.kp.index < 0) return std::nullopt;
    return timestamp(m_boardFrame.channels[size_t(m_mapping.kp.index)], m_mapping.kp);
}

QString IntegrationSession::actionResult(const PlannedAction &action) const {
    if (!m_mapping.actions.contains(action.key)) return QStringLiteral("Не сопоставлено");
    if (m_uploadState != QStringLiteral("CONFIRMED")) return QStringLiteral("Загрузка БЦВМ не подтверждена");
    if (!m_executionObserved) return QStringLiteral("Разрешение исполнения не подтверждено");
    if (m_prePermissionActivity) return QStringLiteral("Кадр с сигналом получен до разрешения исполнения");
    if (!m_boardArmed) return QStringLiteral("Плата не сброшена для этого прогона");
    const auto kp = kpTimeUs();
    if (!kp) return QStringLiteral("Ожидание КП");
    const int repetitions = std::count_if(m_plan.actions.cbegin(), m_plan.actions.cend(),
                                          [&action](const PlannedAction &other) { return other.key == action.key; });
    if (repetitions > 1) return QStringLiteral("Повтор действия: плата хранит только первый фронт");
    if (qAbs(qint64(action.offsetMs)) * 1000 >= (qint64(1) << 31))
        return QStringLiteral("Интервал счётчика неоднозначен после переполнения");
    const ActionMapping mapped = m_mapping.actions.value(action.key);
    QStringList results;
    for (const InputRef &input : mapped.inputs) {
        const auto stamp = timestamp(m_boardFrame.channels[size_t(input.index)], input);
        const int yps = input.index / 8 + 1;
        const int channel = input.index % 8 + 1;
        if (!stamp) {
            results << QStringLiteral("ЯПС%1/%2: нет отметки").arg(yps).arg(channel);
            continue;
        }
        // Both timestamps belong to the same uint32 counter; this also handles one wrap.
        // Intervals beyond half of the counter range are ambiguous and need a new protocol.
        const qint64 deltaUs = qint32(*stamp - *kp);
        const double measuredMs = double(deltaUs) / 1000.0;
        const double deviation = measuredMs - action.offsetMs;
        QString result = QStringLiteral("ЯПС%1/%2: %3 мс, Δ %4 мс")
                             .arg(yps).arg(channel).arg(measuredMs, 0, 'f', 3).arg(deviation, 0, 'f', 3);
        if (mapped.toleranceMs)
            result += qAbs(deviation) <= *mapped.toleranceMs ? QStringLiteral(" [в допуске]")
                                                             : QStringLiteral(" [вне допуска]");
        results << result;
    }
    return results.join(QStringLiteral("; "));
}

bool IntegrationSession::saveReport(const QString &path, QString *error) {
    if (path.isEmpty()) return fail(error, QStringLiteral("Не задан путь отчёта"));
    const QString framesPath = path + QStringLiteral(".frames.jsonl");
    m_frameEvidence.flush();
    if (!m_frameEvidence.seek(0)) return fail(error, QStringLiteral("Не удалось прочитать журнал кадров"));
    QSaveFile frames(framesPath);
    if (!frames.open(QIODevice::WriteOnly)) return fail(error, frames.errorString());
    while (!m_frameEvidence.atEnd()) {
        const QByteArray chunk = m_frameEvidence.read(65536);
        if (frames.write(chunk) != chunk.size()) return fail(error, frames.errorString());
    }
    if (!frames.commit()) return fail(error, frames.errorString());
    m_frameEvidence.seek(m_frameEvidence.size());

    QJsonObject report;
    report.insert(QStringLiteral("format"), QStringLiteral("pyro_stand.report.v1"));
    report.insert(QStringLiteral("saved_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    report.insert(QStringLiteral("run_id"), m_runId);
    report.insert(QStringLiteral("upload_id"), m_uploadId);
    report.insert(QStringLiteral("upload_state"), m_uploadState);
    report.insert(QStringLiteral("bcvm_lock_state"), m_lockState);
    report.insert(QStringLiteral("execution_enabled_confirmed"), m_executionEnabled);
    report.insert(QStringLiteral("execution_permission_observed"), m_executionObserved);
    report.insert(QStringLiteral("activity_before_permission"), m_prePermissionActivity);
    report.insert(QStringLiteral("bcvm_utc_at_upload"), m_bcvmUtc);
    report.insert(QStringLiteral("cyclogram_sha256"), m_cyclogramSha);
    report.insert(QStringLiteral("cyclogram_base64"), QString::fromLatin1(m_cyclogramBytes.toBase64()));
    report.insert(QStringLiteral("plan_t0_utc"), m_hasPlan ? m_plan.t0Utc.toString(Qt::ISODate) : QString());
    report.insert(QStringLiteral("mapping_version"), m_mapping.version);
    report.insert(QStringLiteral("mapping_json"), QJsonDocument::fromJson(m_mappingBytes).object());
    report.insert(QStringLiteral("kp_time_us"), kpTimeUs() ? QJsonValue(double(*kpTimeUs())) : QJsonValue());
    report.insert(QStringLiteral("accepted_frames"), QString::number(m_acceptedFrames));
    report.insert(QStringLiteral("rejected_frames"), QString::number(m_rejectedFrames));
    report.insert(QStringLiteral("frame_evidence_file"), QFileInfo(framesPath).fileName());
    report.insert(QStringLiteral("debug_evidence"), m_debugEvidence);
    report.insert(QStringLiteral("local_evidence"), m_localEvidence);
    QJsonArray actions;
    for (const auto &action : m_plan.actions) {
        QJsonObject item;
        item.insert(QStringLiteral("key"), action.key);
        item.insert(QStringLiteral("offset_ms"), action.offsetMs);
        item.insert(QStringLiteral("result"), actionResult(action));
        actions.append(item);
    }
    report.insert(QStringLiteral("actions"), actions);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    const QByteArray data = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (output.write(data) != data.size() || !output.commit()) return fail(error, output.errorString());
    return true;
}

void IntegrationSession::setError(const QString &text) {
    m_lastError = text;
    emit message(text);
    emit changed();
}
