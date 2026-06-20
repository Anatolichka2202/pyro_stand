#include "mainwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDateTime>

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

    // T11: БЦВМ indicator — parse logMessage for known strings
    connect(m_stand, &Stand::logMessage, this, [this](const QString &msg, const QString &) {
        if (msg.contains("БЦВМ недоступна"))
            updateBcvmIndicator(false);
        if (msg.contains("Циклограмма отправлена"))
            updateBcvmIndicator(true);
    });

    // T11: COM indicator — go red on portError
    connect(m_stand, &Stand::portError, this, [this](const QString &msg) {
        updateComIndicator(false);
        m_loadBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
        QMessageBox::critical(this, "Ошибка", msg);
    });

    connect(m_stand, &Stand::eventFired, this, [this](int eventId, int tick) {
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].firedTick = tick;
                m_displayEvents[i].status    = "ok";
                updateTableRow(i, m_displayEvents[i]);

                // T12: paint channels green for this event
                for (const QString &part : m_displayEvents[i].channels.split(',')) {
                    bool ok = false;
                    int c = part.trimmed().toInt(&ok);
                    if (ok && c >= 1 && c <= 8)
                        updateChannelDot(c, "#3fb950");
                }

                // T18: mark on timeline + advance playhead to fired tick
                m_timeline->markEventFired(eventId, "ok");
                m_timeline->setPlayheadMs(tick);
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

                // T12: paint channels red for this event
                for (const QString &part : m_displayEvents[i].channels.split(',')) {
                    bool ok = false;
                    int c = part.trimmed().toInt(&ok);
                    if (ok && c >= 1 && c <= 8)
                        updateChannelDot(c, "#f85149");
                }

                // T18: mark on timeline
                m_timeline->markEventFired(eventId, "fail");
                break;
            }
        }
    });

    connect(m_stand, &Stand::analysisDone, this, [this](const QVector<EventRow> &events) {
        m_displayEvents.clear();
        for (const auto &e : events) { if (e.hasChannels) m_displayEvents.append(e); }
        updateTable(m_displayEvents);
        resetChannelDots();
        // T17: show summary strip
        updateSummaryStrip(m_displayEvents);
        m_summaryStrip->setVisible(true);
        // T18: refresh timeline with final analysis results (includes deviation data)
        m_timeline->setEvents(events);
    });

    // Загружаем циклограмму один раз — после подключения всех сигналов
    m_stand->loadCyclogram();
    // T18: populate timeline from initially loaded cyclogram
    m_timeline->setEvents(m_stand->getEvents());
    setPhase(m_stand->getPhase());

    // T11: set initial COM indicator state
    updateComIndicator(m_stand->isPortOpen());

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

    // ── Панель управления (правая колонка) ───────────────────────────────────
    // Общая структура: статусы сверху, затем две группы кнопок
    static const auto makeGroupLabel = [](const QString &text, QWidget *parent) {
        auto *lbl = new QLabel(text, parent);
        lbl->setStyleSheet(
            "font-size: 9px; letter-spacing: 0.15em; color: #8b949e;"
            "text-transform: uppercase; margin-bottom: 2px;");
        return lbl;
    };
    static const auto makeSeparator = [](QWidget *parent) {
        auto *sep = new QFrame(parent);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: #30363d;");
        sep->setFixedHeight(1);
        return sep;
    };

    // Контейнер панели со стилем карточки
    QWidget *ctrlCard = new QWidget(this);
    ctrlCard->setStyleSheet(
        "QWidget {"
        "  background: #161b22;"
        "  border: 1px solid #30363d;"
        "  border-radius: 6px;"
        "}"
        // Стиль всех кнопок внутри карточки
        "QPushButton {"
        "  background: #0d1117;"
        "  border: 1px solid #30363d;"
        "  border-radius: 4px;"
        "  color: #e6edf3;"
        "  padding: 7px 12px;"
        "  font-size: 12px;"
        "  text-align: left;"
        "}"
        "QPushButton:hover  { border-color: #58a6ff; background: #1c2330; }"
        "QPushButton:disabled { opacity: 0.4; }"
        "QPushButton#stopBtn { border-color: #30363d; color: #f85149; }"
        "QPushButton#stopBtn:hover { border-color: #f85149; background: #2d1010; }"
    );

    QVBoxLayout *ctrlLayout = new QVBoxLayout(ctrlCard);
    ctrlLayout->setSpacing(6);
    ctrlLayout->setContentsMargins(12, 10, 12, 10);

    // ── Статусы COM / БЦВМ (вверху, крупно) ─────────────────────────────────
    QHBoxLayout *statusRow = new QHBoxLayout();
    statusRow->setSpacing(12);

    m_comIndicator = new QLabel("● COM7", this);
    m_comIndicator->setStyleSheet(
        "font-size: 12px; font-weight: 600; letter-spacing: 0.05em;"
        "color: #8b949e; background: transparent; border: none;");

    m_bcvmIndicator = new QLabel("● БЦВМ", this);
    m_bcvmIndicator->setStyleSheet(
        "font-size: 12px; font-weight: 600; letter-spacing: 0.05em;"
        "color: #8b949e; background: transparent; border: none;");

    statusRow->addWidget(m_comIndicator);
    statusRow->addWidget(m_bcvmIndicator);
    statusRow->addStretch();
    ctrlLayout->addLayout(statusRow);
    ctrlLayout->addWidget(makeSeparator(ctrlCard));

    // ── Группа «Подготовка» ──────────────────────────────────────────────────
    ctrlLayout->addWidget(makeGroupLabel("Подготовка", ctrlCard));

    m_loadBtn = new QPushButton("▤  ЗАГРУЗИТЬ НА БОРТ", ctrlCard);
    m_loadBtn->setObjectName("loadBtn");
    connect(m_loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadToBoard);
    ctrlLayout->addWidget(m_loadBtn);

    m_setTimeBtn = new QPushButton("⏱  УСТАНОВИТЬ ВРЕМЯ СТАРТА", ctrlCard);
    m_setTimeBtn->setObjectName("setTimeBtn");
    connect(m_setTimeBtn, &QPushButton::clicked, this, &MainWindow::onSetTime);
    ctrlLayout->addWidget(m_setTimeBtn);

    m_timeInput = new QTimeEdit(ctrlCard);
    m_timeInput->setDisplayFormat("hh:mm:ss");
    m_timeInput->setTime(QTime(0, 0, 10));
    m_timeInput->setStyleSheet(
        "QTimeEdit {"
        "  background: #0d1117; color: #c9d1d9;"
        "  border: 1px solid #30363d; border-radius: 4px;"
        "  padding: 4px 8px; font-size: 13px;"
        "}"
        "QTimeEdit::up-button, QTimeEdit::down-button {"
        "  background: #21262d; border: none; width: 16px;"
        "}"
        "QTimeEdit::up-arrow   { image: none; border-left: 4px solid transparent;"
        "  border-right: 4px solid transparent; border-bottom: 5px solid #8b949e; }"
        "QTimeEdit::down-arrow { image: none; border-left: 4px solid transparent;"
        "  border-right: 4px solid transparent; border-top: 5px solid #8b949e; }"
    );
    m_timeInput->hide();
    ctrlLayout->addWidget(m_timeInput);

    ctrlLayout->addWidget(makeSeparator(ctrlCard));

    // ── Группа «Управление» ──────────────────────────────────────────────────
    ctrlLayout->addWidget(makeGroupLabel("Управление", ctrlCard));

    m_stopBtn = new QPushButton("■  СТОП", ctrlCard);
    m_stopBtn->setObjectName("stopBtn");
    m_stopBtn->setEnabled(false);
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);
    ctrlLayout->addWidget(m_stopBtn);

    m_resetBtn = new QPushButton("↺  СБРОС", ctrlCard);
    m_resetBtn->setObjectName("resetBtn");
    m_resetBtn->setEnabled(false);
    connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onReset);
    ctrlLayout->addWidget(m_resetBtn);

    ctrlLayout->addStretch();
    topLayout->addWidget(ctrlCard);

    mainLayout->addLayout(topLayout);

    // T12: 8-channel state dots panel
    QWidget *channelPanel = new QWidget(this);
    QVBoxLayout *channelVBox = new QVBoxLayout(channelPanel);
    channelVBox->setSpacing(4);
    channelVBox->setContentsMargins(0, 0, 0, 0);

    QLabel *channelCaption = new QLabel("КАНАЛЫ", this);
    channelCaption->setStyleSheet("font-size: 10px; letter-spacing: 0.2em; color: #8b949e;");
    channelVBox->addWidget(channelCaption);

    QHBoxLayout *dotsLayout = new QHBoxLayout();
    dotsLayout->setSpacing(12);
    dotsLayout->setContentsMargins(0, 0, 0, 0);

    for (int i = 0; i < 8; ++i) {
        QVBoxLayout *dotBox = new QVBoxLayout();
        dotBox->setSpacing(2);
        dotBox->setAlignment(Qt::AlignHCenter);

        QLabel *label = new QLabel(QString("CH%1").arg(i + 1), this);
        label->setStyleSheet("font-size: 9px; color: #8b949e; letter-spacing: 0.05em;");
        label->setAlignment(Qt::AlignHCenter);

        m_channelDots[i] = new QLabel("●", this);
        m_channelDots[i]->setStyleSheet("font-size: 18px; color: #484f58;");
        m_channelDots[i]->setAlignment(Qt::AlignHCenter);

        dotBox->addWidget(label);
        dotBox->addWidget(m_channelDots[i]);
        dotsLayout->addLayout(dotBox);
    }
    dotsLayout->addStretch();
    channelVBox->addLayout(dotsLayout);

    mainLayout->addWidget(channelPanel);

    // T18: горизонтальная шкала времени
    m_timeline = new TimelineWidget(this);
    mainLayout->addWidget(m_timeline);

    // Таблица событий
    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({"#", "СОБЫТИЕ", "КАНАЛЫ", "ПЛАН МС", "ФАКТ МС", "ОТКЛ МС", "СТАТУС"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QTableWidget::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table);

    // T17: Summary strip (hidden until analysisDone)
    m_summaryStrip = new QWidget(this);
    m_summaryStrip->setStyleSheet(
        "QWidget {"
        "  background-color: #161b22;"
        "  border-top: 1px solid #30363d;"
        "}"
    );
    m_summaryStrip->setVisible(false);

    QHBoxLayout *summaryLayout = new QHBoxLayout(m_summaryStrip);
    summaryLayout->setContentsMargins(12, 6, 12, 6);
    summaryLayout->setSpacing(16);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("font-size: 12px; color: #e6edf3; background: transparent; border: none;");

    m_exportCsvBtn = new QPushButton("Экспорт CSV", this);
    m_exportCsvBtn->setObjectName("exportCsvBtn");
    m_exportCsvBtn->setStyleSheet(
        "QPushButton {"
        "  background: #21262d;"
        "  color: #c9d1d9;"
        "  border: 1px solid #30363d;"
        "  border-radius: 4px;"
        "  padding: 4px 12px;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover { background: #30363d; }"
        "QPushButton:pressed { background: #161b22; }"
    );
    connect(m_exportCsvBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);

    summaryLayout->addWidget(m_summaryLabel, 1);
    summaryLayout->addWidget(m_exportCsvBtn, 0);

    mainLayout->addWidget(m_summaryStrip);

    // Лог — двухколоночная таблица: время | сообщение
    m_logTable = new QTableWidget(0, 2, this);
    m_logTable->setHorizontalHeaderLabels({"ВРЕМЯ", "СООБЩЕНИЕ"});
    m_logTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_logTable->horizontalHeader()->setStyleSheet(
        "QHeaderView::section {"
        "  background: #161b22; color: #8b949e;"
        "  border: none; border-bottom: 1px solid #30363d;"
        "  font-size: 9px; letter-spacing: 0.15em; padding: 4px 8px;"
        "}"
    );
    m_logTable->verticalHeader()->hide();
    m_logTable->setShowGrid(false);
    m_logTable->setEditTriggers(QTableWidget::NoEditTriggers);
    m_logTable->setSelectionMode(QTableWidget::NoSelection);
    m_logTable->setFocusPolicy(Qt::NoFocus);
    m_logTable->setMaximumHeight(180);
    m_logTable->setStyleSheet(
        "QTableWidget {"
        "  background: #0d1117; border: 1px solid #30363d;"
        "  border-radius: 4px; font-size: 12px;"
        "}"
        "QTableWidget::item { padding: 3px 8px; border: none; }"
        "QScrollBar:vertical { background: #0d1117; width: 6px; }"
        "QScrollBar::handle:vertical { background: #30363d; border-radius: 3px; }"
    );
    mainLayout->addWidget(m_logTable);
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

    // T12: при переходе в Running — окрасить отслеживаемые каналы в жёлтый
    if (newPhase == Phase::Running) {
        // Сначала сбросить все в серый, потом пометить ожидаемые жёлтым
        resetChannelDots();
        for (const auto &e : m_displayEvents) {
            if (e.hasChannels && (e.status.isEmpty() || e.status == "pending")) {
                for (const QString &part : e.channels.split(',')) {
                    bool ok = false;
                    int c = part.trimmed().toInt(&ok);
                    if (ok && c >= 1 && c <= 8)
                        updateChannelDot(c, "#e3b341");
                }
            }
        }
    } else if (newPhase == Phase::Idle || newPhase == Phase::Loaded) {
        resetChannelDots();
        // T17: hide summary strip when returning to idle/loaded
        if (m_summaryStrip)
            m_summaryStrip->setVisible(false);
    }

    updatePhaseLabel();
    refreshNextEventHighlight();
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

// ─── T11: Индикаторы статуса ──────────────────────────────────────────────────

void MainWindow::updateComIndicator(bool open)
{
    if (!m_comIndicator) return;
    const QString color = open ? "#3fb950" : "#f85149";
    m_comIndicator->setStyleSheet(
        QString("font-size: 10px; letter-spacing: 0.1em; color: %1;").arg(color));
}

void MainWindow::updateBcvmIndicator(bool reachable)
{
    if (!m_bcvmIndicator) return;
    const QString color = reachable ? "#3fb950" : "#f85149";
    m_bcvmIndicator->setStyleSheet(
        QString("font-size: 10px; letter-spacing: 0.1em; color: %1;").arg(color));
}

// ─── T12: Точки каналов ───────────────────────────────────────────────────────

void MainWindow::updateChannelDot(int channel, const QString &color)
{
    if (channel < 1 || channel > 8) return;
    if (!m_channelDots[channel - 1]) return;
    m_channelDots[channel - 1]->setStyleSheet(
        QString("font-size: 18px; color: %1;").arg(color));
}

void MainWindow::resetChannelDots()
{
    for (int i = 0; i < 8; ++i) {
        if (m_channelDots[i])
            m_channelDots[i]->setStyleSheet("font-size: 18px; color: #484f58;");
    }
}

// ─── UI helpers ───────────────────────────────────────────────────────────────

void MainWindow::refreshNextEventHighlight()
{
    // Снять подсветку со всех строк
    for (int i = 0; i < m_table->rowCount(); ++i)
        for (int j = 0; j < m_table->columnCount(); ++j)
            if (auto *item = m_table->item(i, j))
                item->setBackground(QColor());

    if (m_phase != Phase::Countdown && m_phase != Phase::Running) return;

    // Найти первое pending событие с каналами
    for (int i = 0; i < m_displayEvents.size(); ++i) {
        if (m_displayEvents[i].status == "pending" && m_displayEvents[i].hasChannels) {
            for (int j = 0; j < m_table->columnCount(); ++j)
                if (auto *item = m_table->item(i, j))
                    item->setBackground(QColor(28, 45, 58)); // тёмно-синий
            m_nextEventRow = i;
            break;
        }
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
    // Цвет и левая полоска-индикатор уровня
    QString textColor  = "#c8d0dc";
    QString stripColor = "#58a6ff"; // info = синий
    if (type == "system") {
        textColor  = "#8b949e";
        stripColor = "#30363d";
    } else if (type == "event") {
        textColor  = "#3fb950";
        stripColor = "#3fb950";
    } else if (type == "event-post") {
        textColor  = "#f85149";
        stripColor = "#f85149";
    }

    const int row = m_logTable->rowCount();
    m_logTable->insertRow(row);

    // Колонка 0: временная метка
    const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
    auto *tsItem = new QTableWidgetItem(ts);
    tsItem->setForeground(QColor("#8b949e"));
    tsItem->setFont(QFont("JetBrains Mono, monospace", 10));
    m_logTable->setItem(row, 0, tsItem);

    // Колонка 1: сообщение с цветовым индикатором слева
    auto *msgItem = new QTableWidgetItem(text);
    msgItem->setForeground(QColor(textColor));
    m_logTable->setItem(row, 1, msgItem);

    // Фон строки — лёгкий тинт для ok/fail
    if (type == "event") {
        QColor bg(0x3f, 0xb9, 0x50, 18);
        tsItem->setBackground(bg);
        msgItem->setBackground(bg);
    } else if (type == "event-post") {
        QColor bg(0xf8, 0x51, 0x49, 18);
        tsItem->setBackground(bg);
        msgItem->setBackground(bg);
    }

    m_logTable->scrollToBottom();
    (void)stripColor; // используется только для будущего delegate
}

void MainWindow::updateTable(const QVector<EventRow> &events)
{
    m_displayEvents = events;
    m_table->setRowCount(m_displayEvents.size());
    for (int i = 0; i < m_displayEvents.size(); ++i)
        updateTableRow(i, m_displayEvents[i]);
    refreshNextEventHighlight();
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

    // Колонка 3: "ПЛАН МС" — плановое время всегда отображается
    {
        auto *planItem = new QTableWidgetItem(QString::number(data.time_ms));
        planItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        planItem->setForeground(QColor("#8b949e"));
        m_table->setItem(row, 3, planItem);
    }

    const bool final = (m_phase == Phase::Completed || m_phase == Phase::Stopped);

    // Колонка 4: "ФАКТ МС"
    {
        QString factText;
        bool hasFact = false;
        if (data.calculatedMs != -1) {
            factText = QString::number(data.calculatedMs) + " мс";
            hasFact = true;
        } else if (!final && data.firedTick != -1) {
            factText = QString::number(data.firedTick);
            hasFact = true;
        } else {
            factText = "—";
        }
        auto *factItem = new QTableWidgetItem(factText);
        factItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (hasFact)
            factItem->setForeground(QColor("#e6edf3"));
        else
            factItem->setForeground(QColor("#8b949e"));
        m_table->setItem(row, 4, factItem);
    }

    // Определяем статус и цвет для колонок 5 и 6
    QString statusText;
    QColor  statusColor = QColor("#8b949e");
    if (final) {
        if (data.status == "ok") {
            if (data.deviationMs <= 0)     { statusText = "✓ ОК";                                          statusColor = QColor("#3fb950"); }
            else if (data.deviationMs <= 5){ statusText = QString("✓ ОК (±%1 мс)").arg(data.deviationMs);  statusColor = QColor("#e3b341"); }
            else                           { statusText = QString("✗ НЕ ОК (±%1 мс)").arg(data.deviationMs); statusColor = QColor("#f85149"); }
        } else if (data.status == "fail")  { statusText = "✗ НЕ СРАБОТАЛО"; statusColor = QColor("#f85149"); }
        else                               { statusText = "—"; }
    } else {
        if      (data.status == "ok")   { statusText = "выполнено";    statusColor = QColor("#3fb950"); }
        else if (data.status == "fail") { statusText = "не сработало"; statusColor = QColor("#f85149"); }
        else                            { statusText = "—"; }
    }

    // Колонка 5: "ОТКЛ МС"
    {
        QString devText;
        QColor  devColor = QColor("#8b949e");
        if (final && data.status == "ok") {
            devText  = QString::number(data.deviationMs) + " мс";
            devColor = statusColor;
        } else {
            devText = "—";
        }
        auto *devItem = new QTableWidgetItem(devText);
        devItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        devItem->setForeground(devColor);
        m_table->setItem(row, 5, devItem);
    }

    // Колонка 6: "СТАТУС"
    auto *statusItem = new QTableWidgetItem(statusText);
    statusItem->setForeground(statusColor);
    statusItem->setTextAlignment(Qt::AlignCenter | Qt::AlignVCenter);
    m_table->setItem(row, 6, statusItem);

    refreshNextEventHighlight();
}

// ─── Слоты кнопок ─────────────────────────────────────────────────────────────

void MainWindow::onLoadToBoard() { m_stand->sendToBoard(); }

void MainWindow::onSetTime()
{
    if (m_timeInput->isVisible()) {
        const QTime t = m_timeInput->time();
        m_timeInput->hide();
        m_stand->setStartTimeFromUI(t);
    } else {
        m_timeInput->setTime(m_stand->getStartTime());
        m_timeInput->show();
        m_timeInput->setFocus();
    }
}

void MainWindow::onStop()  { m_stand->stop(); }

void MainWindow::onReset()
{
    m_stand->resetForNewTest();
    m_stand->loadCyclogram();
    // T18: reset timeline colors and playhead, then reload events
    m_timeline->reset();
    m_timeline->setEvents(m_stand->getEvents());
    setPhase(Phase::Loaded);
}

// ─── T17: Сводная полоса и CSV-экспорт ───────────────────────────────────────

void MainWindow::updateSummaryStrip(const QVector<EventRow> &events)
{
    if (!m_summaryLabel) return;

    int okCount   = 0;
    int failCount = 0;
    int maxDev    = 0;

    for (const auto &e : events) {
        if (e.status == "ok") {
            ++okCount;
            if (e.deviationMs > maxDev)
                maxDev = e.deviationMs;
        } else if (e.status == "fail") {
            ++failCount;
        }
    }

    // Build rich-text label: coloured counts + max deviation
    const QString okPart   = QString("<span style=\"color:#3fb950;\">%1 ✓</span>").arg(okCount);
    const QString failPart = QString("<span style=\"color:#f85149;\">%2 ✗</span>").arg(failCount);
    const QString devPart  = QString("Макс. откл.: %1 мс").arg(maxDev);

    m_summaryLabel->setText(
        QString("<span style=\"color:#e6edf3;\">Результат: %1 / %2 &nbsp;&middot;&nbsp; %3</span>")
            .arg(okPart, failPart, devPart)
    );
}

void MainWindow::onExportCsv()
{
    const QString defaultName =
        QString("pyro_result_%1.csv")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm"));

    const QString path = QFileDialog::getSaveFileName(
        this,
        "Экспорт в CSV",
        defaultName,
        "CSV (*.csv)"
    );
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Ошибка",
                              "Не удалось открыть файл:\n" + path);
        return;
    }

    QTextStream out(&file);
    // UTF-8 BOM for Excel compatibility
    out.setEncoding(QStringConverter::Utf8);
    out << "\xEF\xBB\xBF";

    // Header
    out << "#,Событие,Каналы,"
           "План (мс),Факт (мс),"
           "Откл. (мс),Статус\n";

    // Helper: quote a field if it contains a comma
    auto csvField = [](const QString &s) -> QString {
        if (s.contains(','))
            return "\"" + s + "\"";
        return s;
    };

    for (int i = 0; i < m_displayEvents.size(); ++i) {
        const EventRow &e = m_displayEvents[i];

        const QString factStr = (e.calculatedMs != -1) ? QString::number(e.calculatedMs) : QString();
        const QString devStr  = (e.deviationMs  != -1 && e.status == "ok")
                                    ? QString::number(e.deviationMs) : QString();

        out << (i + 1) << ","
            << csvField(e.key) << ","
            << csvField(e.channels) << ","
            << e.time_ms << ","
            << factStr << ","
            << devStr << ","
            << e.status << "\n";
    }

    file.close();
    addLog(QString("Экспорт CSV: %1").arg(path), "system");
}
