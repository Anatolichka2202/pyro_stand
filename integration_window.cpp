#include "integration_window.h"
#include "timeline_widget.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDateTime>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSerialPortInfo>
#include <QSettings>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <cstdint>
#include <limits>

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

QString inputName(const InputRef &input, bool legacy = false, bool secondOnly = false) {
    static const QStringList labels = {
        QStringLiteral("Пиросвеча камеры"), QStringLiteral("Пиросвеча газогенератора"),
        QStringLiteral("Пиропатрон пусковой камеры"), QStringLiteral("Пусковой клапан Г"),
        QStringLiteral("Пусковой клапан О"), QStringLiteral("Отсечной клапан Г"),
        QStringLiteral("Отсечной клапан О"), QStringLiteral("КП")};
    if (legacy)
        return QStringLiteral("%1 / %2 · %3")
            .arg(input.index >= 8 || secondOnly ? QStringLiteral("БКД-Б") : QStringLiteral("БКД-А-1"))
            .arg(input.index % 8 + 1).arg(labels.at(input.index % 8));
    return QStringLiteral("ЯПС %1 / %2 %3")
        .arg(input.index / 8 + 1)
        .arg(input.index % 8 + 1)
        .arg(input.falling ? QStringLiteral("↓") : QStringLiteral("↑"));
}

QString inputNames(const ActionMapping &mapping, bool legacy = false, bool secondOnly = false) {
    QStringList names;
    for (const auto &input : mapping.inputs) names << inputName(input, legacy, secondOnly);
    return names.join(QStringLiteral(", "));
}

// The debug contract supplies event keys, not the legacy INI descriptions.
// Only the four labels confirmed by the operator design are expanded.
QString eventName(const QString &key) {
    if (key == QStringLiteral("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"))
        return QStringLiteral("Зажигание пиросвечей двигателей 9–12");
    if (key == QStringLiteral("IGNITE_PYRO_CANDLES_ENGINES_1_TO_8"))
        return QStringLiteral("Зажигание пиросвечей двигателей 1–8");
    if (key == QStringLiteral("CLOSE_MAIN_VALVES_ENGINES_9_TO_12"))
        return QStringLiteral("Закрытие главных клапанов двигателей 9–12");
    if (key == QStringLiteral("LIFT_OFF_CONTACT"))
        return QStringLiteral("Контакт подъёма");
    return key;
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
    chip->setObjectName(QStringLiteral("connectionChip"));
    chip->setStyleSheet(QStringLiteral(
        "QFrame#connectionChip { background:#161b22; border:1px solid #30363d; border-radius:4px; }"));
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
    const QString color = active ? QStringLiteral("#3fb950") : QStringLiteral("#484f58");
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
                                     QWidget *parent)
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

    m_session.start();
    refresh();
}

