#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();

    // Сначала создаём Stand, чтобы он был доступен в resetState
    m_stand = new Stand(this);


    // Подключение сигналов
    connect(m_stand, &Stand::phaseChanged, this, &MainWindow::setPhase);

    connect(m_stand, &Stand::timerUpdated, this, &MainWindow::updateTimer);

    connect(m_stand, &Stand::nextEventTimer, this, &MainWindow::updateNextEventTimer);

    connect(m_stand, &Stand::eventFired, [this](int eventId, int tick){
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].firedTick = tick;
                m_displayEvents[i].status = "ok";
                updateTableRow(i, m_displayEvents[i]);
                break;
            }
        }
    });

    connect(m_stand, &Stand::analysisDone, [this](const QVector<EventRow> &events){
        m_displayEvents.clear();
        for (const auto &e : events) {
            if (e.hasChannels) m_displayEvents.append(e);
        }
        updateTable(m_displayEvents);
    });

    connect(m_stand, &Stand::logMessage, this, &MainWindow::addLog);

    connect(m_stand, &Stand::portError, [this](const QString &msg){
        m_loadBtn->setEnabled(false);
        QMessageBox::critical(this, "Ошибка", msg);
    });

    connect(m_stand, &Stand::eventFailed, [this](int eventId){
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].status = "fail";
                m_displayEvents[i].firedTick = -1;
                updateTableRow(i, m_displayEvents[i]);
                break;
            }
        }
    });

    // Теперь загружаем циклограмму (сигналы уже подключены)
    m_stand->loadCyclogram();

    // Установить UI в соответствии с текущим состоянием стенда
    setPhase(m_stand->getPhase());

    // Если порт не открыт, блокируем кнопку загрузки
    if (!m_stand->isPortOpen()) {
        m_loadBtn->setEnabled(false);
    }

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkState = !m_blinkState;
        // обновить стиль точки в зависимости от фазы и состояния
        setPhase(m_phase); // или отдельный метод обновления цвета
    });
}

MainWindow::~MainWindow() {}

void MainWindow::setupUI()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(24, 16, 24, 16);

    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->setSpacing(16);

    QVBoxLayout *timerLayout = new QVBoxLayout();
    QLabel *timerCaption = new QLabel("ОБРАТНЫЙ ОТСЧЁТ ДО СТАРТА", this);
    timerCaption->setObjectName("timerCaption");
    timerCaption->setStyleSheet("font-size: 10px; letter-spacing: 0.2em; color: #8b949e;");
    m_timerLabel = new QLabel("--:--", this);
    m_timerLabel->setStyleSheet("font-size: 44px; font-weight: 600; color: #e3b341; font-family: 'JetBrains Mono';");
    m_phaseLabel = new QLabel("● ОЖИДАНИЕ", this);
    m_phaseLabel->setStyleSheet("font-size: 10px; letter-spacing: 0.15em; color: #8b949e;");
    m_nextEventLabel = new QLabel("До события: --:--", this);
    m_nextEventLabel->setStyleSheet("font-size: 12px; color: #8b949e;");
    timerLayout->addWidget(timerCaption);
    timerLayout->addWidget(m_timerLabel);
    timerLayout->addWidget(m_phaseLabel);
    timerLayout->addWidget(m_nextEventLabel);
    topLayout->addLayout(timerLayout, 1);

    QVBoxLayout *ctrlLayout = new QVBoxLayout();
    ctrlLayout->setSpacing(8);

    m_loadBtn = new QPushButton("▤  ЗАГРУЗИТЬ НА БОРТ", this);
    m_loadBtn->setObjectName("loadBtn");
    connect(m_loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadToBoard);

    m_setTimeBtn = new QPushButton("⏱  УСТАНОВИТЬ ВРЕМЯ СТАРТА", this);
    m_setTimeBtn->setObjectName("setTimeBtn");
    connect(m_setTimeBtn, &QPushButton::clicked, this, &MainWindow::onSetTime);

    m_timeInput = new QLineEdit(this);
    m_timeInput->setPlaceholderText("ЧЧ:ММ:СС");
    m_timeInput->setInputMask("00:00:00");
    m_timeInput->setText("00:00:10");
    m_timeInput->hide();

    m_stopBtn = new QPushButton("■  СТОП", this);
    m_stopBtn->setObjectName("stopBtn");
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    ctrlLayout->addWidget(m_loadBtn);
    ctrlLayout->addWidget(m_setTimeBtn);
    ctrlLayout->addWidget(m_timeInput);
    ctrlLayout->addWidget(m_stopBtn);
    topLayout->addLayout(ctrlLayout);

    mainLayout->addLayout(topLayout);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({"#", "СОБЫТИЕ", "КАНАЛЫ", "ТИК / МС", "СТАТУС"});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(200);
    mainLayout->addWidget(m_logEdit);
}

