#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTimeEdit>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QFrame>
#include <QWidget>
#include <memory>
#include "types.h"
#include "stand.h"
#include "timeline_widget.h"
#include "session_logger.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onLoadToBoard();
    void onSetTime();
    void onStop();
    void onReset();
    void onExportCsv();

private:
    void setupUI();
    void setPhase(Phase newPhase);
    void updateTimer(const TimerState &state);
    void updateNextEventTimer(const NextEventInfo &info);
    void addLog(const QString &text, const QString &type = "system");
    void updateTable(const QVector<EventRow> &events);
    void updateTableRow(int row, const EventRow &data);
    void updatePhaseLabel();
    void refreshNextEventHighlight();

    // T11: COM / БЦВМ indicators
    void updateComIndicator(bool open);
    void updateBcvmIndicator(bool reachable);

    // T12: 8-channel chips
    void updateChannelDot(int channel, const QString &color); // channel 1-8
    void resetChannelDots();

    // T17: summary strip
    void updateSummaryStrip(const QVector<EventRow> &events);

    QLabel       *m_timerLabel     = nullptr;
    QLabel       *m_phaseLabel     = nullptr;
    QLabel       *m_nextEventLabel = nullptr;
    QTableWidget *m_table          = nullptr;
    QTextEdit    *m_logEdit        = nullptr;
    QPushButton  *m_loadBtn        = nullptr;
    QPushButton  *m_setTimeBtn     = nullptr;
    QPushButton  *m_stopBtn        = nullptr;
    QPushButton  *m_resetBtn       = nullptr;
    QTimeEdit    *m_timeInput      = nullptr;
    QLabel       *m_startTimeLabel = nullptr;  // shows current start time inline

    // T11: COM + БЦВМ as chip-style widgets
    QFrame *m_comChip    = nullptr;
    QLabel *m_comDot     = nullptr;
    QLabel *m_comStatus  = nullptr;
    QFrame *m_bcvmChip   = nullptr;
    QLabel *m_bcvmDot    = nullptr;
    QLabel *m_bcvmStatus = nullptr;

    // T12: channel chip widgets (dot inside a styled card)
    struct ChannelChip {
        QFrame *frame = nullptr;
        QLabel *dot   = nullptr;
    };
    ChannelChip m_channelChips[8];

    // T17
    QWidget     *m_summaryStrip = nullptr;
    QLabel      *m_summaryLabel = nullptr;
    QPushButton *m_exportCsvBtn = nullptr;

    // T18: timeline
    TimelineWidget *m_timeline = nullptr;

    // T16: m_stand destroyed before m_logger (reverse declaration order)
    std::unique_ptr<SessionLogger> m_logger;
    std::unique_ptr<Stand>         m_stand;
    Phase  m_phase = Phase::Idle;
    QVector<EventRow> m_displayEvents;

    QTimer *m_blinkTimer = nullptr;
    bool    m_blinkState = false;
    int     m_nextEventRow = -1;
};

#endif // MAINWINDOW_H
