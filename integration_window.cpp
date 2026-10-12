#include "integration_window.h"
#include "timeline_widget.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

const QColor kDim("#8b949e");
const QColor kNeutral("#e6edf3");
const QColor kGreen("#3fb950");
const QColor kAmber("#e3b341");
const QColor kRed("#f85149");

QString signedMs(double value) {
    if (std::abs(value) < 0.0005) value = 0.0;
    return (value > 0.0 ? QStringLiteral("+") : QString()) +
           QString::number(value, 'f', 3);
}

QString inputName(const InputRef &input) {
    return QStringLiteral("ЯПС %1 / %2 %3")
        .arg(input.index / 8 + 1)
        .arg(input.index % 8 + 1)
        .arg(input.falling ? QStringLiteral("↓") : QStringLiteral("↑"));
}

QString inputNames(const ActionMapping &mapping) {
    QStringList names;
    for (const auto &input : mapping.inputs) names << inputName(input);
    return names.join(QStringLiteral(", "));
}

void cell(QTableWidget *table, int row, int col, const QString &value,
          const QColor &foreground = kNeutral, const QColor &background = QColor(),
          Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter,
          const QString &tip = {}) {
    QTableWidgetItem *item = table->item(row, col);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, col, item);
    }
    if (item->text() != value) item->setText(value);
    item->setTextAlignment(alignment);
    item->setForeground(foreground);
    item->setBackground(background);
    item->setToolTip(tip);
}

QFrame *statusChip(QWidget *parent, const QString &title, QLabel **dotOut, QLabel **statusOut) {
    auto *chip = new QFrame(parent);
    chip->setStyleSheet(QStringLiteral(
        "QFrame { background:#161b22; border:1px solid #30363d; border-radius:5px; }"));
    auto *lay = new QHBoxLayout(chip);
    lay->setContentsMargins(8, 6, 9, 6);
    lay->setSpacing(8);
    auto *dot = new QLabel(chip);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(QStringLiteral("background:#484f58; border:0; border-radius:4px;"));
    auto *col = new QVBoxLayout;
    col->setSpacing(1);
    auto *caption = new QLabel(title, chip);
    caption->setStyleSheet(QStringLiteral(
        "font-size:12px; font-weight:600; color:#e6edf3; background:transparent; border:0;"));
    auto *status = new QLabel(QStringLiteral("нет связи"), chip);
    status->setStyleSheet(QStringLiteral("font-size:11px; color:#8b949e; background:transparent; border:0;"));
    col->addWidget(caption);
    col->addWidget(status);
    lay->addWidget(dot);
    lay->addLayout(col, 1);
    *dotOut = dot;
    *statusOut = status;
    return chip;
}

void setChip(QLabel *dot, QLabel *status, bool active, const QString &message) {
    const QString color = active ? QStringLiteral("#3fb950") : QStringLiteral("#f85149");
    dot->setStyleSheet(QStringLiteral("background:%1; border:0; border-radius:4px;").arg(color));
    status->setText(message);
    status->setStyleSheet(QStringLiteral(
        "font-size:11px; color:%1; background:transparent; border:0;").arg(
            active ? QStringLiteral("#3fb950") : QStringLiteral("#8b949e")));
}

struct DisplayAction {
    PlannedAction plan;
    bool physicalKp = false;
};

} // namespace

IntegrationWindow::IntegrationWindow(IntegrationSession::Options options,
                                     const QString &mappingPath, QWidget *parent)
    : QMainWindow(parent), m_session(std::move(options), this) {
    setupUi();
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(65);
    connect(&m_refreshTimer, &QTimer::timeout, this, &IntegrationWindow::refresh);
    connect(&m_session, &IntegrationSession::changed, this, [this] {
        if (!m_refreshTimer.isActive()) m_refreshTimer.start();
    });
    connect(&m_session, &IntegrationSession::message, this, &IntegrationWindow::appendMessage);
    m_clockTimer.setInterval(250);
    connect(&m_clockTimer, &QTimer::timeout, this, &IntegrationWindow::updatePlannedClock);
    m_clockTimer.start();

    if (!mappingPath.isEmpty()) m_session.loadMapping(mappingPath);
    m_session.start();
    refresh();
}

