#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
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
    // Production: creates Stand + SessionLogger internally.
    // portOverride: serial device path; empty = DEFAULT_SERIAL_PORT from platform.h
    // logDir: log file directory; empty = applicationDirPath() with home fallback
    explicit MainWindow(const QString &portOverride = {},
                        const QString &logDir = {},
                        QWidget *parent = nullptr);

    // Test/demo: caller provides pre-configured Stand (cyclogram not yet loaded).
    // cyclogramPath: path to .ini file; empty = QCoreApplication::applicationDirPath()
    explicit MainWindow(std::unique_ptr<Stand> stand,
                        const QString &cyclogramPath,
                        QWidget *parent = nullptr);

    ~MainWindow();

private slots:
    void onLoadToBoard();
    void onSetTime();
    void onStop();
    void onReset();
    void onExportCsv();

private:
    void setupUI();
    void connectStand();              // wires all Stand signals → slots/lambdas
    void finalizeInit(const QString &cyclogramPath); // loadCyclogram + initial UI sync

    void setPhase(Phase newPhase);
    void updateTimer(const TimerState &state);
    void updateNextEventTimer(const NextEventInfo &info);
    void addLog(const QString &text, const QString &type = "system");
    void updateTable(const QVector<EventRow> &events);
    void updateTableRow(int row, const EventRow &data);
    void setSubRow(int row, int channel, int calcMs, int planMs, bool isLast, bool hasSpread);
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
    QTextEdit    *m_eventLog       = nullptr;  // техлог срабатываний (правая панель)
    QPushButton  *m_loadBtn        = nullptr;
    QPushButton  *m_setTimeBtn     = nullptr;
    QPushButton  *m_stopBtn        = nullptr;
    QPushButton  *m_resetBtn       = nullptr;
    QComboBox    *m_transferCombo  = nullptr;
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

    QLabel *m_transferStatusLabel = nullptr;
    QTimer *m_transferStatusTimer = nullptr;

    QWidget *m_channelPanel  = nullptr;  // hidden; keep for future use
    QFrame  *m_progressArea  = nullptr;  // upload progress (occupies channels/timeline space)
    QLabel  *m_progressLabel = nullptr;

    QTimer *m_blinkTimer   = nullptr;
    bool    m_blinkState   = false;
    int     m_nextEventRow = -1;

    int     m_streamTick   = 0;  // counts timerTick calls; used for right-log byte reception display
    QTime        m_t0ActualTime;   // фактическое время T0 (SET_UTC_TIME + syncIndex)
    QTime        m_plannedT0Time;  // плановое время T0 (START_UTC_TIME из циклограммы)
    QVector<int> m_eventToRow;     // m_displayEvents[i] → номер строки таблицы (с учётом подстрок)
};

#endif // MAINWINDOW_H