void MainWindow::resetState()
{
    m_phase = Phase::Idle;
    m_events.clear();
    m_logId = 0;
    m_table->setRowCount(0);
    m_logEdit->clear();
    setPhase(Phase::Idle);
}

void MainWindow::setPhase(Phase newPhase)
{
    m_phase = newPhase;
    bool blink = (newPhase == Phase::Countdown || newPhase == Phase::Running);

    if (blink && !m_blinkTimer->isActive()) {
        m_blinkTimer->start();
        m_blinkState = true;
    } else if (!blink && m_blinkTimer->isActive()) {
        m_blinkTimer->stop();
        m_blinkState = true; // всегда видимый
    }

    QString phaseText, phaseColor;
    switch (newPhase) {
    case Phase::Idle:       phaseText = "ОЖИДАНИЕ";   phaseColor = "#8b949e"; break;
    case Phase::Loaded:     phaseText = "ЗАГРУЖЕНО";   phaseColor = "#58a6ff"; break;
    case Phase::Countdown:  phaseText = "ОТСЧЁТ";     phaseColor = "#e3b341"; blink = true; break;
    case Phase::Running:    phaseText = "ВЫПОЛНЕНИЕ";  phaseColor = "#3fb950"; blink = true; break;
    case Phase::Completed:  phaseText = "ЗАВЕРШЕНО";   phaseColor = "#3fb950"; break;
    case Phase::Stopped:    phaseText = "ОСТАНОВЛЕНО"; phaseColor = "#f85149"; break;
    }
    m_phaseLabel->setText(QString("● %1").arg(phaseText));
    m_phaseLabel->setStyleSheet(QString("font-size: 10px; letter-spacing: 0.15em; color: %1;")
                                    .arg(phaseColor)); //TODO мигание

    bool loadEnabled = (newPhase == Phase::Idle || newPhase == Phase::Loaded ||
                        newPhase == Phase::Completed || newPhase == Phase::Stopped);
    bool setTimeEnabled = (newPhase == Phase::Idle);
    bool stopEnabled = (newPhase == Phase::Countdown || newPhase == Phase::Running);

    m_loadBtn->setEnabled(loadEnabled && (m_stand ? m_stand->isPortOpen() : false));
    m_setTimeBtn->setEnabled(setTimeEnabled);
    m_stopBtn->setEnabled(stopEnabled);

    if (newPhase != Phase::Idle)
        m_timeInput->hide();

    QLabel *caption = findChild<QLabel*>("timerCaption");
    if (caption) {
        if (newPhase == Phase::Running || newPhase == Phase::Completed) {
            caption->setText("ВРЕМЯ В ПОЛЁТЕ");
        } else {
            caption->setText("ОБРАТНЫЙ ОТСЧЁТ ДО СТАРТА");
        }
    }

    if (newPhase == Phase::Completed) {
        m_timerLabel->setText("ПОЛЁТНОЕ\nЗАДАНИЕ\nОТРАБОТАНО");
        m_timerLabel->setStyleSheet("font-size: 24px; font-weight: 600; color: #3fb950; font-family: 'JetBrains Mono'; line-height: 1.2;");
        m_nextEventLabel->setText("");
    } else if (newPhase == Phase::Stopped) {
        updateTimer("СТОП", "#f85149");
        m_nextEventLabel->setText("");
    } else if (newPhase != Phase::Countdown && newPhase != Phase::Running) {
        updateTimer("--:--", "#484f58");
        m_nextEventLabel->setText("До события: --:--");
    }
}

void MainWindow::updateTimer(const QString &text, const QString &color)
{
    m_timerLabel->setText(text);
    m_timerLabel->setStyleSheet(QString("font-size: 44px; font-weight: 600; color: %1; font-family: 'JetBrains Mono';").arg(color));
}

