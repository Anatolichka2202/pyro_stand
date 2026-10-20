#pragma once

#include "integration.h"

#include <QHash>
#include <QSet>
#include <QLabel>
#include <QMainWindow>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include "event_canvas.h"

class TimelineWidget;
class QPushButton;
class QDialog;
class QComboBox;

// Операторское окно показывает события и результаты, полученные от
// IntegrationSession; декодирование и оценка измерений принадлежат сессии.
class IntegrationWindow final : public QMainWindow {
    friend class OperatorGuiTest;
    Q_OBJECT
public:
    explicit IntegrationWindow(IntegrationSession::Options options, QWidget *parent = nullptr);

private:
    void setupUi();
    void refreshCanvas();
    void refreshEventTable();
    void configurePeer();
    IntegrationSession &clockSession();
    void refresh();
    void updatePlannedClock();
    void appendMessage(const QString &text);
    void configureTester();
    void refreshLive();
    void openFreeRecording();
    void appendJournal(const QString &level, const QString &message, const QString &color);

    IntegrationSession m_session;
    std::unique_ptr<IntegrationSession> m_peer;
    EventCanvas *m_canvas = nullptr;
    QLabel *m_firstClock = nullptr;
    QLabel *m_clockSource = nullptr;
    QLabel *m_peerStatus = nullptr;
    QElapsedTimer m_visualElapsed;
    std::optional<double> m_visualAnchor;
    QString m_visualCollection;
    std::optional<double> m_forecastPosition;
    double m_lastVisualPosition = 0;
    QTimer m_refreshTimer;
    QTimer m_clockTimer;

    // Состояния живых входов не зависят от циклограммы и допуска.
    QDialog *m_liveDialog = nullptr;
    QComboBox *m_liveSource = nullptr;
    QTableWidget *m_liveTable = nullptr;
    QLabel *m_liveStatus = nullptr;
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
    QLabel *m_streamStatus = nullptr;
    QLabel *m_eventCaption = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QPushButton *m_resetButton = nullptr;

    TimelineWidget *m_timeline = nullptr;
    QTableWidget *m_eventTable = nullptr;
    QTableWidget *m_planTable = nullptr;
    QTextEdit *m_eventLog = nullptr;
    QTextEdit *m_recentLog = nullptr;
    QSet<QString> m_expandedEvents;

    QString m_lastUploadId;
    QString m_timelineSignature;
    QHash<QString, QString> m_loggedStates;
};
