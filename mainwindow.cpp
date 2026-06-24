#include "mainwindow.h"
#include "platform.h"
#include <QVBoxLayout>
#include <QStandardPaths>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include <QSplitter>
#include <QScrollBar>

// ─── Production constructor ───────────────────────────────────────────────────

MainWindow::MainWindow(const QString &portOverride, const QString &logDir, QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();

    // Resolve log directory: prefer explicit arg, then applicationDirPath if writable,
    // then home dir as last resort (relevant on Linux with system installs).
    QString resolvedLogDir = logDir;
    if (resolvedLogDir.isEmpty()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        resolvedLogDir = QFileInfo(appDir).isWritable()
                             ? appDir
                             : QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }

    // T16: logger создаётся первым, stand — вторым.
    // Деструкторы: m_stand (join worker thread) → m_logger (close file).
    m_logger = std::make_unique<SessionLogger>(resolvedLogDir);

    const QString port = portOverride.isEmpty() ? QString(DEFAULT_SERIAL_PORT) : portOverride;
    m_stand  = std::make_unique<Stand>(nullptr, port, nullptr, m_logger.get());

    connectStand();
    finalizeInit({});
}

// ─── Test / demo constructor ──────────────────────────────────────────────────

MainWindow::MainWindow(std::unique_ptr<Stand> stand,
                       const QString &cyclogramPath,
                       QWidget *parent)
    : QMainWindow(parent)
{
    setupUI();
    m_stand = std::move(stand); // m_logger stays nullptr → no file logging
    connectStand();
    finalizeInit(cyclogramPath);
}

// ─── connectStand ─────────────────────────────────────────────────────────────

void MainWindow::connectStand()
{
    m_blinkTimer = new QTimer(this);
    m_blinkTimer->setInterval(500);
    connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkState = !m_blinkState;
        updatePhaseLabel();
    });


    connect(m_stand.get(), &Stand::phaseChanged,    this, &MainWindow::setPhase);
    connect(m_stand.get(), &Stand::timerTick,        this, &MainWindow::updateTimer);
    connect(m_stand.get(), &Stand::nextEventChanged, this, &MainWindow::updateNextEventTimer);
    connect(m_stand.get(), &Stand::logMessage,       this, &MainWindow::addLog);

    // Статус загрузки циклограммы
    m_transferStatusTimer = new QTimer(this);
    m_transferStatusTimer->setSingleShot(true);
    connect(m_transferStatusTimer, &QTimer::timeout, this, [this]() {
        m_transferStatusLabel->setVisible(false);
    });
    connect(m_stand.get(), &Stand::transferProgress, this, [this](const QString &stage) {
        m_transferStatusLabel->setVisible(true);
        if (stage == "checking") {
            m_transferStatusLabel->setStyleSheet(
                "font-size: 11px; padding: 3px 0; color: #8b949e; background: transparent;");
            m_transferStatusLabel->setText("Проверка БЦВМ...");
            m_transferStatusTimer->stop();
        } else if (stage == "sending") {
            m_transferStatusLabel->setStyleSheet(
                "font-size: 11px; padding: 3px 0; color: #e3b341; background: transparent;");
            m_transferStatusLabel->setText("Загрузка циклограммы...");
            m_transferStatusTimer->stop();
        } else if (stage == "done") {
            m_transferStatusLabel->setStyleSheet(
                "font-size: 11px; padding: 3px 0; color: #3fb950; background: transparent;");
            m_transferStatusLabel->setText("Циклограмма загружена ✓");
            m_transferStatusTimer->start(5000);
        } else if (stage == "error") {
            m_transferStatusLabel->setStyleSheet(
                "font-size: 11px; padding: 3px 0; color: #f85149; background: transparent;");
            m_transferStatusLabel->setText("Ошибка загрузки");
            m_transferStatusTimer->start(8000);
        }
    });

    // T11: БЦВМ indicator
    connect(m_stand.get(), &Stand::logMessage, this, [this](const QString &msg, const QString &) {
        if (msg.contains("БЦВМ недоступна"))
            updateBcvmIndicator(false);
        if (msg.contains("Циклограмма отправлена"))
            updateBcvmIndicator(true);
    });

    // T11: COM indicator
    connect(m_stand.get(), &Stand::portError, this, [this](const QString &msg) {
        updateComIndicator(false);
        m_loadBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
        QMessageBox::critical(this, "Ошибка", msg);
    });

    connect(m_stand.get(), &Stand::eventFired, this, [this](int eventId, int tick) {
        for (int i = 0; i < m_displayEvents.size(); ++i) {
            if (m_displayEvents[i].id == eventId) {
                m_displayEvents[i].firedTick = tick;
                // Статус ("ok" / "late") определяется после анализа, не здесь.
                // В таблице показываем только сырой тик до финала.
                updateTableRow((i < m_eventToRow.size()) ? m_eventToRow[i] : i, m_displayEvents[i]);
                for (const QString &part : m_displayEvents[i].channels.split(',')) {
                    bool ok = false; int c = part.trimmed().toInt(&ok);
                    if (ok && c >= 1 && c <= 8) updateChannelDot(c, "#3fb950");
                }
                break;
            }
        }
    });

    connect(m_stand.get(), &Stand::analysisDone, this, [this](const QVector<EventRow> &events) {
        // Фактическое время T0 = SET_UTC_TIME + syncIndex (аппаратный тик КП)
        m_t0ActualTime  = m_stand->getSetTime().addMSecs(m_stand->getSyncIndex());
        // Плановое время T0 = START_UTC_TIME из циклограммы
        m_plannedT0Time = m_stand->getStartTime();

        m_displayEvents.clear();
        for (const auto &e : events) { if (e.hasChannels) m_displayEvents.append(e); }
        updateTable(m_displayEvents);
        resetChannelDots();
        updateSummaryStrip(m_displayEvents);
        m_summaryStrip->setVisible(true);
        m_timeline->setEvents(events);
    });
}

// ─── finalizeInit ─────────────────────────────────────────────────────────────

void MainWindow::finalizeInit(const QString &cyclogramPath)
{
    m_stand->loadCyclogram(cyclogramPath);
    m_timeline->setEvents(m_stand->getEvents());
    setPhase(m_stand->getPhase());
    updateComIndicator(m_stand->isPortOpen());
    if (!m_stand->isPortOpen()) {
        m_loadBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
    }
}