void IntegrationWindow::setupUi() {
    setWindowTitle(QStringLiteral("Пиростенд — контроль исполнения циклограммы"));
    resize(1380, 840);
    setMinimumSize(940, 580);

    // Original summer HMI structure: main content with timer, timeline,
    // event table and system log; narrow event journal on the right.
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setStyleSheet(QStringLiteral(
        "QSplitter::handle { background:#21262d; width:3px; }"));
    setCentralWidget(splitter);

    auto *left = new QWidget(splitter);
    auto *main = new QVBoxLayout(left);
    main->setSpacing(10);
    main->setContentsMargins(20, 14, 10, 14);

    auto *top = new QHBoxLayout;
    top->setSpacing(16);
    auto *clock = new QVBoxLayout;
    clock->setSpacing(4);
    m_clockCaption = new QLabel(QStringLiteral("ДО ПЛАНОВОГО T0 · ЧАСЫ ПК"), left);
    m_clockCaption->setStyleSheet(QStringLiteral(
        "font-size:13px; letter-spacing:0.15em; color:#8b949e;"));
    m_timerLabel = new QLabel(QStringLiteral("--:--"), left);
    m_timerLabel->setObjectName(QStringLiteral("timerLabel"));
    m_timerLabel->setStyleSheet(QStringLiteral(
        "font-size:64px; font-weight:700; color:#30363d; font-family:'JetBrains Mono';"));
    auto *phase = new QHBoxLayout;
    m_phaseLabel = new QLabel(QStringLiteral("● ОЖИДАНИЕ"), left);
    m_phaseLabel->setStyleSheet(QStringLiteral("font-size:13px; font-weight:600; color:#8b949e;"));
    m_nextEventLabel = new QLabel(QStringLiteral("Нет циклограммы"), left);
    m_nextEventLabel->setStyleSheet(QStringLiteral("font-size:12px; color:#6e7681;"));
    phase->addWidget(m_phaseLabel);
    phase->addWidget(m_nextEventLabel, 1);
    clock->addStretch();
    clock->addWidget(m_clockCaption);
    clock->addWidget(m_timerLabel);
    clock->addLayout(phase);
    clock->addStretch();
    top->addLayout(clock, 1);

    auto *controls = new QWidget(left);
    controls->setObjectName(QStringLiteral("ctrlCard"));
    controls->setMinimumWidth(250);
    controls->setMaximumWidth(360);
    controls->setStyleSheet(QStringLiteral(
        "QWidget#ctrlCard { background:#161b22; border:1px solid #30363d; border-radius:6px; }"));
    auto *ctrl = new QVBoxLayout(controls);
    ctrl->setSpacing(8);
    ctrl->setContentsMargins(10, 10, 10, 10);
    auto *connection = new QHBoxLayout;
    connection->setSpacing(6);
    connection->addWidget(statusChip(controls, QStringLiteral("Debug КАСУ ПП"),
                                     &m_debugDot, &m_debugStatus));
    connection->addWidget(statusChip(controls, QStringLiteral("Плата"),
                                     &m_boardDot, &m_boardStatus));
    ctrl->addLayout(connection);
    ctrl->addWidget(statusChip(controls, QStringLiteral("БЦВМ / разрешение"),
                               &m_bcvmDot, &m_bcvmStatus));

    auto *startBox = new QFrame(controls);
    startBox->setStyleSheet(QStringLiteral(
        "QFrame { background:#0d1117; border:1px solid #30363d; border-radius:5px; }"));
    auto *startLay = new QHBoxLayout(startBox);
    startLay->setContentsMargins(8, 7, 8, 7);
    auto *startCaption = new QLabel(QStringLiteral("T0 UTC"), startBox);
    startCaption->setStyleSheet(QStringLiteral("font-size:11px; color:#6e7681; border:0;"));
    m_startTimeLabel = new QLabel(QStringLiteral("--:--:--"), startBox);
    m_startTimeLabel->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#e6edf3; font-family:'JetBrains Mono'; border:0;"));
    startLay->addWidget(startCaption);
    startLay->addWidget(m_startTimeLabel, 1);
    ctrl->addWidget(startBox);

    // КАСУ ПП loads the plan and controls the vehicle. Never offer those controls here.
    auto *reset = new QPushButton(QStringLiteral("↺  СБРОСИТЬ ИЗМЕРЕНИЯ ПЛАТЫ"), controls);
    reset->setObjectName(QStringLiteral("resetBtn"));
    reset->setStyleSheet(QStringLiteral(
        "QPushButton { background:#1f6feb; color:white; border:1px solid #1f6feb;"
        "border-radius:5px; padding:9px 10px; font-size:12px; font-weight:600; }"
        "QPushButton:hover { background:#388bfd; }"));
    ctrl->addWidget(reset);

    auto *map = new QPushButton(QStringLiteral("КАРТА СООТВЕТСТВИЯ СОБЫТИЙ"), controls);
    auto *save = new QPushButton(QStringLiteral("СОХРАНИТЬ ОТЧЁТ"), controls);
    for (auto *button : {map, save}) {
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:#0d1117; color:#c8d0dc; border:1px solid #30363d;"
            "border-radius:5px; padding:7px 10px; font-size:11px; }"
            "QPushButton:hover { border-color:#58a6ff; color:#58a6ff; }"));
        ctrl->addWidget(button);
    }
    top->addWidget(controls);
    main->addLayout(top);

    connect(reset, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Сброс измерений"),
            QStringLiteral("Отправить аппаратную команду сброса пиротестеру? "
                           "Полученные отметки платы будут сброшены. "
                           "Для нового прогона потребуется пустой кадр после сброса."));
        if (answer == QMessageBox::Yes) m_session.resetBoard();
    });
    connect(map, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Карта соответствия событий"), {}, QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) m_session.loadMapping(path);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Отчёт испытания"),
            QStringLiteral("pyro_stand_report.json"), QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) return;
        QString error;
        if (!m_session.saveReport(path, &error))
            QMessageBox::warning(this, QStringLiteral("Отчёт"), error);
        else
            appendMessage(QStringLiteral("Отчёт сохранён: %1 (включая .frames.jsonl)").arg(path));
    });

    // Keep an operator-centric status line, not a table of all tester inputs.
    auto *meta = new QFrame(left);
    meta->setStyleSheet(QStringLiteral(
        "QFrame { background:#161b22; border:1px solid #30363d; border-radius:5px; }"
        "QLabel { background:transparent; border:0; font-size:11px; color:#8b949e; }"));
    auto *metaLay = new QVBoxLayout(meta);
    metaLay->setContentsMargins(10, 6, 10, 6);
    metaLay->setSpacing(3);
    m_uploadStatus = new QLabel(meta);
    m_kpStatus = new QLabel(meta);
    m_mappingStatus = new QLabel(meta);
    m_clockStatus = new QLabel(meta);
    m_clockStatus->setWordWrap(true);
    metaLay->addWidget(m_uploadStatus);
    metaLay->addWidget(m_kpStatus);
    metaLay->addWidget(m_mappingStatus);
    metaLay->addWidget(m_clockStatus);
    main->addWidget(meta);

    // Use the original timeline painter and event-centric semantics.
    m_timeline = new TimelineWidget(left);
    m_timeline->setObjectName(QStringLiteral("eventTimeline"));
    m_timeline->setVisible(false);
    main->addWidget(m_timeline);

    m_eventCaption = new QLabel(QStringLiteral("ОТСЛЕЖИВАЕМЫЕ СОБЫТИЯ"), left);
    m_eventCaption->setStyleSheet(QStringLiteral(
        "font-size:12px; letter-spacing:0.2em; color:#8b949e;"));
    main->addWidget(m_eventCaption);

    m_eventTable = new QTableWidget(0, 7, left);
    m_eventTable->setObjectName(QStringLiteral("eventTable"));
    m_eventTable->setHorizontalHeaderLabels({
        QStringLiteral("#"), QStringLiteral("СОБЫТИЕ"),
        QStringLiteral("КАНАЛЫ СОБЫТИЯ"), QStringLiteral("ПЛАН МС"),
        QStringLiteral("ФАКТ МС"), QStringLiteral("ОТКЛ МС"),
        QStringLiteral("СТАТУС")});
    auto *header = m_eventTable->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int col = 2; col < 7; ++col)
        header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->verticalHeader()->hide();
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_eventTable->setFocusPolicy(Qt::NoFocus);
    main->addWidget(m_eventTable, 1);

    auto *summary = new QFrame(left);
    summary->setStyleSheet(QStringLiteral(
        "QFrame { background:#161b22; border-top:2px solid #30363d; }"));
    auto *summaryLay = new QHBoxLayout(summary);
    summaryLay->setContentsMargins(14, 8, 14, 8);
    m_summaryLabel = new QLabel(summary);
    m_summaryLabel->setStyleSheet(QStringLiteral(
        "color:#e6edf3; font-size:12px; font-weight:600; border:0;"));
    summaryLay->addWidget(m_summaryLabel);
    main->addWidget(summary);

    m_log = new QTextEdit(left);
    m_log->setObjectName(QStringLiteral("systemJournal"));
    m_log->setReadOnly(true);
    m_log->setMinimumHeight(80);
    m_log->setMaximumHeight(140);
    m_log->setStyleSheet(QStringLiteral(
        "QTextEdit { background:#0d1117; border:1px solid #21262d; border-radius:4px;"
        "color:#8b949e; font-size:11px; font-family:'JetBrains Mono'; padding:4px; }"));
    main->addWidget(m_log);

    auto *right = new QWidget(splitter);
    right->setMinimumWidth(175);
    auto *rightLay = new QVBoxLayout(right);
    rightLay->setContentsMargins(4, 14, 14, 14);
    rightLay->setSpacing(6);
    auto *eventCaption = new QLabel(QStringLiteral("СРАБАТЫВАНИЯ"), right);
    eventCaption->setStyleSheet(QStringLiteral(
        "font-size:11px; font-weight:600; letter-spacing:0.2em; color:#8b949e;"));
    rightLay->addWidget(eventCaption);
    auto *eventSub = new QLabel(QStringLiteral("события циклограммы"), right);
    eventSub->setStyleSheet(QStringLiteral("font-size:10px; color:#484f58;"));
    rightLay->addWidget(eventSub);
    m_eventLog = new QTextEdit(right);
    m_eventLog->setObjectName(QStringLiteral("eventJournal"));
    m_eventLog->setReadOnly(true);
    m_eventLog->setStyleSheet(QStringLiteral(
        "QTextEdit { background:#0a0c10; border:1px solid #21262d; border-radius:4px;"
        "color:#3fb950; font-size:12px; font-family:'JetBrains Mono'; padding:6px; }"));
    rightLay->addWidget(m_eventLog, 1);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({1080, 280});
}