void IntegrationWindow::setupUi() {
    setWindowTitle(QStringLiteral("Пиростенд — контроль исполнения циклограммы"));
    resize(1280, 800);
    setMinimumSize(980, 600);

    // One 1280×800 operator canvas, as in FullScreen-v1.
    auto *surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("operatorSurface"));
    setCentralWidget(surface);
    auto *main = new QVBoxLayout(surface);
    main->setSpacing(10);
    main->setContentsMargins(20, 14, 20, 14);

    auto *top = new QHBoxLayout;
    top->setSpacing(16);
    auto *clock = new QVBoxLayout;
    clock->setSpacing(4);
    m_clockCaption = new QLabel(QStringLiteral("ДО ЗАПУСКА · ЧАСЫ ПК (UTC)"), surface);
    m_clockCaption->setStyleSheet(QStringLiteral(
        "font-size:13px; letter-spacing:0.15em; color:#8b949e;"));
    m_timerLabel = new QLabel(QStringLiteral("--:--"), surface);
    m_timerLabel->setObjectName(QStringLiteral("timerLabel"));
    m_timerLabel->setStyleSheet(QStringLiteral(
        "font-size:52px; font-weight:600; color:#484f58; font-family:'JetBrains Mono';"));
    auto *phase = new QHBoxLayout;
    m_phaseLabel = new QLabel(QStringLiteral("● ОЖИДАНИЕ"), surface);
    m_phaseLabel->setStyleSheet(QStringLiteral("font-size:13px; font-weight:600; color:#8b949e;"));
    m_nextEventLabel = new QLabel(QStringLiteral("Нет циклограммы"), surface);
    m_nextEventLabel->setStyleSheet(QStringLiteral("font-size:12px; color:#6e7681;"));
    phase->addWidget(m_phaseLabel);
    phase->addWidget(m_nextEventLabel, 1);
    clock->addStretch();
    clock->addWidget(m_clockCaption);
    clock->addWidget(m_timerLabel);
    clock->addLayout(phase);
    clock->addStretch();
    top->addLayout(clock, 1);

    auto *controls = new QWidget(surface);
    controls->setObjectName(QStringLiteral("ctrlCard"));
    controls->setMinimumWidth(250);
    controls->setMaximumWidth(340);
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
    auto *settingsCaption = new QLabel(QStringLiteral("НАСТРОЙКА И УПРАВЛЕНИЕ"), controls);
    settingsCaption->setStyleSheet(QStringLiteral(
        "font-size:11px; font-weight:600; color:#8b949e; border:0; background:transparent;"));
    ctrl->addWidget(settingsCaption);

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
    m_resetButton = reset;
    reset->setObjectName(QStringLiteral("resetBtn"));
    ctrl->addWidget(reset);

    auto *map = new QPushButton(QStringLiteral("ПОДКЛЮЧЕНИЕ ПИРОТЕСТЕРА"), controls);
    map->setObjectName(QStringLiteral("testerConfigurationBtn"));
    auto *save = new QPushButton(QStringLiteral("СОХРАНИТЬ ОТЧЁТ"), controls);
    map->setObjectName(QStringLiteral("mappingBtn"));
    save->setObjectName(QStringLiteral("reportBtn"));
    ctrl->addWidget(map);
    ctrl->addWidget(save);
    top->addWidget(controls);
    main->addLayout(top);

    connect(reset, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Сброс измерений"),
            m_session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial
                ? QStringLiteral("Начать новый сбор старого пиротестера? Буфер COM и накопленные отметки будут очищены. Все входы должны быть неактивны.")
                : QStringLiteral("Отправить аппаратную команду сброса пиротестеру? "
                           "Полученные отметки платы будут сброшены. "
                           "Для нового прогона потребуется пустой кадр после сброса."));
        if (answer == QMessageBox::Yes) m_session.resetBoard();
    });
    connect(map, &QPushButton::clicked, this, &IntegrationWindow::configureTester);
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
    auto *meta = new QFrame(surface);
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
    m_timeline = new TimelineWidget(surface);
    m_timeline->setObjectName(QStringLiteral("eventTimeline"));
    m_timeline->setVisible(false);
    main->addWidget(m_timeline);

    m_eventCaption = new QLabel(QStringLiteral("ОТСЛЕЖИВАЕМЫЕ СОБЫТИЯ"), surface);
    m_eventCaption->setStyleSheet(QStringLiteral(
        "font-size:12px; letter-spacing:0.2em; color:#8b949e;"));
    main->addWidget(m_eventCaption);

    m_eventTable = new QTableWidget(0, 7, surface);
    m_eventTable->setObjectName(QStringLiteral("eventTable"));
    m_eventTable->setHorizontalHeaderLabels({
        QStringLiteral("#"), QStringLiteral("СОБЫТИЕ"),
        QStringLiteral("КАНАЛЫ"), QStringLiteral("ПЛАН МС"),
        QStringLiteral("ФАКТ МС"), QStringLiteral("ОТКЛ МС"),
        QStringLiteral("СТАТУС")});
    auto *header = m_eventTable->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int col = 2; col < 7; ++col)
        header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setAlternatingRowColors(true);
    m_eventTable->verticalHeader()->setDefaultSectionSize(28);
    m_eventTable->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_eventTable->verticalHeader()->hide();
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_eventTable->setFocusPolicy(Qt::NoFocus);
    main->addWidget(m_eventTable);

    auto *summary = new QFrame(surface);
    summary->setStyleSheet(QStringLiteral(
        "QFrame { background:#161b22; border-top:2px solid #30363d; }"));
    auto *summaryLay = new QHBoxLayout(summary);
    summaryLay->setContentsMargins(14, 8, 14, 8);
    m_summaryLabel = new QLabel(summary);
    m_summaryLabel->setStyleSheet(QStringLiteral(
        "color:#e6edf3; font-size:12px; font-weight:600; border:0;"));
    summaryLay->addWidget(m_summaryLabel);
    main->addWidget(summary);

    auto *journalCaption = new QLabel(QStringLiteral("ЖУРНАЛ СЕССИИ"), surface);
    journalCaption->setObjectName(QStringLiteral("journalCaption"));
    journalCaption->setStyleSheet(QStringLiteral(
        "font-size:11px; font-weight:600; color:#8b949e;"));
    main->addWidget(journalCaption);

    m_eventLog = new QTextEdit(surface);
    m_eventLog->setObjectName(QStringLiteral("eventJournal"));
    m_eventLog->setReadOnly(true);
    m_eventLog->setMinimumHeight(105);
    m_eventLog->setMaximumHeight(190);
    m_eventLog->setStyleSheet(QStringLiteral(
        "QTextEdit { background:#161b22; border:1px solid #30363d; border-radius:4px;"
        "color:#c8d0dc; font-size:11px; font-family:'JetBrains Mono'; padding:4px 8px; }"));
    main->addWidget(m_eventLog, 1);
}