MainWindow::~MainWindow() {}

// ─── Helper: chip stylesheet ────────────────────────────────────────────────

static QString chipStyle(const QString &border, const QString &bg = "#161b22")
{
    return QString("QFrame { background: %1; border: 1px solid %2; border-radius: 5px; }").arg(bg, border);
}

static QString dotStyle(const QString &color)
{
    return QString("background: %1; border-radius: 5px; border: none;").arg(color);
}

// ─── Helper: build a chip widget ────────────────────────────────────────────

static QFrame *makeStatusChip(QWidget *parent, const QString &title,
                               QLabel **dotOut, QLabel **statusOut)
{
    QFrame *chip = new QFrame(parent);
    chip->setStyleSheet(chipStyle("#30363d"));
    chip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    QHBoxLayout *lay = new QHBoxLayout(chip);
    lay->setContentsMargins(8, 6, 10, 6);
    lay->setSpacing(8);

    QLabel *dot = new QLabel(chip);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(dotStyle("#484f58"));

    QVBoxLayout *textCol = new QVBoxLayout();
    textCol->setSpacing(1);
    QLabel *name = new QLabel(title, chip);
    name->setStyleSheet("font-size: 12px; font-weight: 600; color: #e6edf3; background: transparent; border: none;");

    QLabel *status = new QLabel("нет связи", chip);
    status->setStyleSheet("font-size: 11px; color: #8b949e; background: transparent; border: none;");

    textCol->addWidget(name);
    textCol->addWidget(status);

    lay->addWidget(dot);
    lay->addLayout(textCol);

    if (dotOut)    *dotOut    = dot;
    if (statusOut) *statusOut = status;
    return chip;
}

// ─── setupUI ────────────────────────────────────────────────────────────────

