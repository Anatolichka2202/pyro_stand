#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();

    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkState = !m_blinkState;
        updatePhaseLabel();
    });

    m_stand = new Stand(this);

    // ── Сигналы Stand → MainWindow ──────────────────────────────────────────
    // Все лямбды получают явный контекст (this), чтобы Qt гарантированно
    // ставил вызов в очередь GUI-треда при эмите из рабочего потока.

    connect(m_stand, &Stand::phaseChanged,    this, &MainWindow::setPhase);
    connect(m_stand, &Stand::timerUpdated,    this, &MainWindow::updateTimer);
    connect(m_stand, &Stand::nextEventTimer,  this, &MainWindow::updateNextEventTimer);
    connect(m_stand, &Stand::logMessage,      this, &MainWindow::addLog);

    connect(m_stand, &Stand::eventFired, this, [this](int eventId, int tick) {
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].firedTick = tick;
                m_displayEvents[i].status    = "ok";
                updateTableRow(i, m_displayEvents[i]);
                break;
            }
        }
    });

    connect(m_stand, &Stand::eventFailed, this, [this](int eventId) {
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].status    = "fail";
                m_displayEvents[i].firedTick = -1;
                updateTableRow(i, m_displayEvents[i]);
                break;
            }
        }
    });

    connect(m_stand, &Stand::analysisDone, this, [this](const QVector<EventRow> &events) {
        m_displayEvents.clear();
        for (const auto &e : events) { if (e.hasChannels) m_displayEvents.append(e); }
        updateTable(m_displayEvents);
    });

    connect(m_stand, &Stand::portError, this, [this](const QString &msg) {
        m_loadBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
        QMessageBox::critical(this, "Ошибка", msg);
    });

    // Загружаем циклограмму один раз — после подключения всех сигналов
    m_stand->loadCyclogram();
    setPhase(m_stand->getPhase());

    if (!m_stand->isPortOpen()) {
        m_loadBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
    }
}

MainWindow::~MainWindow() {}

// ─── setupUI ──────────────────────────────────────────────────────────────────

void MainWindow::setupUI()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(24, 16, 24, 16);

    // Верхняя панель: таймер + кнопки управления
    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->setSpacing(16);

    // Таймер
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

    // Кнопки управления
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

    m_resetBtn = new QPushButton("↺  СБРОС", this);
    m_resetBtn->setObjectName("resetBtn");
    m_resetBtn->setEnabled(false);
    connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onReset);

    ctrlLayout->addWidget(m_loadBtn);
    ctrlLayout->addWidget(m_setTimeBtn);
    ctrlLayout->addWidget(m_timeInput);
    ctrlLayout->addWidget(m_stopBtn);
    ctrlLayout->addWidget(m_resetBtn);
    topLayout->addLayout(ctrlLayout);

    mainLayout->addLayout(topLayout);

    // Таблица событий
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({"#", "СОБЫТИЕ", "КАНАЛЫ", "ТИК / МС", "СТАТУС"});
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    // Лог
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(200);
    mainLayout->addWidget(m_logEdit);
}

// ─── Управление фазой ─────────────────────────────────────────────────────────

void MainWindow::setPhase(Phase newPhase)
{
    m_phase = newPhase;

    const bool portOk = m_stand && m_stand->isPortOpen();
    const bool canLoad = (newPhase == Phase::Idle   || newPhase == Phase::Loaded ||
                          newPhase == Phase::Completed || newPhase == Phase::Stopped);
    m_loadBtn->setEnabled(canLoad && portOk);
    m_setTimeBtn->setEnabled(newPhase == Phase::Idle || newPhase == Phase::Loaded);
    m_stopBtn->setEnabled(newPhase == Phase::Countdown || newPhase == Phase::Running);
    m_resetBtn->setEnabled(newPhase == Phase::Completed || newPhase == Phase::Stopped);

    if (newPhase != Phase::Idle && newPhase != Phase::Loaded)
        m_timeInput->hide();

    QLabel *caption = findChild<QLabel*>("timerCaption");
    if (caption) {
        caption->setText((newPhase == Phase::Running || newPhase == Phase::Completed)
                             ? "ВРЕМЯ В ПОЛЁТЕ" : "ОБРАТНЫЙ ОТСЧЁТ ДО СТАРТА");
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

    updatePhaseLabel();
}

void MainWindow::updatePhaseLabel()
{
    if (!m_phaseLabel) return;

    struct { Phase p; const char* text; const char* color; bool blink; } cfg[] = {
        {Phase::Idle,      "ОЖИДАНИЕ",   "#8b949e", false},
        {Phase::Loaded,    "ЗАГРУЖЕНО",   "#58a6ff", false},
        {Phase::Countdown, "ОТСЧЁТ",     "#e3b341", true},
        {Phase::Running,   "ВЫПОЛНЕНИЕ", "#3fb950", true},
        {Phase::Completed, "ЗАВЕРШЕНО",  "#3fb950", false},
        {Phase::Stopped,   "ОСТАНОВЛЕНО","#f85149", false},
    };

    const char* text  = "";
    const char* color = "#8b949e";
    bool blink = false;
    for (const auto &c : cfg) {
        if (c.p == m_phase) { text = c.text; color = c.color; blink = c.blink; break; }
    }

    if (blink && !m_blinkTimer->isActive()) { m_blinkTimer->start(); m_blinkState = true; }
    else if (!blink && m_blinkTimer->isActive()) { m_blinkTimer->stop(); m_blinkState = true; }

    m_phaseLabel->setText(QString("● %1").arg(text));
    m_phaseLabel->setStyleSheet(QString("font-size: 10px; letter-spacing: 0.15em; color: %1;")
                                    .arg((blink && !m_blinkState) ? "transparent" : color));
}

// ─── UI helpers ───────────────────────────────────────────────────────────────

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
    QString color = "#c8d0dc";
    if      (type == "system")     color = "#8b949e";
    else if (type == "event")      color = "#c8d0dc";
    else if (type == "event-post") color = "#f85149";
    m_logEdit->append(QString("<span style=\"color:%1;\">%2</span>").arg(color, text.toHtmlEscaped()));
}