void IntegrationWindow::appendJournal(const QString &level, const QString &message,
                                      const QString &color) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    // Qt rich text has no reliable border-left; use the spec's visible ▌ glyph.
    m_eventLog->append(QStringLiteral(
        "<span style=\"color:%1;\">▌</span>  "
        "<span style=\"color:#8b949e;\">%2</span>  "
        "<span style=\"color:%1; font-weight:600;\">%3</span>  "
        "<span style=\"color:#c8d0dc;\">%4</span>")
        .arg(color, stamp.toHtmlEscaped(), level.toHtmlEscaped(), message.toHtmlEscaped()));
}

void IntegrationWindow::appendMessage(const QString &message) {
    const bool error = message.contains(QStringLiteral("ошиб"), Qt::CaseInsensitive) ||
                       message.contains(QStringLiteral("не удалось"), Qt::CaseInsensitive) ||
                       message.contains(QStringLiteral("сбой"), Qt::CaseInsensitive);
    appendJournal(error ? QStringLiteral("ОШИБКА") : QStringLiteral("INFO"), message,
                  error ? QStringLiteral("#f85149") : QStringLiteral("#8b949e"));
}

void IntegrationWindow::updatePlannedClock() {
    const auto measured = m_session.measuredPositionMs();
    if (measured) {
        const bool legacy = m_session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        m_clockCaption->setText(legacy ? QStringLiteral("ПОСЛЕ КП · СЧЁТЧИК COM")
                                      : QStringLiteral("ПОСЛЕДНЯЯ ОТМЕТКА ПЛАТЫ · КП"));
        m_timerLabel->setText(QStringLiteral("+%1.%2")
            .arg(qint64(*measured) / 1000).arg(qint64(*measured) % 1000, 3, 10, QChar('0')));
        m_timerLabel->setStyleSheet(QStringLiteral(
            "font-size:64px; font-weight:700; font-family:'JetBrains Mono'; color:#3fb950;"));
        m_timerLabel->setToolTip(legacy
            ? QStringLiteral("Индекс последнего принятого байта минус индекс КП8 того же COM; один байт = 1 мс")
            : QStringLiteral("Последняя известная отметка сопоставленного входа относительно КП. Между событиями часы ПК её не продвигают."));
        m_timeline->setPlayheadMs(qint64(*measured));
        return;
    }
    m_timeline->setPlayheadMs(INT64_MIN);
    if (!m_session.hasPlan() || !m_session.plan().t0Utc.isValid()) {
        m_timerLabel->setText(QStringLiteral("--:--"));
        m_timerLabel->setStyleSheet(QStringLiteral(
            "font-size:52px; font-weight:600; color:#484f58; font-family:'JetBrains Mono';"));
        m_clockCaption->setText(QStringLiteral("ДО ЗАПУСКА · ЧАСЫ ПК (UTC)"));
        return;
    }
    // This PC UTC clock is not the tester's physical microsecond counter.
    const qint64 deltaMs = QDateTime::currentDateTimeUtc().msecsTo(m_session.plan().t0Utc);
    const bool before = deltaMs > 0;
    const bool kpConfirmed = m_session.measurementEligible() &&
                             m_session.kpTimeUs().has_value();
    m_clockCaption->setText(before ? QStringLiteral("ДО ЗАПУСКА · ЧАСЫ ПК (UTC)")
        : QStringLiteral("ПОСЛЕ ПЛАНОВОГО T0 · ЧАСЫ ПК (UTC)"));
    const qint64 seconds = qAbs(deltaMs) / 1000;
    const QString display = seconds >= 3600
        ? QStringLiteral("%1:%2:%3")
            .arg(seconds / 3600, 2, 10, QChar('0'))
            .arg((seconds / 60) % 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'))
        : QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QChar('0'))
            .arg(seconds % 60, 2, 10, QChar('0'));
    m_timerLabel->setText(display);
    const QString color = kpConfirmed ? QStringLiteral("#3fb950")
        : (before && m_session.uploadState() == QStringLiteral("CONFIRMED")
           ? QStringLiteral("#e3b341") : QStringLiteral("#8b949e"));
    m_timerLabel->setStyleSheet(QStringLiteral(
        "font-size:52px; font-weight:600; font-family:'JetBrains Mono'; color:%1;").arg(color));
    m_timerLabel->setToolTip(QStringLiteral(
        "Время до/после планового T0 UTC по часам ПК. "
        "Зелёный подтверждает наличие КП, но не означает синхронизацию ПК с платой."));
}