void MainWindow::setupUI()
{
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setStyleSheet(
        "QSplitter::handle { background: #21262d; width: 3px; }");
    setCentralWidget(splitter);

    // Левая панель — весь основной контент
    QWidget *leftPanel = new QWidget;
    QVBoxLayout *mainLayout = new QVBoxLayout(leftPanel);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(20, 14, 10, 14);
    splitter->addWidget(leftPanel);

    // ── TOP ROW: timer (left, flex) + controls (right, fixed 300px) ──────────
    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setSpacing(16);

    // Left: timer block
    QVBoxLayout *timerCol = new QVBoxLayout();
    timerCol->setSpacing(4);

    QLabel *timerCaption = new QLabel("ДО ЗАПУСКА", this);
    timerCaption->setObjectName("timerCaption");
    timerCaption->setStyleSheet("font-size: 13px; letter-spacing: 0.2em; color: #8b949e; text-transform: uppercase;");

    m_timerLabel = new QLabel("--:--", this);
    m_timerLabel->setObjectName("timerLabel");
    m_timerLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_timerLabel->setStyleSheet(
        "font-size: 64px; font-weight: 700; color: #30363d; font-family: 'JetBrains Mono'; letter-spacing: 0.04em;");

    QHBoxLayout *phaseLine = new QHBoxLayout();
    phaseLine->setSpacing(14);
    m_phaseLabel = new QLabel("● ОЖИДАНИЕ", this);
    m_phaseLabel->setObjectName("phaseLabel");
    m_phaseLabel->setStyleSheet("font-size: 13px; font-weight: 600; letter-spacing: 0.15em; color: #8b949e;");
    m_nextEventLabel = new QLabel("До события: --:--", this);
    m_nextEventLabel->setObjectName("nextEventLabel");
    m_nextEventLabel->setStyleSheet("font-size: 13px; color: #6e7681;");
    phaseLine->addWidget(m_phaseLabel);
    phaseLine->addWidget(m_nextEventLabel);
    phaseLine->addStretch();

    timerCol->addStretch(1);           // вертикальное центрирование: отступ сверху
    timerCol->addWidget(timerCaption);
    timerCol->addWidget(m_timerLabel);
    timerCol->addLayout(phaseLine);
    timerCol->addStretch(1);           // отступ снизу
    topRow->addLayout(timerCol, 1);

    // Right: control card — адаптивная ширина
    QWidget *ctrlCard = new QWidget(this);
    ctrlCard->setMinimumWidth(240);
    ctrlCard->setMaximumWidth(360);
    ctrlCard->setStyleSheet(
        "QWidget#ctrlCard { background: #161b22; border: 1px solid #30363d; border-radius: 6px; }");
    ctrlCard->setObjectName("ctrlCard");

    QVBoxLayout *ctrlCardLayout = new QVBoxLayout(ctrlCard);
    ctrlCardLayout->setSpacing(8);
    ctrlCardLayout->setContentsMargins(10, 10, 10, 10);

    // COM + БЦВМ chips row
    QHBoxLayout *connRow = new QHBoxLayout();
    connRow->setSpacing(8);
    m_comChip  = makeStatusChip(ctrlCard, "COM7",  &m_comDot,  &m_comStatus);
    m_bcvmChip = makeStatusChip(ctrlCard, "БЦВМ",  &m_bcvmDot, &m_bcvmStatus);
    connRow->addWidget(m_comChip);
    connRow->addWidget(m_bcvmChip);
    ctrlCardLayout->addLayout(connRow);

    // Primary button: ЗАГРУЗИТЬ НА БОРТ
    m_loadBtn = new QPushButton("▤  ЗАГРУЗИТЬ НА БОРТ", ctrlCard);
    m_loadBtn->setObjectName("loadBtn");
    m_loadBtn->setStyleSheet(
        "QPushButton {"
        "  background: #1f6feb;"
        "  border: 1px solid #1f6feb;"
        "  color: #ffffff;"
        "  border-radius: 5px;"
        "  padding: 9px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "  text-align: left;"
        "}"
        "QPushButton:hover { background: #388bfd; border-color: #388bfd; }"
        "QPushButton:disabled {"
        "  background: #21262d;"
        "  border-color: #30363d;"
        "  color: #484f58;"
        "}"
    );
    connect(m_loadBtn, &QPushButton::clicked, this, &MainWindow::onLoadToBoard);
    ctrlCardLayout->addWidget(m_loadBtn);

    m_transferStatusLabel = new QLabel("", ctrlCard);
    m_transferStatusLabel->setObjectName("transferStatusLabel");
    m_transferStatusLabel->setAlignment(Qt::AlignCenter);
    m_transferStatusLabel->setStyleSheet(
        "font-size: 11px; padding: 3px 0; color: #8b949e; background: transparent;");
    m_transferStatusLabel->setVisible(false);
    ctrlCardLayout->addWidget(m_transferStatusLabel);

    // Выбор протокола передачи циклограммы
    m_transferCombo = new QComboBox(ctrlCard);
    m_transferCombo->addItem("UDP",  static_cast<int>(TransferMode::UDP));
    m_transferCombo->addItem("TFTP", static_cast<int>(TransferMode::TFTP));
    m_transferCombo->setStyleSheet(
        "QComboBox {"
        "  background: #0d1117; border: 1px solid #30363d; border-radius: 5px;"
        "  color: #8b949e; font-size: 11px; padding: 4px 8px;"
        "}"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox QAbstractItemView { background: #161b22; color: #c9d1d9; border: 1px solid #30363d; }");
    connect(m_transferCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (m_stand)
            m_stand->setTransferMode(static_cast<TransferMode>(m_transferCombo->itemData(idx).toInt()));
    });
    ctrlCardLayout->addWidget(m_transferCombo);

    // Time display + СТОП row
    QHBoxLayout *timeStopRow = new QHBoxLayout();
    timeStopRow->setSpacing(8);

    // Time display (clickable → opens time edit)
    QWidget *timeBox = new QWidget(ctrlCard);
    timeBox->setStyleSheet(
        "background: #0d1117; border: 1px solid #30363d; border-radius: 5px;");
    QHBoxLayout *timeBoxLayout = new QHBoxLayout(timeBox);
    timeBoxLayout->setContentsMargins(8, 7, 8, 7);
    timeBoxLayout->setSpacing(6);
    QLabel *startLabel = new QLabel("СТАРТ", timeBox);
    startLabel->setStyleSheet("font-size: 11px; color: #6e7681; background: transparent; border: none;");
    m_startTimeLabel = new QLabel("--:--:--", timeBox);
    m_startTimeLabel->setStyleSheet(
        "font-size: 13px; color: #8b949e; font-family: 'JetBrains Mono'; background: transparent; border: none;");
    timeBoxLayout->addWidget(startLabel);
    timeBoxLayout->addWidget(m_startTimeLabel, 1);

    // СТОП button (shown during countdown/running)
    m_stopBtn = new QPushButton("■  СТОП", ctrlCard);
    m_stopBtn->setObjectName("stopBtn");
    m_stopBtn->setEnabled(false);
    m_stopBtn->setStyleSheet(
        "QPushButton {"
        "  background: rgba(248,81,73,0.12);"
        "  border: 1px solid #f85149;"
        "  color: #ff7b72;"
        "  border-radius: 5px;"
        "  padding: 9px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: rgba(248,81,73,0.22); }"
        "QPushButton:disabled {"
        "  background: #21262d;"
        "  border-color: #30363d;"
        "  color: #484f58;"
        "}"
    );
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::onStop);

    // СБРОС button (shown after completion/stop)
    m_resetBtn = new QPushButton("↺  СБРОС", ctrlCard);
    m_resetBtn->setObjectName("resetBtn");
    m_resetBtn->setEnabled(false);
    m_resetBtn->setVisible(false);
    m_resetBtn->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  border: 1px solid #58a6ff;"
        "  color: #58a6ff;"
        "  border-radius: 5px;"
        "  padding: 9px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: rgba(88,166,255,0.12); border-color: #79c0ff; color: #79c0ff; }"
        "QPushButton:disabled { background: transparent; border-color: #30363d; color: #484f58; }"
    );
    connect(m_resetBtn, &QPushButton::clicked, this, &MainWindow::onReset);

    timeStopRow->addWidget(timeBox, 1);
    timeStopRow->addWidget(m_stopBtn);
    timeStopRow->addWidget(m_resetBtn);
    ctrlCardLayout->addLayout(timeStopRow);

    // TODO: Remove from non-production builds once engineers resolve UTC sync with БЦВМ.
    // Set time button (secondary, appears in Idle/Loaded)
    m_setTimeBtn = new QPushButton("⏱  УСТАНОВИТЬ ВРЕМЯ СТАРТА", ctrlCard);
    m_setTimeBtn->setObjectName("setTimeBtn");
    m_setTimeBtn->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  border: 1px solid #30363d;"
        "  color: #8b949e;"
        "  border-radius: 5px;"
        "  padding: 5px 10px;"
        "  font-size: 11px;"
        "  text-align: left;"
        "}"
        "QPushButton:hover { border-color: #58a6ff; color: #58a6ff; }"
    );
    connect(m_setTimeBtn, &QPushButton::clicked, this, &MainWindow::onSetTime);
    ctrlCardLayout->addWidget(m_setTimeBtn);

    m_timeInput = new QTimeEdit(ctrlCard);
    m_timeInput->setDisplayFormat("hh:mm:ss");
    m_timeInput->setTime(QTime(0, 0, 10));
    m_timeInput->setStyleSheet(
        "QTimeEdit {"
        "  background: #161b22;"
        "  color: #c9d1d9;"
        "  border: 1px solid #58a6ff;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "  font-size: 13px;"
        "}"
        "QTimeEdit::up-button, QTimeEdit::down-button {"
        "  background: #21262d; border: none; width: 16px;"
        "}"
        "QTimeEdit::up-arrow   { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-bottom: 5px solid #8b949e; }"
        "QTimeEdit::down-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top:    5px solid #8b949e; }"
    );
    m_timeInput->hide();
    ctrlCardLayout->addWidget(m_timeInput);

    topRow->addWidget(ctrlCard);
    mainLayout->addLayout(topRow);

    // ── CHANNEL CHIPS ROW ─────────────────────────────────────────────────────
    QWidget *channelPanel = new QWidget(this);
    QVBoxLayout *chanVBox = new QVBoxLayout(channelPanel);
    chanVBox->setSpacing(5);
    chanVBox->setContentsMargins(0, 0, 0, 0);

    QLabel *chanCaption = new QLabel("КАНАЛЫ", this);
    chanCaption->setStyleSheet("font-size: 12px; letter-spacing: 0.2em; color: #8b949e;");
    chanVBox->addWidget(chanCaption);

    QHBoxLayout *chipsRow = new QHBoxLayout();
    chipsRow->setSpacing(6);
    chipsRow->setContentsMargins(0, 0, 0, 0);

    static const char *chNames[] = {
        "Канал 1","Канал 2","Канал 3","Канал 4",
        "Канал 5","Канал 6","Канал 7","Канал 8"
    };
    for (int i = 0; i < 8; ++i) {
        QFrame *chip = new QFrame(this);
        chip->setStyleSheet(chipStyle("#30363d"));
        chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        QHBoxLayout *cl = new QHBoxLayout(chip);
        cl->setContentsMargins(7, 5, 9, 5);
        cl->setSpacing(6);

        QLabel *dot = new QLabel(chip);
        dot->setFixedSize(10, 10);
        dot->setStyleSheet(dotStyle("#484f58"));

        QLabel *name = new QLabel(chNames[i], chip);
        name->setStyleSheet("font-size: 13px; color: #e6edf3; background: transparent; border: none;");

        cl->addWidget(dot);
        cl->addWidget(name);

        m_channelChips[i] = {chip, dot};
        chipsRow->addWidget(chip);
    }
    chanVBox->addLayout(chipsRow);
    mainLayout->addWidget(channelPanel);

    // ── TIMELINE ──────────────────────────────────────────────────────────────
    m_timeline = new TimelineWidget(this);
    mainLayout->addWidget(m_timeline);

    // ── EVENT TABLE ───────────────────────────────────────────────────────────
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
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    mainLayout->addWidget(m_table, 1);  // растягивается при увеличении окна

    // ── SUMMARY STRIP (hidden until analysisDone) ─────────────────────────────
    m_summaryStrip = new QWidget(this);
    m_summaryStrip->setStyleSheet(
        "QWidget { background-color: #161b22; border-top: 2px solid #30363d; }");
    m_summaryStrip->setVisible(false);

    QHBoxLayout *summaryLayout = new QHBoxLayout(m_summaryStrip);
    summaryLayout->setContentsMargins(14, 8, 14, 8);
    summaryLayout->setSpacing(16);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName("summaryLabel");
    m_summaryLabel->setStyleSheet(
        "font-size: 13px; font-weight: 600; color: #e6edf3; background: transparent; border: none;");

    m_exportCsvBtn = new QPushButton("⬇ Экспорт CSV", this);
    m_exportCsvBtn->setObjectName("exportCsvBtn");
    m_exportCsvBtn->setStyleSheet(
        "QPushButton { background: #21262d; color: #8b949e; border: 1px solid #30363d;"
        "  border-radius: 4px; padding: 4px 12px; font-size: 12px; }"
        "QPushButton:hover { border-color: #58a6ff; color: #58a6ff; }"
    );
    connect(m_exportCsvBtn, &QPushButton::clicked, this, &MainWindow::onExportCsv);

    summaryLayout->addWidget(m_summaryLabel, 1);
    summaryLayout->addWidget(m_exportCsvBtn, 0);
    mainLayout->addWidget(m_summaryStrip);

    // ── LOG ───────────────────────────────────────────────────────────────────
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMinimumHeight(80);
    m_logEdit->setMaximumHeight(160);
    m_logEdit->setStyleSheet(
        "QTextEdit {"
        "  background: #0d1117;"
        "  border: 1px solid #21262d;"
        "  border-radius: 4px;"
        "  color: #6e7681;"
        "  font-family: 'JetBrains Mono', monospace;"
        "  font-size: 11px;"
        "  padding: 4px;"
        "}"
    );
    mainLayout->addWidget(m_logEdit);  // fixed height — таблица занимает всё оставшееся место

    // ── ПРАВАЯ ПАНЕЛЬ: техлог срабатываний каналов ───────────────────────────
    QWidget *rightPanel = new QWidget;
    rightPanel->setMinimumWidth(160);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(4, 14, 14, 14);
    rightLayout->setSpacing(6);

    auto *eventLogCaption = new QLabel("СРАБАТЫВАНИЯ", rightPanel);
    eventLogCaption->setStyleSheet(
        "font-size: 11px; font-weight: 600; letter-spacing: 0.2em; color: #8b949e;");
    rightLayout->addWidget(eventLogCaption);

    auto *eventLogSub = new QLabel("каналы · события", rightPanel);
    eventLogSub->setStyleSheet(
        "font-size: 10px; color: #484f58; letter-spacing: 0.05em;");
    rightLayout->addWidget(eventLogSub);

    m_eventLog = new QTextEdit(rightPanel);
    m_eventLog->setReadOnly(true);
    m_eventLog->setMinimumHeight(80);
    m_eventLog->setStyleSheet(
        "QTextEdit {"
        "  background: #0a0c10;"
        "  border: 1px solid #21262d;"
        "  border-radius: 4px;"
        "  color: #3fb950;"
        "  font-family: 'JetBrains Mono', monospace;"
        "  font-size: 12px;"
        "  padding: 6px;"
        "}"
    );
    rightLayout->addWidget(m_eventLog, 1);
    splitter->addWidget(rightPanel);

    splitter->setStretchFactor(0, 4);  // левая панель занимает 4/5
    splitter->setStretchFactor(1, 1);  // правая — 1/5, можно тащить сплиттер
    splitter->setSizes({760, 220});
}

