#pragma once

#include "integration.h"

#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>

class TimelineWidget;
class QPushButton;

// Operator HMI based on the original summer layout; data comes from
// IntegrationSession, never from the legacy Stand/serial transfer path.
class IntegrationWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit IntegrationWindow(IntegrationSession::Options options, QWidget *parent = nullptr);

private:
    void setupUi();
    void refresh();
    void updatePlannedClock();
    void appendMessage(const QString &text);
    void configureTester();

    IntegrationSession m_session;
    QTimer m_refreshTimer;
    QTimer m_clockTimer;

    QLabel *m_clockCaption = nullptr;
    QLabel *m_timerLabel = nullptr;
    QLabel *m_phaseLabel = nullptr;
    QLabel *m_nextEventLabel = nullptr;
    QLabel *m_startTimeLabel = nullptr;
    QLabel *m_debugDot = nullptr;
    QLabel *m_debugStatus = nullptr;
    QLabel *m_boardDot = nullptr;
    QLabel *m_boardStatus = nullptr;
    QLabel *m_bcvmDot = nullptr;
    QLabel *m_bcvmStatus = nullptr;
    QLabel *m_uploadStatus = nullptr;
    QLabel *m_kpStatus = nullptr;
    QLabel *m_mappingStatus = nullptr;
    QLabel *m_clockStatus = nullptr;
    QLabel *m_eventCaption = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_resetButton = nullptr;

    TimelineWidget *m_timeline = nullptr;
    QTableWidget *m_eventTable = nullptr;
    QTextEdit *m_eventLog = nullptr;
    QTextEdit *m_log = nullptr;

    QString m_lastUploadId;
    QString m_timelineSignature;
    QHash<QString, QString> m_loggedStates;
};