void IntegrationWindow::configureTester() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Подключение пиротестера и БКД"));
    auto *layout = new QFormLayout(&dialog);
    auto options = m_session.options();
    auto *transport = new QComboBox(&dialog);
    transport->addItem(QStringLiteral("Новый — Ethernet"), int(IntegrationSession::BoardTransport::Udp));
    transport->addItem(QStringLiteral("Новый — RS-485"), int(IntegrationSession::BoardTransport::Serial));
    transport->addItem(QStringLiteral("Старый — COM, 1 байт = 1 мс"), int(IntegrationSession::BoardTransport::LegacySerial));
    transport->setCurrentIndex(transport->findData(int(options.boardTransport)));
    auto *port = new QComboBox(&dialog);
    port->setEditable(true);
    for (const auto &info : QSerialPortInfo::availablePorts()) port->addItem(info.portName());
    port->setCurrentText(options.serialPort);
    auto *secondPort = new QComboBox(&dialog);
    secondPort->setEditable(true);
    secondPort->addItem(QStringLiteral(""));
    for (const auto &info : QSerialPortInfo::availablePorts()) secondPort->addItem(info.portName());
    secondPort->setCurrentText(options.secondLegacySerialPort);
    auto *address = new QLineEdit(options.boardIp.toString(), &dialog);
    auto *first = new QComboBox(&dialog);
    auto *second = new QComboBox(&dialog);
    auto *kp = new QComboBox(&dialog);
    for (auto *box : {first, second, kp}) {
        box->addItem(QStringLiteral("Не назначен"), 0);
        for (int yps = 1; yps <= 12; ++yps)
            box->addItem(QStringLiteral("ЯПС %1").arg(yps), yps);
    }
    first->setCurrentIndex(first->findData(options.firstStageYps));
    second->setCurrentIndex(second->findData(options.secondStageYps));
    kp->setCurrentIndex(kp->findData(options.kpYps));
    auto *block = new QComboBox(&dialog);
    block->addItem(QStringLiteral("БКД-А-1 (первая ступень)"), false);
    block->addItem(QStringLiteral("БКД-Б (вторая ступень)"), true);
    block->setCurrentIndex(options.legacySecondStage ? 1 : 0);
    layout->addRow(QStringLiteral("Пиротестер"), transport);
    layout->addRow(QStringLiteral("COM · 115200, 8N1"), port);
    layout->addRow(QStringLiteral("IP новой платы"), address);
    layout->addRow(QStringLiteral("БКД-А-1 · пункты 3, 5, 16"), first);
    layout->addRow(QStringLiteral("БКД-Б · пункты 20, 21, 22"), second);
    layout->addRow(QStringLiteral("КП · вход 8"), kp);
    layout->addRow(QStringLiteral("БКД на старой плате"), block);
    layout->addRow(QStringLiteral("Второй COM · БКД-Б (необязательно)"), secondPort);
    auto *note = new QLabel(QStringLiteral(
        "1–2: пиросвечи; 3–5: пусковая группа; 6–7: отсечные клапаны.\n"
        "Назначьте ЯПС по фактическому подключению. Времена берутся из циклограммы КАСУ ПП.\n"
        "Два старых COM: первый — А-1, второй — Б; КП8 нужен в обоих потоках.\n"
        "После изменения подключения начните новый сбор измерений."), &dialog);
    note->setWordWrap(true);
    layout->addRow(note);
    const auto update = [=] {
        const auto value = IntegrationSession::BoardTransport(transport->currentData().toInt());
        const bool legacy = value == IntegrationSession::BoardTransport::LegacySerial;
        port->setEnabled(value != IntegrationSession::BoardTransport::Udp);
        address->setEnabled(!legacy);
        first->setEnabled(!legacy); second->setEnabled(!legacy); kp->setEnabled(!legacy);
        block->setEnabled(legacy);
        secondPort->setEnabled(legacy);
    };
    connect(transport, &QComboBox::currentIndexChanged, &dialog, update);
    update();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        options.boardTransport = IntegrationSession::BoardTransport(transport->currentData().toInt());
        options.serialPort = port->currentText().trimmed();
        options.secondLegacySerialPort = secondPort->currentText().trimmed();
        options.firstStageYps = first->currentData().toInt();
        options.secondStageYps = second->currentData().toInt();
        options.kpYps = kp->currentData().toInt();
        options.legacySecondStage = block->currentData().toBool();
        const bool legacy = options.boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        if ((options.boardTransport != IntegrationSession::BoardTransport::Udp && options.serialPort.isEmpty()) ||
            (!legacy && !options.boardIp.setAddress(address->text().trimmed())) ||
            (!legacy && options.firstStageYps != 0 && options.firstStageYps == options.secondStageYps) ||
            (legacy && !options.secondLegacySerialPort.isEmpty() && options.serialPort.compare(options.secondLegacySerialPort, Qt::CaseInsensitive) == 0)) {
            QMessageBox::warning(&dialog, QStringLiteral("Подключение"),
                QStringLiteral("Укажите COM/IP. Два БКД должны иметь разные группы ЯПС."));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    QSettings settings(QStringLiteral("PyroStand"), QStringLiteral("Operator"));
    settings.setValue(QStringLiteral("tester/transport"), int(options.boardTransport));
    settings.setValue(QStringLiteral("tester/serial"), options.serialPort);
    settings.setValue(QStringLiteral("tester/secondSerial"), options.secondLegacySerialPort);
    settings.setValue(QStringLiteral("tester/ip"), options.boardIp.toString());
    settings.setValue(QStringLiteral("tester/firstYps"), options.firstStageYps);
    settings.setValue(QStringLiteral("tester/secondYps"), options.secondStageYps);
    settings.setValue(QStringLiteral("tester/kpYps"), options.kpYps);
    settings.setValue(QStringLiteral("tester/legacySecondStage"), options.legacySecondStage);
    m_loggedStates.clear();
    m_timelineSignature.clear();
    m_eventLog->clear();
    m_session.configureBoard(options);
}