// ─── Phase management ─────────────────────────────────────────────────────────

void MainWindow::setPhase(Phase newPhase)
{
    m_phase = newPhase;

    const bool portOk  = m_stand && m_stand->isPortOpen();
    const bool canLoad = (newPhase == Phase::Idle || newPhase == Phase::Loaded ||
                          newPhase == Phase::Completed || newPhase == Phase::Stopped);

    m_loadBtn->setEnabled(canLoad && portOk);
    m_setTimeBtn->setEnabled(newPhase == Phase::Idle || newPhase == Phase::Loaded);
    m_setTimeBtn->setVisible(newPhase == Phase::Idle || newPhase == Phase::Loaded);

    const bool duringFlight = (newPhase == Phase::Countdown || newPhase == Phase::Running);
    const bool afterFlight  = (newPhase == Phase::Completed || newPhase == Phase::Stopped);

    if (!duringFlight) m_streamTick = 0;

    m_stopBtn->setEnabled(duringFlight);
    m_stopBtn->setVisible(!afterFlight);
    m_resetBtn->setEnabled(afterFlight);
    m_resetBtn->setVisible(afterFlight);

    if (newPhase != Phase::Idle && newPhase != Phase::Loaded)
        m_timeInput->hide();

    // Timer caption
    QLabel *caption = findChild<QLabel*>("timerCaption");
    if (caption) {
        caption->setText((newPhase == Phase::Running || newPhase == Phase::Completed)
                             ? "ВРЕМЯ В ПОЛЁТЕ" : "ДО ЗАПУСКА");
    }

    // Timer display for non-ticking states
    if (newPhase == Phase::Completed) {
        m_timerLabel->setText("ПОЛЁТНОЕ\nЗАДАНИЕ\nОТРАБОТАНО");
        m_timerLabel->setStyleSheet(
            "font-size: 28px; font-weight: 700; color: #3fb950; font-family: 'JetBrains Mono';"
            " line-height: 1.3; letter-spacing: 0.04em;");
        m_nextEventLabel->setText("");
    } else if (newPhase == Phase::Stopped) {
        m_timerLabel->setText("СТОП");
        m_timerLabel->setStyleSheet(
            "font-size: 56px; font-weight: 700; color: #f85149; font-family: 'JetBrains Mono';"
            " letter-spacing: 0.04em;");
        m_nextEventLabel->setText("");
    } else if (!duringFlight) {
        m_timerLabel->setText("--:--");
        m_timerLabel->setStyleSheet(
            "font-size: 56px; font-weight: 700; color: #30363d; font-family: 'JetBrains Mono';"
            " letter-spacing: 0.04em;");
        m_nextEventLabel->setText("До события: --:--");
    }

    // Start time label
    if (m_stand && m_startTimeLabel) {
        const QTime t = m_stand->getStartTime();
        m_startTimeLabel->setText(t.isValid() ? t.toString("hh:mm:ss") : "--:--:--");
        const QString timeColor = (newPhase == Phase::Idle || newPhase == Phase::Loaded)
                                      ? "#e6edf3" : "#8b949e";
        m_startTimeLabel->setStyleSheet(
            QString("font-size: 13px; color: %1; font-family: 'JetBrains Mono';"
                    " background: transparent; border: none;").arg(timeColor));
    }

    // Table col 4 header: raw ticks during flight, relative ms after
    if (auto *hdr = m_table->horizontalHeaderItem(4))
        hdr->setText((newPhase == Phase::Running || newPhase == Phase::Countdown)
                         ? "ФАКТ (ТИК)" : "ФАКТ МС");

    // Channel chips
    if (newPhase == Phase::Running) {
        resetChannelDots();
        for (const auto &e : m_displayEvents) {
            if (e.hasChannels && (e.status.isEmpty() || e.status == "pending")) {
                for (const QString &part : e.channels.split(',')) {
                    bool ok = false; int c = part.trimmed().toInt(&ok);
                    if (ok && c >= 1 && c <= 8) updateChannelDot(c, "#e3b341");
                }
            }
        }
    } else if (newPhase == Phase::Idle || newPhase == Phase::Loaded) {
        resetChannelDots();
        if (m_summaryStrip) m_summaryStrip->setVisible(false);
    }

    updatePhaseLabel();
    refreshNextEventHighlight();
}

