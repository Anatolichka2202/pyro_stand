#ifndef SESSION_LOGGER_H
#define SESSION_LOGGER_H

#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTime>
#include <QDir>
#include <QDateTime>

// SessionLogger — пишет сессию в logDir/session_YYYY-MM-DD_HH-MM-SS/pyro_stand.log
//
// Временна́я метка каждой записи НЕ берётся из системных часов.
// Вместо этого используется формула: SET_UTC_TIME + tickMs (абсолютный индекс из COM-потока).
// setTimeBase() должен быть вызван сразу после разбора SET_UTC_TIME из циклограммы.
//
// Не QObject, не синглтон. Потокобезопасен (QMutex).
// Реализация полностью инлайн; session_logger.cpp — только заглушка включения.

class SessionLogger
{
public:
    // Создаёт подпапку session_YYYY-MM-DD_HH-MM-SS/ внутри logDir и открывает pyro_stand.log.
    explicit SessionLogger(const QString &logDir)
    {
        const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
        const QString sessionDir = logDir + QDir::separator() + "session_" + ts;
        QDir().mkpath(sessionDir);
        m_sessionDir = sessionDir;
        m_filePath   = sessionDir + QDir::separator() + "pyro_stand.log";

        m_file.setFileName(m_filePath);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            m_stream.setDevice(&m_file);
            m_stream.setEncoding(QStringConverter::Utf8);
        }
    }

    ~SessionLogger()
    {
        if (m_file.isOpen()) {
            m_stream.flush();
            m_file.close();
        }
    }

    SessionLogger(const SessionLogger &) = delete;
    SessionLogger &operator=(const SessionLogger &) = delete;

    // Устанавливает базу времени. Вызывается после разбора SET_UTC_TIME из циклограммы.
    // tickMs=0 в COM-потоке соответствует этому времени.
    void setTimeBase(const QTime &setUtcTime)
    {
        QMutexLocker lock(&m_mutex);
        m_baseTime = setUtcTime;
        m_baseSet  = true;
    }

    // Записать строку в лог.
    // level: "INFO" | "WARN" | "ERROR" | "EVENT"
    // tickMs: абсолютный индекс из COM-потока (0 = до начала потока)
    // Метка времени = m_baseTime.addMSecs(tickMs)
    void log(const QString &level, const QString &msg, int64_t tickMs = 0)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_file.isOpen()) return;
        const QString paddedLevel = QString("%1").arg(level, -5);
        m_stream << derivedTimestamp(tickMs) << " | " << paddedLevel << " | " << msg << "\n";
        m_stream.flush();
    }

    // Записать 6-строчный заголовок файла. Вызывается один раз из Stand::loadCyclogram().
    void writeHeader(const QString &cyclogramPath,
                     const QTime   & /*setTime*/,
                     const QString &setTimeStr,
                     const QTime   & /*startTime*/,
                     const QString &startTimeStr,
                     int            trackedCount)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_file.isOpen()) return;
        m_stream << "# pyro_stand session log\n";
        m_stream << "# Session: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
        m_stream << "# Cyclogram: " << cyclogramPath << "\n";
        m_stream << "# SET_UTC_TIME: " << setTimeStr
                 << " / START_UTC_TIME: " << startTimeStr << "\n";
        m_stream << "# Tracked events: " << trackedCount << "\n";
        m_stream << "# " << QString(u'─').repeated(50) << "\n";
        m_stream.flush();
    }

    QString filePath()    const { return m_filePath; }
    QString sessionDir()  const { return m_sessionDir; }
    bool    isOpen()      const { return m_file.isOpen(); }

private:
    QString derivedTimestamp(int64_t tickMs) const
    {
        if (!m_baseSet) return "??:??:??.???";
        return m_baseTime.addMSecs(tickMs).toString("HH:mm:ss.zzz");
    }

    QFile          m_file;
    QTextStream    m_stream;
    QString        m_filePath;
    QString        m_sessionDir;
    QTime          m_baseTime;
    bool           m_baseSet  = false;
    mutable QMutex m_mutex;
};

#endif // SESSION_LOGGER_H