void IntegrationWindow::refresh() {
    const bool legacy = m_session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial;
    const bool secondOnly = legacy && m_session.options().secondLegacySerialPort.isEmpty() && m_session.options().legacySecondStage;
    m_resetButton->setText(legacy ? QStringLiteral("↺  НАЧАТЬ НОВЫЙ СБОР")
                                  : QStringLiteral("↺  СБРОСИТЬ ИЗМЕРЕНИЯ ПЛАТЫ"));
    const bool hasPlan = m_session.hasPlan();
    const bool eligible = m_session.measurementEligible();
    const bool kpKnown = eligible && m_session.kpTimeUs().has_value();
    const bool uploaded = m_session.uploadState() == QStringLiteral("CONFIRMED");

    setChip(m_debugDot, m_debugStatus, m_session.debugConnected(),
            m_session.debugConnected() ? QStringLiteral("подключён") : QStringLiteral("нет связи"));
    setChip(m_boardDot, m_boardStatus, m_session.boardReceiving(),
            m_session.boardReceiving() ? (legacy ? QStringLiteral("маски идут · 1 байт/мс") : QStringLiteral("кадры идут"))
                                       : (legacy ? QStringLiteral("нет COM-данных") : QStringLiteral("нет кадров")));
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

    m_uploadStatus->setText(QStringLiteral("ЦИКЛОГРАММА · %1")
        .arg(uploaded ? QStringLiteral("ЗАГРУЗКА ПОДТВЕРЖДЕНА")
                      : QStringLiteral("ЗАГРУЗКА НЕ ПОДТВЕРЖДЕНА")));
    m_uploadStatus->setToolTip(QStringLiteral("Состояние: %1\nID загрузки: %2")
        .arg(m_session.uploadState(), m_session.uploadId()));
    m_kpStatus->setText(kpKnown
        ? QStringLiteral("КП · зафиксирован на счётчике платы: %1 мкс; относительный ноль = 0 мс")
              .arg(*m_session.kpTimeUs())
        : QStringLiteral("КП · нет подтверждённой опорной отметки; относительные факты не вычисляются"));
    m_mappingStatus->setText(QStringLiteral("ПОДКЛЮЧЕНИЕ БКД · %1")
        .arg(m_session.mapping().version.isEmpty()
            ? QStringLiteral("не загружена") : m_session.mapping().version));
    if (!m_session.mapping().warnings.isEmpty())
        m_mappingStatus->setText(m_mappingStatus->text() + QStringLiteral(" · НЕСООТВЕТСТВИЕ ПУНКТОВ"));
    m_mappingStatus->setToolTip(m_session.mapping().warnings.join('\n'));
    m_clockStatus->setText(QStringLiteral(
        "UTC БЦВМ при загрузке: %1 · синхронизация с платой не подтверждена")
        .arg(m_session.bcvmUtc().isEmpty()
            ? QStringLiteral("неизвестно") : m_session.bcvmUtc()));
    if (!m_session.cyclogramSha256().isEmpty())
        m_uploadStatus->setToolTip(m_uploadStatus->toolTip() +
            QStringLiteral("\nSHA-256 циклограммы: ") + m_session.cyclogramSha256());

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
            ? QStringLiteral("Нет назначенных событий · проверьте подключение БКД и пункты циклограммы")
            : QStringLiteral("Ожидание циклограммы от КАСУ ПП"));
    }

    // Show only assigned inputs as subrows; this is not the raw 96-channel table.
    int displayCount = 0;
    for (const auto &row : rows) {
        ++displayCount;
        if (!row.physicalKp) {
            const auto mapped = m_session.mapping().actions.value(row.plan.key);
            if (mapped.inputs.size() > 1)
                displayCount += mapped.inputs.size();
        }
    }
    if (m_eventTable->rowCount() != displayCount)
        m_eventTable->setRowCount(displayCount);
    const int height = m_eventTable->horizontalHeader()->sizeHint().height() +
                       8 + std::clamp(displayCount, 2, 12) * 28;
    m_eventTable->setMaximumHeight((std::max)(118, height));

    int completed = 0, deviations = 0, waiting = 0, unevaluated = 0;
    double maxDeviationMs = 0.0;
    double latestMeasuredMs = -std::numeric_limits<double>::infinity();
    bool foundAwaited = false;
    int tableRow = 0;
    for (const auto &row : rows) {
        const auto &action = row.plan;
        const QColor rowBg;
        QString facts = QStringLiteral("—"), delta = QStringLiteral("—");
        QString status, tooltip;
        QColor statusColor = kDim;
        QColor bg = rowBg;
        bool nextPending = false;
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
            if (!kpKnown && !foundAwaited) {
                nextPending = true;
                foundAwaited = true;
                m_nextEventLabel->setText(QStringLiteral("Ожидание контакта подъёма"));
            }
        } else {
            evaluation = m_session.measureAction(action);
            if (evaluation.channels.isEmpty()) {
                const auto mapped = m_session.mapping().actions.value(action.key);
                for (const auto &input : mapped.inputs)
                    evaluation.channels.append({input, std::nullopt, std::nullopt});
            }
            tooltip = evaluation.reason;
            using State = ActionMeasurement::State;
            if (evaluation.state == State::Measured) {
                // A multi-input event is complete on its LAST observed input.
                // Each input is evaluated separately against its own planned time.
                double lastMs = evaluation.channels.first().actualMs.value();
                bool inTolerance = true;
                for (const auto &sample : evaluation.channels) {
                    lastMs = std::max(lastMs, *sample.actualMs);
                    latestMeasuredMs = std::max(latestMeasuredMs, *sample.actualMs);
                    maxDeviationMs = std::max(maxDeviationMs, std::abs(*sample.deviationMs));
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
                    appendJournal(QStringLiteral("СОБЫТ."),
                                  QStringLiteral("%1 · %2 · факт %3 мс · Δ %4 мс")
                                      .arg(eventName(action.key), status, facts, delta),
                                  statusColor.name());
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
                for (const auto &sample : evaluation.channels)
                    if (sample.actualMs)
                        latestMeasuredMs = std::max(latestMeasuredMs, *sample.actualMs);
                if (!foundAwaited) {
                    m_nextEventLabel->setText(QStringLiteral("Без факта: ") + eventName(action.key));
                    nextPending = true;
                    foundAwaited = true;
                }
                m_loggedStates.remove(stateKey);
            }
        }

        if (!row.physicalKp) {
            QStringList details;
            for (const auto &sample : evaluation.channels)
                details << QStringLiteral("%1 · %2")
                    .arg(inputName(sample.input),
                         sample.actualMs ? QStringLiteral("%1 мс · Δ %2 мс")
                             .arg(*sample.actualMs, 0, 'f', 3)
                             .arg(*sample.deviationMs, 0, 'f', 3)
                             : QStringLiteral("нет отметки"));
            tooltip = QStringLiteral("Ключ: %1\n%2").arg(action.key, tooltip);
            if (!details.isEmpty()) tooltip += QStringLiteral("\n") + details.join(QStringLiteral("\n"));
        }
        if (nextPending) bg = QColor("#1c2d3a");
        cell(m_eventTable, tableRow, 0,
             row.physicalKp ? QStringLiteral("КП") : QString::number(action.ordinal),
             nextPending ? QColor("#0d1117") : kDim,
             nextPending ? QColor("#58a6ff") : bg, Qt::AlignCenter);
        cell(m_eventTable, tableRow, 1,
             row.physicalKp ? QStringLiteral("КОНТАКТ ПОДЪЁМА · ОПОРНЫЙ НОЛЬ") : eventName(action.key),
             kNeutral, bg, Qt::AlignLeft | Qt::AlignVCenter, tooltip);
        cell(m_eventTable, tableRow, 2,
             row.physicalKp ? inputName(m_session.mapping().kp, legacy, secondOnly)
                 : inputNames(m_session.mapping().actions.value(action.key), legacy, secondOnly),
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
                cell(m_eventTable, tableRow, 1, QStringLiteral("    ↳ ") + inputName(sample.input, legacy, secondOnly),
                     kDim, subBg);
                cell(m_eventTable, tableRow, 2, inputName(sample.input, legacy, secondOnly), kDim, subBg);
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
        "РЕЗУЛЬТАТ · ИЗМЕРЕНО: %1 / %2  ·  ВНЕ ДОПУСКА: %3  ·  "
        "ОЖИДАНИЕ: %4  ·  БЕЗ ОЦЕНКИ: %5  ·  МАКС. |Δ|: %6 мс")
        .arg(completed).arg(rows.size() - int(m_session.mapping().kp.index >= 0 && !explicitKp))
        .arg(deviations).arg(waiting).arg(unevaluated)
        .arg(completed ? QString::number(maxDeviationMs, 'f', 3) : QStringLiteral("—")));
    // The marker tracks the latest physical relative measurement, not PC UTC.
    m_timeline->setPlayheadMs(kpKnown && std::isfinite(latestMeasuredMs)
        ? int64_t(std::llround(latestMeasuredMs))
        : std::numeric_limits<int64_t>::min());
    updatePlannedClock();
}