void MainWindow::updatePhaseLabel()
{
    if (!m_phaseLabel) return;

    struct { Phase p; const char* text; const char* color; bool blink; } cfg[] = {
        {Phase::Idle,      "ОЖИДАНИЕ",    "#8b949e", false},
        {Phase::Loaded,    "ЗАГРУЖЕНО",   "#58a6ff", false},
        {Phase::Countdown, "ОТСЧЁТ",      "#e3b341", true},
        {Phase::Running,   "ВЫПОЛНЕНИЕ",  "#3fb950", true},
        {Phase::Completed, "ЗАВЕРШЕНО",   "#3fb950", false},
        {Phase::Stopped,   "ОСТАНОВЛЕНО", "#f85149", false},
    };

    const char *text = "", *color = "#8b949e";
    bool blink = false;
    for (const auto &c : cfg)
        if (c.p == m_phase) { text = c.text; color = c.color; blink = c.blink; break; }

    if (blink && !m_blinkTimer->isActive()) { m_blinkTimer->start(); m_blinkState = true; }
    else if (!blink && m_blinkTimer->isActive()) { m_blinkTimer->stop(); m_blinkState = true; }

    m_phaseLabel->setText(QString("● %1").arg(text));
    m_phaseLabel->setStyleSheet(
        QString("font-size: 13px; letter-spacing: 0.15em; color: %1;")
            .arg((blink && !m_blinkState) ? "transparent" : color));
}

// ─── T11: Connection indicators ──────────────────────────────────────────────

void MainWindow::updateComIndicator(bool open)
{
    if (!m_comDot || !m_comStatus || !m_comChip) return;
    const QString dot    = open ? "#3fb950" : "#f85149";
    const QString border = open ? "#3fb950" : "#f85149";
    m_comDot->setStyleSheet(dotStyle(dot));
    m_comStatus->setText(open ? "связь есть" : "нет связи");
    m_comStatus->setStyleSheet(
        QString("font-size: 11px; color: %1; background: transparent; border: none;").arg(dot));
    m_comChip->setStyleSheet(chipStyle(border));
}

void MainWindow::updateBcvmIndicator(bool reachable)
{
    if (!m_bcvmDot || !m_bcvmStatus || !m_bcvmChip) return;
    const QString dot    = reachable ? "#3fb950" : "#484f58";
    const QString border = reachable ? "#3fb950" : "#30363d";
    m_bcvmDot->setStyleSheet(dotStyle(dot));
    m_bcvmStatus->setText(reachable ? "готова" : "не загружено");
    m_bcvmStatus->setStyleSheet(
        QString("font-size: 11px; color: %1; background: transparent; border: none;").arg(dot));
    m_bcvmChip->setStyleSheet(chipStyle(border));
}

