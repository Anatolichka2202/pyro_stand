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

// SessionLogger — записывает сессию в файл pyro_YYYY-MM-DD_HH-MM-SS.log
// рядом с исполняемым файлом (logDir = QCoreApplication::applicationDirPath()).
//
// Не является синглтоном и не наследует QObject (не нужны сигналы/слоты).
// Создаётся в MainWindow и передаётся в Stand*.
// Все вызовы потокобезопасны (QMutex).
// Реализация полностью инлайн — не требует отдельного session_logger.cpp
// в тестовых целях (Stand вызывает logger только если logger != nullptr).

class SessionLogger
{
public:
    // logDir — каталог, куда записывается лог
    explicit SessionLogger(const QString &logDir)
    {
        const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
        m_filePath = logDir + QDir::separator() + "pyro_" + ts + ".log";

        m_file.setFileName(m_filePath);
        if (m_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            m_stream.setDevice(&m_file);
            m_stream.setEncoding(QStringConverter::Utf8);
        }
        // Если файл не открылся — все дальнейшие вызовы log() молча игнорируются
    }

    ~SessionLogger()
    {
        if (m_file.isOpen()) {
            m_stream.flush();
            m_file.close();
        }
    }

    // Не копируемый — владеет файловым дескриптором
    SessionLogger(const SessionLogger &) = delete;
    SessionLogger &operator=(const SessionLogger &) = delete;

    // Записать строку в лог.
    // level: "INFO" | "WARN" | "ERROR" | "EVENT"
    // Формат строки: 2026-06-19T10:00:22.134 | INFO  | сообщение
    void log(const QString &level, const QString &msg)
    {
        QMutexLocker lock(&m_mutex);
        if (!m_file.isOpen()) return;

        // Выравниваем level до 5 символов: INFO , WARN , ERROR, EVENT
        const QString paddedLevel = QString("%1").arg(level, -5);
        m_stream << timestampNow() << " | " << paddedLevel << " | " << msg << "\n";
        m_stream.flush();
    }

    // Записать 6-строчный заголовок файла.
    // Вызывается один раз из Stand::loadCyclogram().
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
        m_stream << "# Started: " << timestampNow() << "\n";
        m_stream << "# Cyclogram: " << cyclogramPath << "\n";
        m_stream << "# SET_UTC_TIME: " << setTimeStr
                 << " / START_UTC_TIME: " << startTimeStr << "\n";
        m_stream << "# Tracked events: " << trackedCount << "\n";
        m_stream << "# " << QString(u'─').repeated(50) << "\n";
        m_stream.flush();
    }

    QString filePath() const { return m_filePath; }
    bool    isOpen()   const { return m_file.isOpen(); }

private:
    QString timestampNow() const
    {
        return QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss.zzz");
    }

    QFile          m_file;
    QTextStream    m_stream;
    QString        m_filePath;
    mutable QMutex m_mutex;
};

#endif // SESSION_LOGGER_H