void MainWindow::updateTable(const QVector<EventRow> &events)
{
    m_displayEvents = events;
    m_table->setRowCount(m_displayEvents.size());
    for (int i = 0; i < m_displayEvents.size(); ++i)
        updateTableRow(i, m_displayEvents[i]);
}

void MainWindow::updateTableRow(int row, const EventRow &data)
{
    if (row < 0 || row >= m_table->rowCount()) return;

    auto setItem = [this, row](int col, const QString &text, Qt::AlignmentFlag align = Qt::AlignLeft) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(align | Qt::AlignVCenter);
        m_table->setItem(row, col, item);
    };

    setItem(0, QString::number(data.id), Qt::AlignCenter);
    setItem(1, data.description.isEmpty() ? data.key : data.description);
    setItem(2, data.channels, Qt::AlignCenter);

    // Колонка "ТИК / МС"
    const bool final = (m_phase == Phase::Completed || m_phase == Phase::Stopped);
    if (final)
        setItem(3, data.calculatedMs != -1 ? QString::number(data.calculatedMs) + " мс" : "—", Qt::AlignRight);
    else
        setItem(3, data.firedTick != -1 ? QString::number(data.firedTick) : "—", Qt::AlignRight);

    // Колонка "СТАТУС"
    QString statusText;
    QColor  statusColor = Qt::gray;
    if (final) {
        if (data.status == "ok") {
            if (data.deviationMs <= 0)    { statusText = "✓ ОК";                                      statusColor = Qt::green; }
            else if (data.deviationMs <=5){ statusText = QString("✓ ОК (±%1 мс)").arg(data.deviationMs); statusColor = Qt::yellow; }
            else                          { statusText = QString("✗ НЕ ОК (±%1 мс)").arg(data.deviationMs); statusColor = Qt::red; }
        } else if (data.status == "fail") { statusText = "✗ НЕ СРАБОТАЛО"; statusColor = Qt::red; }
        else                              { statusText = "—"; }
    } else {
        if      (data.status == "ok")   { statusText = "выполнено";     statusColor = Qt::green; }
        else if (data.status == "fail") { statusText = "не сработало";  statusColor = Qt::red; }
        else                            { statusText = "—"; }
    }
    auto *statusItem = new QTableWidgetItem(statusText);
    statusItem->setForeground(statusColor);
    statusItem->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
    m_table->setItem(row, 4, statusItem);
}

// ─── Слоты кнопок ─────────────────────────────────────────────────────────────

void MainWindow::onLoadToBoard() { m_stand->sendToBoard(); }

void MainWindow::onSetTime()
{
    if (m_timeInput->isVisible()) {
        const QTime t = QTime::fromString(m_timeInput->text(), "hh:mm:ss");
        if (t.isValid()) { m_timeInput->hide(); m_stand->setStartTimeFromUI(t); }
    } else {
        m_timeInput->show();
        m_timeInput->setText(m_stand->getStartTime().toString("hh:mm:ss"));
        m_timeInput->setFocus();
    }
}

void MainWindow::onStop()  { m_stand->stop(); }

void MainWindow::onReset()
{
    m_stand->resetForNewTest();
    m_stand->loadCyclogram();
    setPhase(Phase::Loaded);
}