// ─── T12: Channel chips ───────────────────────────────────────────────────────

void MainWindow::updateChannelDot(int channel, const QString &color)
{
    if (channel < 1 || channel > 8) return;
    auto &ch = m_channelChips[channel - 1];
    if (!ch.frame || !ch.dot) return;

    ch.dot->setStyleSheet(dotStyle(color));

    QString borderColor;
    QString bgColor = "#161b22";
    if      (color == "#3fb950") { borderColor = "#3fb950"; bgColor = "rgba(63,185,80,0.08)";  }
    else if (color == "#f85149") { borderColor = "#f85149"; bgColor = "rgba(248,81,73,0.08)";  }
    else if (color == "#e3b341") { borderColor = "#e3b341"; bgColor = "rgba(227,179,65,0.08)"; }
    else                         { borderColor = "#30363d"; }

    ch.frame->setStyleSheet(chipStyle(borderColor, bgColor));
}

void MainWindow::resetChannelDots()
{
    for (int i = 0; i < 8; ++i) {
        if (m_channelChips[i].frame) {
            m_channelChips[i].dot->setStyleSheet(dotStyle("#484f58"));
            m_channelChips[i].frame->setStyleSheet(chipStyle("#30363d"));
        }
    }
}

// ─── UI helpers ───────────────────────────────────────────────────────────────

void MainWindow::refreshNextEventHighlight()
{
    for (int i = 0; i < m_table->rowCount(); ++i)
        for (int j = 0; j < m_table->columnCount(); ++j)
            if (auto *item = m_table->item(i, j))
                item->setBackground(QColor());

    if (m_phase != Phase::Countdown && m_phase != Phase::Running) return;

    for (int i = 0; i < m_displayEvents.size(); ++i) {
        if (m_displayEvents[i].status == "pending" && m_displayEvents[i].hasChannels) {
            const int tableRow = (i < m_eventToRow.size()) ? m_eventToRow[i] : i;
            for (int j = 0; j < m_table->columnCount(); ++j)
                if (auto *item = m_table->item(tableRow, j))
                    item->setBackground(j == 0 ? QColor(31, 111, 235, 180) : QColor(20, 40, 60));
            m_nextEventRow = tableRow;
            break;
        }
    }
}