void IntegrationWindow::appendMessage(const QString &message) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_log->append(QStringLiteral("<span style=\"color:#6e7681;\">%1</span> %2")
        .arg(stamp.toHtmlEscaped(), message.toHtmlEscaped()));
}

void IntegrationWindow::updatePlannedClock() {
    if (!m_session.hasPlan() || !m_session.plan().t0Utc.isValid()) {
        m_timerLabel->setText(QStringLiteral("--:--"));
        return;
    }
    // This is explicitly the PC's UTC clock, NOT the tester's hardware counter.
    const qint64 deltaMs = QDateTime::currentDateTimeUtc().msecsTo(m_session.plan().t0Utc);
    const bool before = deltaMs > 0;
    m_clockCaption->setText(before ? QStringLiteral("ДО ПЛАНОВОГО T0 · ЧАСЫ ПК")
                                   : QStringLiteral("ПОСЛЕ ПЛАНОВОГО T0 · ЧАСЫ ПК"));
    const qint64 seconds = qAbs(deltaMs) / 1000;
    const QString display = seconds >= 3600
        ? QStringLiteral("%1:%2:%3")
            .arg(seconds / 3600, 2, 10, QChar('0'))
            .arg((seconds / 60) % 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'))
        : QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'));
    m_timerLabel->setText(before ? display : QStringLiteral("+") + display);
    m_timerLabel->setStyleSheet(QStringLiteral(
        "font-size:64px; font-weight:700; font-family:'JetBrains Mono'; color:%1;")
        .arg(before ? QStringLiteral("#e3b341") : QStringLiteral("#58a6ff")));
    m_timerLabel->setToolTip(QStringLiteral(
        "Только разница между плановым T0 UTC и текущими часами ПК. "
        "Не является временем исполнения, полученным от платы."));
}

void IntegrationWindow::refresh() {
    const bool hasPlan = m_session.hasPlan();
    const bool eligible = m_session.measurementEligible();
    const bool kpKnown = eligible && m_session.kpTimeUs().has_value();
    const bool uploaded = m_session.uploadState() == QStringLiteral("CONFIRMED");

    setChip(m_debugDot, m_debugStatus, m_session.debugConnected(),
            m_session.debugConnected() ? QStringLiteral("подключён") : QStringLiteral("нет связи"));
    setChip(m_boardDot, m_boardStatus, m_session.boardReceiving(),
            m_session.boardReceiving() ? QStringLiteral("кадры идут") : QStringLiteral("нет кадров"));
    setChip(m_bcvmDot, m_bcvmStatus, m_session.executionEnabled(),
            m_session.executionEnabled() ? QStringLiteral("разрешено")
                                         : QStringLiteral("не разрешено"));

    QString phaseText;
    QString phaseColor = QStringLiteral("#8b949e");
    if (!hasPlan) {
        phaseText = QStringLiteral("ОЖИДАНИЕ ЦИКЛОГРАММЫ");
    } else if (!uploaded) {
        phaseText = QStringLiteral("ЗАГРУЗКА НЕ ПОДТВЕРЖДЕНА");
        phaseColor = QStringLiteral("#e3b341");
    } else if (kpKnown) {
        phaseText = QStringLiteral("КП ЗАФИКСИРОВАН");
        phaseColor = QStringLiteral("#3fb950");
    } else if (m_session.executionPermissionObserved()) {
        phaseText = QStringLiteral("ОЖИДАНИЕ ОПОРНОГО КП");
        phaseColor = QStringLiteral("#e3b341");
    } else {
        phaseText = QStringLiteral("ПЛАН ПРИНЯТ, ОЖИДАНИЕ");
        phaseColor = QStringLiteral("#58a6ff");
    }
    m_phaseLabel->setText(QStringLiteral("● ") + phaseText);
    m_phaseLabel->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:600; letter-spacing:0.08em; color:%1;").arg(phaseColor));
    m_startTimeLabel->setText(hasPlan
        ? m_session.plan().t0Utc.toString(QStringLiteral("dd.MM HH:mm:ss"))
        : QStringLiteral("--:--:--"));
    m_startTimeLabel->setToolTip(hasPlan ? m_session.plan().t0Utc.toString(Qt::ISODateWithMs)
                                        : QString());

    m_uploadStatus->setText(QStringLiteral("ЦИКЛОГРАММА · %1 · upload_id: %2")
        .arg(m_session.uploadState(),
             m_session.uploadId().isEmpty() ? QStringLiteral("—") : m_session.uploadId()));
    m_kpStatus->setText(kpKnown
        ? QStringLiteral("КП · зафиксирован на счётчике платы: %1 мкс; относительный ноль = 0 мс")
              .arg(*m_session.kpTimeUs())
        : QStringLiteral("КП · нет подтверждённой опорной отметки; относительные факты не вычисляются"));
    m_mappingStatus->setText(QStringLiteral("КАРТА СОБЫТИЙ · %1")
        .arg(m_session.mapping().version.isEmpty()
            ? QStringLiteral("не загружена") : m_session.mapping().version));
    m_clockStatus->setText(QStringLiteral(
        "UTC БЦВМ при загрузке: %1 · синхронизация с платой не подтверждена")
        .arg(m_session.bcvmUtc().isEmpty()
            ? QStringLiteral("неизвестно") : m_session.bcvmUtc()));
    m_uploadStatus->setToolTip(m_session.cyclogramSha256().isEmpty()
        ? QString() : QStringLiteral("SHA-256 циклограммы: ") + m_session.cyclogramSha256());

    if (m_lastUploadId != m_session.uploadId()) {
        m_lastUploadId = m_session.uploadId();
        m_loggedStates.clear();
        m_timelineSignature.clear();
        m_eventLog->clear();
    }

    QVector<DisplayAction> rows;
    QStringList signatureParts;
    bool explicitKp = false;
    if (hasPlan) {
        for (const PlannedAction &action : m_session.plan().actions) {
            if (!m_session.mapping().actions.contains(action.key)) continue;
            rows.append({action, false});
            signatureParts << QStringLiteral("%1:%2:%3")
                .arg(action.ordinal).arg(action.offsetMs).arg(action.key);
            if (action.key == QStringLiteral("LIFT_OFF_CONTACT")) explicitKp = true;
        }
        if (m_session.mapping().kp.index >= 0 && !explicitKp)
            rows.append({{QStringLiteral("КП"), 0, 0}, true});
        std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
            return a.plan.offsetMs < b.plan.offsetMs;
        });
    }
    const QString signature = m_session.uploadId() + QStringLiteral("|") +
        m_session.mapping().version + QStringLiteral("|") +
        signatureParts.join(QStringLiteral("|")) + QStringLiteral("|kp:") +
        QString::number(int(m_session.mapping().kp.index >= 0 && !explicitKp));
    if (signature != m_timelineSignature) {
        m_timelineSignature = signature;
        QVector<EventRow> events;
        for (const auto &row : rows) {
            EventRow event;
            event.id = row.plan.ordinal;
            event.key = row.plan.key;
            event.time_ms = row.plan.offsetMs;
            event.hasChannels = true;
            events.append(event);
        }
        m_timeline->setEvents(events);
    }
    m_timeline->setVisible(!rows.isEmpty());
    m_eventCaption->setText(QStringLiteral("ОТСЛЕЖИВАЕМЫЕ СОБЫТИЯ · %1")
        .arg(rows.size()));
    if (rows.isEmpty()) {
        m_nextEventLabel->setText(hasPlan
            ? QStringLiteral("Нет назначенных событий · нужна карта соответствия")
            : QStringLiteral("Ожидание циклограммы от КАСУ ПП"));
    }

    // Keep the original multichannel subrows, now strictly restricted to
    // inputs assigned to a planned event by the approved mapping.
    int displayCount = 0;
    for (const auto &row : rows) {
        ++displayCount;
        if (!row.physicalKp) {
            const auto mapped = m_session.mapping().actions.value(row.plan.key);
            if (mapped.inputs.size() > 1) displayCount += mapped.inputs.size();
        }
    }
    if (m_eventTable->rowCount() != displayCount)
        m_eventTable->setRowCount(displayCount);

    int completed = 0, deviations = 0, waiting = 0, unevaluated = 0;
    bool foundAwaited = false;
    int tableRow = 0;
    for (const auto &row : rows) {
        const auto &action = row.plan;
        const QColor rowBg = row.physicalKp ? QColor("#132434") : QColor();
        QString facts = QStringLiteral("—"), delta = QStringLiteral("—");
        QString status, tooltip;
        QColor statusColor = kDim;
        QColor bg = rowBg;
        ActionMeasurement evaluation;
        QString stateKey = QString::number(action.ordinal) + QStringLiteral(":") + action.key;

        if (row.physicalKp) {
            status = kpKnown ? QStringLiteral("КП ЗАФИКСИРОВАН") : QStringLiteral("ОЖИДАНИЕ КП");
            statusColor = kpKnown ? kGreen : kAmber;
            if (kpKnown) facts = QStringLiteral("0.000");
            tooltip = QStringLiteral(
                "КП является физическим нулём аппаратной шкалы. "
                "Отклонение от планового UTC без подтверждённой синхронизации не вычисляется.");
            m_timeline->markEventFired(action.ordinal, kpKnown ? QStringLiteral("ok") : QString());
        } else {
            evaluation = m_session.measureAction(action);
            tooltip = evaluation.reason;
            using State = ActionMeasurement::State;
            if (evaluation.state == State::Measured) {
                // A multi-input event is complete on its LAST observed input.
                // Each input is evaluated separately against its own planned time.
                double lastMs = evaluation.channels.first().actualMs.value();
                bool inTolerance = true;
                for (const auto &sample : evaluation.channels) {
                    lastMs = std::max(lastMs, *sample.actualMs);
                    if (evaluation.toleranceMs &&
                        std::abs(*sample.deviationMs) > *evaluation.toleranceMs)
                        inTolerance = false;
                }
                facts = QString::number(lastMs, 'f', 3);
                delta = signedMs(lastMs - action.offsetMs);
                if (!evaluation.toleranceMs) {
                    status = QStringLiteral("ИЗМЕРЕНО · БЕЗ ДОПУСКА");
                    statusColor = kDim;
                    ++unevaluated;
                } else if (inTolerance) {
                    status = QStringLiteral("В ДОПУСКЕ");
                    statusColor = kGreen;
                    bg = QColor("#0f2d1a");
                } else {
                    status = QStringLiteral("ОТКЛОНЕНИЕ");
                    statusColor = kRed;
                    bg = QColor("#3d1515");
                    ++deviations;
                }
                tooltip = evaluation.channels.size() > 1
                    ? QStringLiteral("Факт группы — отметка последнего входа. "
                        "Допуск проверен отдельно по каждому назначенному входу.")
                    : QStringLiteral("Факт измерен относительно КП счётчиком платы.");
                ++completed;
                m_timeline->markEventFired(action.ordinal,
                    !evaluation.toleranceMs ? QString()
                    : inTolerance ? QStringLiteral("ok") : QStringLiteral("fail"));
                const QString logValue = status + QStringLiteral("|") + facts + QStringLiteral("|") + delta;
                if (m_loggedStates.value(stateKey) != logValue) {
                    m_eventLog->append(QStringLiteral(
                        "<span style=\"color:#8b949e;\">%1</span> %2<br/>"
                        "<span style=\"color:%3;\">%4 · факт %5 мс · Δ %6 мс</span>")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")).toHtmlEscaped(),
                             action.key.toHtmlEscaped(), statusColor.name(), status.toHtmlEscaped(),
                             facts, delta));
                }
                m_loggedStates.insert(stateKey, logValue);
            } else {
                m_timeline->markEventFired(action.ordinal, QString());
                if (evaluation.state == State::Partial) {
                    status = QStringLiteral("ЧАСТЬ ВХОДОВ");
                    statusColor = kAmber;
                    ++waiting;
                } else if (evaluation.state == State::WaitingForInputs) {
                    status = QStringLiteral("ОЖИДАНИЕ ОТМЕТКИ");
                    ++waiting;
                } else {
                    status = evaluation.reason;
                    statusColor = kAmber;
                    ++unevaluated;
                }
                if (!foundAwaited) {
                    m_nextEventLabel->setText(QStringLiteral("Без факта: ") + action.key);
                    foundAwaited = true;
                }
                m_loggedStates.remove(stateKey);
            }
        }

        cell(m_eventTable, tableRow, 0,
             row.physicalKp ? QStringLiteral("КП") : QString::number(action.ordinal),
             kDim, bg, Qt::AlignCenter);
        cell(m_eventTable, tableRow, 1,
             row.physicalKp ? QStringLiteral("КОНТАКТ ПОДЪЁМА · ОПОРНЫЙ НОЛЬ") : action.key,
             kNeutral, bg, Qt::AlignLeft | Qt::AlignVCenter, tooltip);
        cell(m_eventTable, tableRow, 2,
             row.physicalKp ? inputName(m_session.mapping().kp)
                 : inputNames(m_session.mapping().actions.value(action.key)),
             kDim, bg, Qt::AlignLeft | Qt::AlignVCenter);
        cell(m_eventTable, tableRow, 3, QString::number(action.offsetMs),
             kDim, bg, Qt::AlignRight | Qt::AlignVCenter);
        cell(m_eventTable, tableRow, 4, facts, kNeutral, bg, Qt::AlignRight | Qt::AlignVCenter,
             tooltip);
        cell(m_eventTable, tableRow, 5, delta,
             statusColor, bg, Qt::AlignRight | Qt::AlignVCenter, tooltip);
        cell(m_eventTable, tableRow, 6, status, statusColor, bg,
             Qt::AlignLeft | Qt::AlignVCenter, tooltip);
        ++tableRow;

        if (!row.physicalKp && evaluation.channels.size() > 1) {
            for (const auto &sample : evaluation.channels) {
                const QColor subBg("#080c12");
                const bool hasValue = sample.actualMs.has_value();
                QColor measuredColor = kDim;
                QString subStatus = hasValue ? QStringLiteral("ИЗМЕРЕНО")
                                             : QStringLiteral("НЕТ ОТМЕТКИ");
                if (hasValue && evaluation.toleranceMs) {
                    const bool good = std::abs(*sample.deviationMs) <= *evaluation.toleranceMs;
                    measuredColor = good ? kGreen : kRed;
                    subStatus = good ? QStringLiteral("В ДОПУСКЕ") : QStringLiteral("ОТКЛОНЕНИЕ");
                }
                cell(m_eventTable, tableRow, 0, QString(), kDim, subBg);
                cell(m_eventTable, tableRow, 1, QStringLiteral("    ↳ ") + inputName(sample.input),
                     kDim, subBg);
                cell(m_eventTable, tableRow, 2, inputName(sample.input), kDim, subBg);
                cell(m_eventTable, tableRow, 3, QString::number(action.offsetMs),
                     kDim, subBg, Qt::AlignRight | Qt::AlignVCenter);
                cell(m_eventTable, tableRow, 4,
                     hasValue ? QString::number(*sample.actualMs, 'f', 3) : QStringLiteral("—"),
                     kNeutral, subBg, Qt::AlignRight | Qt::AlignVCenter);
                cell(m_eventTable, tableRow, 5,
                     hasValue ? signedMs(*sample.deviationMs) : QStringLiteral("—"),
                     measuredColor, subBg, Qt::AlignRight | Qt::AlignVCenter);
                cell(m_eventTable, tableRow, 6, subStatus, measuredColor, subBg);
                ++tableRow;
            }
        }
    }
    if (!foundAwaited && !rows.isEmpty())
        m_nextEventLabel->setText(QStringLiteral("Назначенные события: %1").arg(rows.size()));
    m_summaryLabel->setText(QStringLiteral(
        "ФАКТ: %1 / %2    ·    ОТКЛОНЕНИЯ: %3    ·    ОЖИДАЮТ: %4    ·    БЕЗ ОЦЕНКИ: %5")
        .arg(completed).arg(rows.size() - int(m_session.mapping().kp.index >= 0 && !explicitKp))
        .arg(deviations).arg(waiting).arg(unevaluated));
    updatePlannedClock();
}