void MainWindow::updateNextEventTimer(const QString &text)
{
    m_nextEventLabel->setText("До события: " + text);
}

void MainWindow::addLog(const QString &text, const QString &type)
{
    m_logId++;
    QString color;
    if (type == "system")      color = "#8b949e";
    else if (type == "event")  color = "#c8d0dc";
    else if (type == "event-post") color = "#f85149";
    else                       color = "#c8d0dc";
    m_logEdit->append(QString("<span style=\"color:%1;\">%2</span>").arg(color).arg(text.toHtmlEscaped()));
}

void MainWindow::updateTable(const QVector<EventRow> &events)
{
    m_displayEvents = events; // теперь храним только отслеживаемые
    m_table->setRowCount(m_displayEvents.size());
    for (int i = 0; i < m_displayEvents.size(); ++i) {
        updateTableRow(i, m_displayEvents[i]);
    }
}
void MainWindow::updateTableRow(int row, const EventRow &data)
{
    if (row < 0 || row >= m_table->rowCount()) return;

    // Столбец 0: ID
    m_table->setItem(row, 0, new QTableWidgetItem(QString::number(data.id)));

    // Столбец 1: СОБЫТИЕ – используем описание (комментарий)
    QString displayName = data.description.isEmpty() ? data.key : data.description;
    m_table->setItem(row, 1, new QTableWidgetItem(displayName));

    // Столбец 2: КАНАЛЫ
    m_table->setItem(row, 2, new QTableWidgetItem(data.channels));

    // Столбец 3: ТИК / МС
    QString tickText;
    if (m_phase == Phase::Completed || m_phase == Phase::Stopped) {
        if (data.calculatedMs != -1) {
            tickText = QString::number(data.calculatedMs) + " мс";
        } else {
            tickText = "—";
        }
    } else {
        if (data.firedTick != -1) {
            tickText = QString::number(data.firedTick);
        } else {
            tickText = "—";
        }
    }
    m_table->setItem(row, 3, new QTableWidgetItem(tickText));

    // Столбец 4: СТАТУС
    QString statusText;
    QColor statusColor;
    if (m_phase == Phase::Completed || m_phase == Phase::Stopped) {
        if (data.status == "ok") {
            if (data.deviationMs == 0) {
                statusText = "✓ ОК";
                statusColor = Qt::green;
            } else if (data.deviationMs <= 5) {
                statusText = QString("✓ ОК (откл. ±%1 мс)").arg(data.deviationMs);
                statusColor = Qt::yellow;
            } else {
                statusText = QString("✗ НЕ ОК (откл. ±%1 мс)").arg(data.deviationMs);
                statusColor = Qt::red;
            }
        } else if (data.status == "fail") {
            statusText = "✗ НЕ СРАБОТАЛО";
            statusColor = Qt::red;
        } else {
            statusText = "—";
            statusColor = Qt::gray;
        }
    } else {
        // Во время выполнения
        if (data.status == "ok") {
            statusText = "выполнено";
            statusColor = Qt::green;
        } else if (data.status == "fail") {
            statusText = "не сработало";
            statusColor = Qt::red;
        } else {
            statusText = "—";
            statusColor = Qt::gray;
        }
    }
    auto *statusItem = new QTableWidgetItem(statusText);
    statusItem->setForeground(statusColor);
    statusItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 4, statusItem);

    // Выравнивание остальных столбцов
    m_table->item(row, 0)->setTextAlignment(Qt::AlignCenter);
    m_table->item(row, 2)->setTextAlignment(Qt::AlignCenter);
    m_table->item(row, 3)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

void MainWindow::onLoadToBoard()
{
    m_stand->sendToBoard();
}

void MainWindow::onSetTime()
{
    if (m_timeInput->isVisible()) {
        QTime t = QTime::fromString(m_timeInput->text(), "hh:mm:ss");
        if (t.isValid()) {
            m_timeInput->hide();
            if (m_stand->setStartTimeFromUI(t)) {
                if (m_stand->getPhase() == Phase::Loaded) {
                    // Не запускаем автоматически, пользователь нажимает "ЗАГРУЗИТЬ НА БОРТ"
                }
            }
        }
    } else {
        m_timeInput->show();
        m_timeInput->setFocus();
    }
}

void MainWindow::onStop()
{
    m_stand->stop();
}