void MainWindow::updateTimer(const TimerState &state)
{
    // Right log: byte reception heartbeat every 1000 bytes (= each timerTick)
    if (m_eventLog && (state.phase == Phase::Countdown || state.phase == Phase::Running)) {
        const int64_t byteIdx = static_cast<int64_t>(m_streamTick) * 1000;
        const QString ts = QTime::currentTime().toString("HH:mm:ss");
        m_eventLog->append(
            QString("<span style=\"color:#484f58;\">%1</span>"
                    " <span style=\"color:#6e7681;\">▸ поток [%2 мс]</span>")
                .arg(ts).arg(byteIdx));
        m_eventLog->verticalScrollBar()->setValue(
            m_eventLog->verticalScrollBar()->maximum());
        ++m_streamTick;
    }

    auto fmtHMS = [](int secs) {
        return QString("%1:%2:%3")
            .arg(secs / 3600, 2, 10, QChar('0'))
            .arg((secs % 3600) / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0'));
    };
    auto fmtMS = [](int secs) {
        return QString("%1:%2")
            .arg(secs / 60, 2, 10, QChar('0'))
            .arg(secs % 60, 2, 10, QChar('0'));
    };

    QString text, color;
    if (state.msToStart < 0) {
        const int secs = static_cast<int>(-state.msToStart / 1000);
        text  = (secs >= 3600) ? fmtHMS(secs) : fmtMS(secs);
        color = "#e3b341";
    } else {
        const int secs   = static_cast<int>(state.msToStart / 1000);
        const int tenth  = static_cast<int>((state.msToStart % 1000) / 100);
        if (secs >= 3600)
            text = fmtHMS(secs);
        else
            text = QString("%1:%2.%3")
                .arg(secs / 60, 2, 10, QChar('0'))
                .arg(secs % 60, 2, 10, QChar('0'))
                .arg(tenth);
        color = "#3fb950";
    }
    m_timerLabel->setText(text);
    m_timerLabel->setStyleSheet(
        QString("font-size: 56px; font-weight: 700; color: %1; font-family: 'JetBrains Mono';"
                " letter-spacing: 0.04em;").arg(color));

    // Плейхед таймлайна: msToStart уже относительно фактического T0 после КП
    m_timeline->setPlayheadMs(state.msToStart);
}

void MainWindow::updateNextEventTimer(const NextEventInfo &info)
{
    if (info.eventId == -1 || info.msRemaining == INT64_MAX) {
        m_nextEventLabel->setText("До события: --:--");
        return;
    }
    const int secs  = static_cast<int>(info.msRemaining / 1000);
    const int tenth = static_cast<int>((info.msRemaining % 1000) / 100);
    const QString time = QString("%1:%2.%3")
        .arg(secs / 60, 2, 10, QChar('0')).arg(secs % 60, 2, 10, QChar('0')).arg(tenth);
    m_nextEventLabel->setText(
        QString("До события: <b style=\"color:#e6edf3;\">%1</b> (%2)").arg(time, info.description));
}

void MainWindow::addLog(const QString &text, const QString &type)
{
    QString barColor  = "#30363d";
    QString levelText = "INFO";
    QString levelColor = "#8b949e";
    QString msgColor   = "#c8d0dc";

    if (type == "system") {
        barColor = "#30363d"; levelText = "INFO";   levelColor = "#8b949e"; msgColor = "#8b949e";
    } else if (type == "event") {
        barColor = "#3fb950"; levelText = "СОБЫТ."; levelColor = "#3fb950"; msgColor = "#c8d0dc";
    } else if (type == "event-post") {
        barColor = "#f85149"; levelText = "СОБЫТ."; levelColor = "#f85149"; msgColor = "#c8d0dc";
    } else if (type == "error") {
        barColor = "#f85149"; levelText = "ОШИБКА"; levelColor = "#f85149"; msgColor = "#f85149";
    }

    const QString timeStr = QTime::currentTime().toString("HH:mm:ss");
    const QString escaped = text.toHtmlEscaped();
    // ▌ (U+258C) as colored left bar — border-left не работает в QTextEdit
    m_logEdit->append(
        QString("<span style=\"color:%1;\">▌</span>"
                " <span style=\"color:#484f58;\">%2</span>"
                " <span style=\"color:%3; font-weight:600;\">%4</span>"
                "  <span style=\"color:%5;\">%6</span>")
        .arg(barColor, timeStr, levelColor, levelText, msgColor, escaped));

    // Техлог справа — только сырые срабатывания каналов, без системных сообщений
    if (m_eventLog && (type == "event" || type == "event-post")) {
        const QString evColor = (type == "event") ? "#3fb950" : "#f85149";
        m_eventLog->append(
            QString("<span style=\"color:#484f58;\">%1</span>"
                    " <span style=\"color:%2;\">%3</span>")
                .arg(timeStr, evColor, escaped));
        m_eventLog->verticalScrollBar()->setValue(
            m_eventLog->verticalScrollBar()->maximum());
    }
}

void MainWindow::updateTable(const QVector<EventRow> &events)
{
    m_displayEvents = events;

    // Подсчитываем итоговые строки: основная + подстроки для многоканальных событий
    int totalRows = 0;
    m_eventToRow.resize(m_displayEvents.size());
    for (int i = 0; i < m_displayEvents.size(); ++i) {
        m_eventToRow[i] = totalRows;
        ++totalRows;
        if (m_displayEvents[i].channelCalcMs.size() >= 2)
            totalRows += m_displayEvents[i].channelCalcMs.size();
    }

    m_table->setRowCount(totalRows);

    for (int i = 0; i < m_displayEvents.size(); ++i) {
        const int mainRow = m_eventToRow[i];
        updateTableRow(mainRow, m_displayEvents[i]);

        const auto &ev = m_displayEvents[i];
        if (ev.channelCalcMs.size() >= 2) {
            // Определяем канал, сработавший последним (у него максимальный calcMs)
            int lastCh = -1, maxMs = INT_MIN;
            for (auto it = ev.channelCalcMs.constBegin(); it != ev.channelCalcMs.constEnd(); ++it)
                if (it.value() > maxMs) { maxMs = it.value(); lastCh = it.key(); }
            const bool hasSpread = (ev.channelSpreadMs > 0);

            int subRow = mainRow + 1;
            for (auto it = ev.channelCalcMs.constBegin(); it != ev.channelCalcMs.constEnd(); ++it, ++subRow)
                setSubRow(subRow, it.key(), it.value(), it.key() == lastCh && hasSpread, hasSpread);
        }
    }

    refreshNextEventHighlight();

    if (!m_displayEvents.isEmpty()) {
        const int rowH = m_table->rowHeight(0);
        const int hdrH = m_table->horizontalHeader()->height();
        const int maxRows = 14;
        const int visRows = std::min(totalRows, maxRows);
        m_table->setMaximumHeight(hdrH + visRows * rowH + 4);
    }
}

void MainWindow::setSubRow(int row, int channel, int calcMs, bool isLast, bool /*hasSpread*/)
{
    if (row < 0 || row >= m_table->rowCount()) return;

    const QColor bg(0x08, 0x0c, 0x12);
    const QColor fg(0x6e, 0x76, 0x81);

    auto cell = [this, row, &bg, &fg](int col, const QString &text,
                                      Qt::AlignmentFlag align,
                                      const QColor &color = QColor()) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(align | Qt::AlignVCenter);
        item->setBackground(bg);
        item->setForeground(color.isValid() ? color : fg);
        m_table->setItem(row, col, item);
    };

    const QString msStr = QString::number(calcMs);
    const QString lastMark = isLast ? " ← послед." : "";

    cell(0, "",                                   Qt::AlignCenter);
    cell(1, QString("    ↳  Канал %1").arg(channel), Qt::AlignLeft);
    cell(2, QString::number(channel),             Qt::AlignCenter);
    cell(3, "—",                                  Qt::AlignCenter);
    cell(4, msStr,                                Qt::AlignRight, QColor("#8b949e"));
    cell(5, "—",                                  Qt::AlignCenter);
    cell(6, isLast ? lastMark.trimmed() : "—",    Qt::AlignLeft,
         isLast ? QColor("#e3b341") : fg);
}

