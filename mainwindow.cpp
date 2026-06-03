#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    QWidget* central = new QWidget;
    setCentralWidget(central);
    QVBoxLayout* layout = new QVBoxLayout(central);

    // Панель управления: только время и большая кнопка
    QHBoxLayout* top = new QHBoxLayout;
    startTimeEdit = new QTimeEdit;
    startTimeEdit->setTime(QTime::currentTime().addSecs(10));
    startTimeEdit->setDisplayFormat("hh:mm:ss");
    startTimeEdit->setMinimumWidth(120);
    startBtn = new QPushButton("ПУСК");
    startBtn->setMinimumHeight(40);
    startBtn->setMinimumWidth(120);
    stopBtn = new QPushButton("СТОП");
    stopBtn->setEnabled(false);
    top->addWidget(new QLabel("Время старта (МСК):"));
    top->addWidget(startTimeEdit);
    top->addWidget(startBtn);
    top->addWidget(stopBtn);
    layout->addLayout(top);

    logWidget = new QTextEdit;
    logWidget->setReadOnly(true);
    layout->addWidget(logWidget);

    stand = new Stand(this);
    connect(startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);
    connect(stand, &Stand::logMessage, this, &MainWindow::onLogMessage);
}

MainWindow::~MainWindow() {}

void MainWindow::onStart() {
    stand->setStartMskTime(startTimeEdit->time());
    if (stand->start()) {
        startBtn->setEnabled(false);
        stopBtn->setEnabled(true);
    }
}

void MainWindow::onStop() {
    stand->stop();
    startBtn->setEnabled(true);
    stopBtn->setEnabled(false);
}

void MainWindow::onLogMessage(const QString& msg) {
    logWidget->append(msg);
}
