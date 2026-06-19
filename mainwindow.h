#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextEdit>
#include "types.h"
#include "stand.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onLoadToBoard();   // "ЗАГРУЗИТЬ НА БОРТ"
    void onSetTime();
    void onStop();

private:
    void setupUI();
    void resetState();
    void setPhase(Phase newPhase);
    void updateTimer(const QString &text, const QString &color);
    void updateNextEventTimer(const QString &text);
    void addLog(const QString &text, const QString &type = "system");
    void updateTable(const QVector<EventRow> &events);
    void updateTableRow(int row, const EventRow &data);

    QLabel *m_timerLabel;
    QLabel *m_phaseLabel;
    QLabel *m_nextEventLabel;
    QTableWidget *m_table;
    QTextEdit *m_logEdit;
    QPushButton *m_loadBtn;
    QPushButton *m_setTimeBtn;
    QPushButton *m_stopBtn;
    QLineEdit *m_timeInput;

    Phase m_phase = Phase::Idle;
    QVector<EventRow> m_events;
    int m_logId = 0;
    QVector<EventRow> m_displayEvents;
    Stand *m_stand;

    QTimer *m_blinkTimer;
    bool m_blinkState;
};

#endif // MAINWINDOW_H