void MainWindow::updateTableRow(int row, const EventRow &data)
{
    if (row < 0 || row >= m_table->rowCount()) return;

    auto setItem = [this, row](int col, const QString &text,
                               Qt::AlignmentFlag align = Qt::AlignLeft,
                               const QColor &fg = QColor()) {
        auto *item = new QTableWidgetItem(text);
        item->setTextAlignment(align | Qt::AlignVCenter);
        if (fg.isValid()) item->setForeground(fg);
        m_table->setItem(row, col, item);
    };

    setItem(0, QString::number(data.id), Qt::AlignCenter, QColor("#8b949e"));
    setItem(1, data.description.isEmpty() ? data.key : data.description);
    // Каналы: для группы — показываем разброс между первым и последним срабатыванием
    const QString chText = (data.channelSpreadMs > 0)
        ? QString("%1  (Δ%2мс)").arg(data.channels).arg(data.channelSpreadMs)
        : data.channels;
    setItem(2, chText, Qt::AlignCenter, QColor("#8b949e"));

    // Col 3: ПЛАН МС — always visible, always grey
    setItem(3, QString::number(data.time_ms), Qt::AlignRight, QColor("#8b949e"));

    const bool final = (m_phase == Phase::Completed || m_phase == Phase::Stopped);

    if (!final) {
        // ── Режим полёта: сырой тик + статус СРАБОТАН ────────────────────
        if (data.firedTick != -1) {
            setItem(4, QString::number(data.firedTick), Qt::AlignRight, QColor("#e6edf3"));
            setItem(6, "СРАБОТАН", Qt::AlignCenter, QColor("#3fb950"));
        } else {
            setItem(4, "—", Qt::AlignCenter, QColor("#484f58"));
            setItem(6, "—", Qt::AlignCenter, QColor("#484f58"));
        }
        setItem(5, "—", Qt::AlignCenter, QColor("#484f58"));
    } else {
        // ── Финальный режим: анализ относительно фактического T0 ────────────
        const bool isT0 = (data.key == QLatin1String("LIFT_OFF_CONTACT"));

        // Col 4: ФАКТ МС — calculatedMs (относительно T0)
        if (data.calculatedMs != -1) {
            setItem(4, QString::number(data.calculatedMs), Qt::AlignRight, QColor("#e6edf3"));
        } else {
            setItem(4, "—", Qt::AlignCenter, QColor("#484f58"));
        }

        if (data.status == "fail") {
            // Не сработало совсем
            setItem(5, "—", Qt::AlignCenter, QColor("#484f58"));
            setItem(6, "✗ НЕ СРАБОТАЛО", Qt::AlignCenter, QColor("#f85149"));
            // Подсветка строки красным
            for (int c = 0; c < m_table->columnCount(); ++c)
                if (auto *it = m_table->item(row, c)) it->setBackground(QColor(0x3d, 0x15, 0x15));
        } else if (isT0) {
            // КП штатно = отклонение < 1 с. Откл. не показываем — не важно для оператора.
            // Оранжевый только при грубом расхождении ≥ 1 с (проблема синхронизации).
            if (data.deviationMs < 1000) {
                setItem(5, "—", Qt::AlignCenter, QColor("#484f58"));
                setItem(6, "КП ШТАТНО", Qt::AlignCenter, QColor("#3fb950"));
            } else {
                setItem(5, QString("+%1 мс").arg(data.deviationMs), Qt::AlignRight, QColor("#e3b341"));
                setItem(6, "КП ОТКЛ", Qt::AlignCenter, QColor("#e3b341"));
            }
            for (int c = 0; c < m_table->columnCount(); ++c)
                if (auto *it = m_table->item(row, c)) it->setBackground(QColor(0x0f, 0x2d, 0x1a));
        } else {
            // Обычное событие: цвет по отклонению относительно T0
            QColor devColor;
            QString statusText;
            if (data.deviationMs == 0) {
                devColor   = QColor("#3fb950");
                statusText = "ОК";
            } else if (data.deviationMs < 5) {
                // Аппаратный джиттер (1-4 мс) — в пределах допуска, зелёный
                devColor   = QColor("#3fb950");
                statusText = QString("ОК  (%1мс)").arg(data.deviationMs);
            } else {
                // Заметное отклонение — жёлтый (красный = только fail)
                devColor   = QColor("#e3b341");
                statusText = QString("+%1 мс").arg(data.deviationMs);
            }
            setItem(5, data.deviationMs == 0
                ? QString("—")
                : QString("+%1 мс").arg(data.deviationMs),
                Qt::AlignRight, devColor);
            setItem(6, statusText, Qt::AlignCenter, devColor);
            const QColor rowBg = (data.deviationMs == 0)
                ? QColor(0x0f, 0x2d, 0x1a)
                : QColor(0x2a, 0x22, 0x0a);
            for (int c = 0; c < m_table->columnCount(); ++c)
                if (auto *it = m_table->item(row, c)) it->setBackground(rowBg);
        }
    }

    refreshNextEventHighlight();
}

// ─── Button slots ─────────────────────────────────────────────────────────────

void MainWindow::onLoadToBoard() { m_stand->sendToBoard(); }

void MainWindow::onSetTime()
{
    if (m_timeInput->isVisible()) {
        const QTime t = m_timeInput->time();
        m_timeInput->hide();
        m_stand->setStartTimeFromUI(t);
        if (m_startTimeLabel)
            m_startTimeLabel->setText(t.toString("hh:mm:ss"));
    } else {
        m_timeInput->setTime(m_stand->getStartTime());
        m_timeInput->show();
        m_timeInput->setFocus();
    }
}

void MainWindow::onStop() { m_stand->stop(); }

void MainWindow::onReset()
{
    m_stand->resetForNewTest();
    m_stand->loadCyclogram();
    m_timeline->reset();
    m_timeline->setEvents(m_stand->getEvents());
    setPhase(Phase::Loaded);
}

// ─── T17: Summary strip + CSV export ─────────────────────────────────────────

void MainWindow::updateSummaryStrip(const QVector<EventRow> &events)
{
    if (!m_summaryLabel) return;
    // ✓ зелёный: откл. 0 мс   ≈ жёлтый: любое ненулевое откл.   ✗ красный: не сработало
    int exact = 0, nearCount = 0, badCount = 0, maxDev = 0;
    for (const auto &e : events) {
        if (e.status == "fail") {
            ++badCount;
        } else if (e.status == "ok" || e.status == "late") {
            if (e.deviationMs > maxDev) maxDev = e.deviationMs;
            if (e.deviationMs == 0) ++exact;
            else                    ++nearCount;
        }
    }
    QString summary = QString("<span style=\"color:#e6edf3;\">Результат: "
        "<span style=\"color:#3fb950;\">%1 ✓</span>"
        " / <span style=\"color:#e3b341;\">%2 ≈</span>"
        " / <span style=\"color:#f85149;\">%3 ✗</span>"
        " &nbsp;·&nbsp; Макс. откл.: %4 мс</span>")
        .arg(exact).arg(nearCount).arg(badCount).arg(maxDev);
    m_summaryLabel->setText(summary);
}

void MainWindow::onExportCsv()
{
    const QString path = QFileDialog::getSaveFileName(
        this, "Экспорт в CSV",
        QString("pyro_result_%1.csv").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm")),
        "CSV (*.csv)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Ошибка", "Не удалось открыть файл:\n" + path);
        return;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "\xEF\xBB\xBF";
    out << "#,Событие,Каналы,План (мс),Факт (мс),Откл. (мс),Статус\n";

    auto csvField = [](const QString &s) {
        return s.contains(',') ? "\"" + s + "\"" : s;
    };

    for (int i = 0; i < m_displayEvents.size(); ++i) {
        const EventRow &e = m_displayEvents[i];
        const QString factStr = (e.calculatedMs != -1) ? QString::number(e.calculatedMs) : "NA";
        const QString devStr  = (e.deviationMs != -1 && (e.status == "ok" || e.status == "late"))
                                    ? QString::number(e.deviationMs) : "";
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
