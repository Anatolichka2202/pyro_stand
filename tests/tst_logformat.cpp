#include <QtTest/QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include "../session_logger.h"

// Тесты формата трейс-лога:
// - создаётся подпапка session_*/
// - метки времени = SET_UTC_TIME + tick (не системные часы)
// - заголовок записан корректно
// - файл лога открыт и закрывается при разрушении объекта

class TstLogFormat : public QObject
{
    Q_OBJECT

private slots:

    void sessionFolderCreated()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        {
            SessionLogger logger(tmpDir.path());
            QVERIFY(logger.isOpen());

            // Подпапка session_* должна существовать
            QDir dir(tmpDir.path());
            const QStringList entries = dir.entryList({"session_*"}, QDir::Dirs);
            QCOMPARE(entries.size(), 1);

            // Файл лога внутри подпапки
            QVERIFY(logger.filePath().startsWith(tmpDir.path() + QDir::separator() + "session_"));
            QVERIFY(logger.filePath().endsWith("pyro_stand.log"));
        }
    }

    void timestampDerivedFromSetUtcTime()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(10, 0, 0)); // SET_UTC_TIME = 10:00:00

            // tick=30500 → 10:00:00 + 30500 мс = 10:00:30.500
            logger.log("INFO", "тест события", 30500);
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        QVERIFY2(content.contains("10:00:30.500"),
                 qPrintable("Ожидали '10:00:30.500' в логе, получили:\n" + content));
    }

    void timestampZeroTickEqualsSetTime()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(10, 0, 0));

            // tick=0 → ровно 10:00:00.000
            logger.log("INFO", "до потока", 0);
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        QVERIFY2(content.contains("10:00:00.000"),
                 qPrintable("Ожидали '10:00:00.000' в логе, получили:\n" + content));
    }

    void timestampBeforeBaseShowsPlaceholder()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            // setTimeBase НЕ вызывается
            logger.log("INFO", "без базы");
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        QVERIFY2(content.contains("??:??:??.???"),
                 qPrintable("Ожидали плейсхолдер '??:??:??.???' в логе, получили:\n" + content));
    }

    void headerContainsSetAndStartTime()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(10, 0, 0));
            logger.writeHeader("/path/to/cyclogram.ini",
                               QTime(10, 0, 0), "10:00:00",
                               QTime(10, 0, 30), "10:00:30",
                               3);
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        QVERIFY(content.contains("# pyro_stand session log"));
        QVERIFY(content.contains("SET_UTC_TIME: 10:00:00"));
        QVERIFY(content.contains("START_UTC_TIME: 10:00:30"));
        QVERIFY(content.contains("Tracked events: 3"));
        QVERIFY(content.contains("cyclogram.ini"));
    }

    void logLevelPaddedToFiveChars()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(9, 0, 0));
            logger.log("INFO",  "информация",   0);
            logger.log("ERROR", "ошибка",       0);
            logger.log("EVENT", "событие",      0);
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        // "INFO " (5 символов) и "ERROR" (5 символов)
        QVERIFY(content.contains("INFO  |"));
        QVERIFY(content.contains("ERROR |"));
        QVERIFY(content.contains("EVENT |"));
    }

    void multipleTicksDeriveCorrectly()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(10, 0, 0));
            logger.log("EVENT", "зажигание",  22000); // 10:00:22.000
            logger.log("EVENT", "отрыв",      30000); // 10:00:30.000
            logger.log("EVENT", "закрытие",   50000); // 10:00:50.000
            logPath = logger.filePath();
        }

        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QTextStream(&f).readAll();

        QVERIFY(content.contains("10:00:22.000"));
        QVERIFY(content.contains("10:00:30.000"));
        QVERIFY(content.contains("10:00:50.000"));
    }

    void fileClosedAfterDestruction()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QString logPath;
        {
            SessionLogger logger(tmpDir.path());
            logger.setTimeBase(QTime(10, 0, 0));
            logger.log("INFO", "сообщение", 0);
            logPath = logger.filePath();
            // деструктор вызывается здесь
        }

        // После деструктора файл должен быть доступен для чтения (закрыт корректно)
        QFile f(logPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(!QTextStream(&f).readAll().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstLogFormat)
#include "tst_logformat.moc"
