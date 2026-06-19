#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include "types.h"
#include "stand.h"

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

private:
    void setupUI();
    void setPhase(Phase newPhase);
    void updateTimer(const QString &text, const QString &color);
    void updateNextEventTimer(const QString &text);
    void addLog(const QString &text, const QString &type = "system");
    void updateTable(const QVector<EventRow> &events);
    void updateTableRow(int row, const EventRow &data);
    void updatePhaseLabel();

    // T11: COM / БЦВМ indicators
    void updateComIndicator(bool open);
    void updateBcvmIndicator(bool reachable);

    // T12: 8-channel state dots
    void updateChannelDot(int channel, const QString &color); // channel 1-8
    void resetChannelDots();

    QLabel       *m_timerLabel     = nullptr;
    QLabel       *m_phaseLabel     = nullptr;
    QLabel       *m_nextEventLabel = nullptr;
    QTableWidget *m_table          = nullptr;
    QTextEdit    *m_logEdit        = nullptr;
    QPushButton  *m_loadBtn        = nullptr;
    QPushButton  *m_setTimeBtn     = nullptr;
    QPushButton  *m_stopBtn        = nullptr;
    QPushButton  *m_resetBtn       = nullptr;
    QLineEdit    *m_timeInput      = nullptr;

    // T11
    QLabel *m_comIndicator  = nullptr;
    QLabel *m_bcvmIndicator = nullptr;

    // T12
    QLabel *m_channelDots[8] = {};

    Stand *m_stand = nullptr;
    Phase  m_phase = Phase::Idle;
    QVector<EventRow> m_displayEvents;

    QTimer *m_blinkTimer = nullptr;
    bool    m_blinkState = false;
};

#endif // MAINWINDOW_H
