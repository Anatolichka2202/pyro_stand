#pragma once

#include <QMainWindow>
#include <QTextEdit>
#include <QTimeEdit>
#include <QPushButton>
#include "stand.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    void onStart();
    void onStop();
    void onLogMessage(const QString& msg);

private:
    QTextEdit* logWidget;
    QTimeEdit* startTimeEdit;
    QPushButton* startBtn;
    QPushButton* stopBtn;
    Stand* stand;
};
