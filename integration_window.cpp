#include "integration_window.h"
#include "command_catalog.h"
#include "timeline_widget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSerialPortInfo>
#include <QSettings>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QSizePolicy>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QToolButton>
#include <QMenu>
#include <QTextDocument>
#include <QFileInfo>
#include <QCoreApplication>
#include <QProcess>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QTextLayout>
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

void drawItemFocus(QPainter *painter, const QStyleOptionViewItem &option) {
    if (!(option.state & QStyle::State_HasFocus)) return;
    painter->setPen(QPen(QColor(QStringLiteral("#81b6d0")), 1, Qt::DotLine));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(option.rect.adjusted(1, 1, -2, -2));
}

// Название события читается первым; план и прибор остаются второй, спокойной строкой.
class EventTitleDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        QFont title(option.font); title.setPixelSize(16);
        QFont detail(option.font); detail.setPixelSize(12);
        size.setHeight(QFontMetrics(title).height() * 2 + QFontMetrics(detail).height() + 14);
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        QStyleOptionViewItem background(option);
        initStyleOption(&background, index);
        // initStyleOption заменяет перевод строки разделителем Unicode; берём исходный текст.
        const auto parts = index.data(Qt::DisplayRole).toString().split(u'\n');
        background.text.clear();
        const auto *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, option.widget);
        painter->setClipRect(option.rect);
        const QRect content = option.rect.adjusted(8, 5, -8, -4);
        QFont titleFont(option.font); titleFont.setPixelSize(16);
        painter->setFont(titleFont);
        painter->setPen(kNeutral);
        const QFontMetrics metrics(titleFont);
        const QString title = parts.value(0);
        QTextLayout layout(title, titleFont);
        QTextOption wrap; wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(wrap);
        layout.beginLayout();
        auto line = layout.createLine();
        if (line.isValid()) line.setLineWidth(std::max(1, content.width()));
        layout.endLayout();
        if (line.isValid()) {
            line.draw(painter, content.topLeft());
            const QString rest = title.mid(line.textLength());
            if (!rest.isEmpty())
                painter->drawText(content.left(), content.top() + metrics.height() + metrics.ascent(),
                    metrics.elidedText(rest, Qt::ElideRight, content.width()));
        }
        QFont detailFont(option.font); detailFont.setPixelSize(12);
        painter->setFont(detailFont); painter->setPen(kDim);
        painter->drawText(QRect(content.left(), content.bottom() - 14, content.width(), 15),
            Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(detailFont).elidedText(parts.value(1), Qt::ElideRight, content.width()));
        drawItemFocus(painter, option);
        painter->restore();
    }
};

class ResultStatusDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QFont font(option.font); font.setPixelSize(12); font.setWeight(QFont::Medium);
        return {QFontMetrics(font).horizontalAdvance(index.data().toString()) + 28, 30};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setClipRect(option.rect);
        const QColor color = index.data(Qt::ForegroundRole).value<QBrush>().color();
        QColor fill = color; fill.setAlpha(24);
        const QRect badge = option.rect.adjusted(6, (option.rect.height() - 24) / 2, -6,
            -(option.rect.height() - 24) / 2);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen); painter->setBrush(fill);
        painter->drawRoundedRect(badge, 4, 4);
        QFont font(option.font); font.setPixelSize(12); font.setWeight(QFont::Medium);
        painter->setFont(font); painter->setPen(color);
        painter->drawText(badge.adjusted(4, 0, -4, 0), Qt::AlignCenter,
            QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, std::max(0, badge.width() - 8)));
        drawItemFocus(painter, option);
        painter->restore();
    }
};

QString signedMs(double value) {
    if (std::abs(value) < 0.0005) value = 0.0;
    return (value > 0.0 ? QStringLiteral("+") : QString()) +
           QString::number(value, 'f', 3);
}

// Название для оператора, если в строке ПЗ нет комментария.
QString eventName(const QString &key) {
    if (key == QStringLiteral("IGNITE_PYRO_CANDLES_ENGINES_9_TO_12"))
        return QStringLiteral("Зажигание пиросвечей двигателей 9–12");
    if (key == QStringLiteral("IGNITE_PYRO_CANDLES_ENGINES_1_TO_8"))
        return QStringLiteral("Зажигание пиросвечей двигателей 1–8");
    if (key == QStringLiteral("CLOSE_MAIN_VALVES_ENGINES_9_TO_12"))
        return QStringLiteral("Закрытие главных клапанов двигателей 9–12");
    if (key == QStringLiteral("LIFT_OFF_CONTACT"))
        return QStringLiteral("Контакт подъёма");
    if(key=="FIRE_STARTING_CHAMBER_ENGINES_9_TO_12")return QStringLiteral("Пусковая камера и клапаны");
    if(key=="IGNITE_PYRO_CANDLE")return QStringLiteral("Зажигание пиросвечей");
    if(key=="OPEN_OXIDIZER_AND_FUEL_VALVES")return QStringLiteral("Открытие пусковых клапанов");
    if(key=="SHUTDOWN_ENGINE_BLOCK_B")return QStringLiteral("Закрытие отсечных клапанов");
    return key;
}

QString eventName(const PlannedAction &action) {
    // Комментарий циклограммы — исходное описание оператора.
    // Добавленные краткие примечания оставляем после основного названия.
    const QString comment = action.comment.trimmed();
    if (!comment.isEmpty()) {
        const QString description = comment.section(QStringLiteral(" · "), 0, 0).trimmed();
        if (!description.isEmpty()) return description;
    }
    return eventName(action.key);
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
    item->setToolTip(Qt::convertFromPlainText(tip));
}

QFrame *statusChip(QWidget *parent, const QString &title, QLabel **dotOut, QLabel **statusOut) {
    auto *chip = new QFrame(parent);
    chip->setObjectName(QStringLiteral("connectionChip"));
    chip->setStyleSheet(QStringLiteral(
        "QFrame#connectionChip { background:#161b22; border:1px solid #30363d; border-radius:4px; }"));
    auto *lay = new QHBoxLayout(chip);
    lay->setContentsMargins(8, 5, 8, 5);
    lay->setSpacing(6);
    auto *dot = new QLabel(chip);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(QStringLiteral("background:#484f58; border:0; border-radius:4px;"));
    auto *col = new QHBoxLayout;
    col->setSpacing(6);
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


qint64 presentationOffset(const IntegrationSession &session, const PlannedAction &action) {
    const auto kp = StandProtocol::plannedKpOffset(session.plan(), session.mapping());
    return qint64(action.offsetMs) - kp.value_or(0);
}

} // Конец пространства имён.

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
    m_clockTimer.setInterval(33);
    connect(&m_clockTimer, &QTimer::timeout, this, &IntegrationWindow::updatePlannedClock);
    connect(&m_clockTimer, &QTimer::timeout, this, [this] {
        if (m_liveDialog && m_liveDialog->isVisible()) refreshLive();
        // Автоматическое завершение обрабатывается отдельно в IntegrationSession.
        // Завершение уже вызывает changed() и не требует постоянного обновления.
    });
    m_clockTimer.start();

    m_session.start();
    configurePeer();
    refresh();
}

void IntegrationWindow::setupUi() {
    setWindowTitle(QStringLiteral("Пиростенд — поканальная проверка циклограммы"));
    resize(1280, 800);
    setMinimumSize(1100, 700);
    setStyleSheet("QMainWindow,QWidget#operatorSurface{background:#10161c;color:#e8edf1;} "
                  "QLabel{color:#a6b3be;font-family:'Segoe UI';font-size:12px;} "
                  "QPushButton{background:#19222b;color:#e8edf1;border:1px solid "
                  "#35434f;padding:5px 9px;} QPushButton:focus{border:1px solid #81b6d0;} "
                  "QDialog,QTextEdit{background:#10161c;color:#e8edf1;} "
                  "QLineEdit,QComboBox{background:#19222b;color:#e8edf1;border:1px solid #35434f;padding:4px;} "
                  "QLineEdit:focus,QComboBox:focus{border:1px solid #81b6d0;} "
                  "QLineEdit:disabled,QComboBox:disabled{background:#10161c;color:#8b949e;} "
                  "QComboBox QAbstractItemView{background:#19222b;color:#e8edf1;selection-background-color:#35434f;} "
                  "QCheckBox{color:#a6b3be;}"
                  "QToolButton{background:#19222b;color:#e8edf1;border:1px solid #35434f;padding:5px;}");
    auto *surface = new QWidget(this);
    surface->setObjectName("operatorSurface");
    setCentralWidget(surface);
    auto *main = new QVBoxLayout(surface);
    main->setContentsMargins(14, 10, 14, 10);
    main->setSpacing(5);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("ПИРОСТЕНД"), surface);
    title->setStyleSheet("font-size:17px;color:#81b6d0");
    header->addWidget(title);
    header->addStretch();
    m_startTimeLabel = new QLabel(surface);
    auto *map = new QPushButton(QStringLiteral("Подключение"), surface);
    map->setObjectName("testerConfigurationBtn");
    m_resetButton = new QPushButton(QStringLiteral("Новый сбор"), surface);
    m_resetButton->setObjectName("resetBtn");
    auto *finish = new QPushButton(QStringLiteral("Завершить сбор"), surface);
    m_finishButton = finish;
    finish->setObjectName("finishCollectionBtn");
    auto *live = new QPushButton(QStringLiteral("Живые каналы"), surface);
    live->setObjectName("liveChannelsBtn");
    auto *save = new QPushButton(QStringLiteral("Отчёт"), surface);
    save->setObjectName("reportBtn");
    auto *free = new QPushButton(QStringLiteral("Свободная запись…"), surface);
    free->setObjectName(QStringLiteral("freeRecordingBtn"));
    free->setToolTip(QStringLiteral("Регистрация входов без ПЗ и КАСУПП; КП необязателен"));
    connect(free, &QPushButton::clicked, this, &IntegrationWindow::openFreeRecording);
    auto *log = new QPushButton(QStringLiteral("Журнал"), surface);
    log->setObjectName("journalBtn");
    auto *planButton = new QPushButton(QStringLiteral("Полный план"), surface);
    planButton->setObjectName("fullPlanBtn");
    for (auto *button : {map, m_resetButton, finish, live, free, save})
        header->addWidget(button);
    auto *more=new QToolButton(surface);
    more->setText(QStringLiteral("Ещё"));more->setObjectName("secondaryCommandsBtn");
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu=new QMenu(more);
    menu->addAction(log->text(),log,&QPushButton::click);
    menu->addAction(planButton->text(),planButton,&QPushButton::click);
    auto *freeAction = menu->addAction(QStringLiteral("Свободная регистрация…"), this, &IntegrationWindow::openFreeRecording);
    freeAction->setObjectName(QStringLiteral("freeRecordingAction"));
    auto *editorAction = menu->addAction(
        QStringLiteral("Редактор циклограмм…"), this,
        [this] { launchCyclogramEditor({}); });
    editorAction->setObjectName(QStringLiteral("cyclogramEditorAction"));
    auto *openInEditor = menu->addAction(
        QStringLiteral("Открыть файл в редакторе…"), this, [this] {
            const QString file = QFileDialog::getOpenFileName(
                this, QStringLiteral("Выбрать циклограмму для редактора"), {},
                QStringLiteral("Циклограммы и проекты (*.txt *.ini project.json);;Все файлы (*)"));
            if (!file.isEmpty()) launchCyclogramEditor(file);
        });
    openInEditor->setObjectName(QStringLiteral("cyclogramEditorOpenFileAction"));
    more->setMenu(menu);header->addWidget(more);
    log->hide();planButton->hide();
    main->addLayout(header);
    auto *timePanel = new QFrame(surface);
    timePanel->setObjectName(QStringLiteral("timePanel"));
    timePanel->setStyleSheet(QStringLiteral("QFrame#timePanel{background:#161f27;border:1px solid #2c3944;border-radius:5px;}"));
    auto *clocks = new QHBoxLayout(timePanel);
    clocks->setContentsMargins(12, 8, 12, 8);
    m_firstClock = new QLabel(QStringLiteral("ДО ПЕРВОГО СОБЫТИЯ\n—"), surface);
    m_firstClock->setObjectName("firstEventClock");
    clocks->addWidget(m_firstClock, 1);
    auto *time = new QVBoxLayout;
    m_clockCaption = new QLabel(surface);
    m_clockCaption->setWordWrap(true);
    m_timerLabel = new QLabel("--:--", surface);
    m_timerLabel->setObjectName("timerLabel");
    m_clockSource = new QLabel(surface);
    m_clockSource->setObjectName("clockSource");
    m_clockSource->setWordWrap(true);
    time->setSpacing(0);
    time->addWidget(m_clockCaption);
    time->addWidget(m_timerLabel);
    time->addWidget(m_clockSource);
    clocks->addLayout(time, 1);
    auto *reference = new QVBoxLayout;
    m_kpStatus = new QLabel(surface);
    m_kpStatus->setWordWrap(true);
    m_peerStatus = new QLabel(surface);
    m_peerStatus->setObjectName("comparisonStatus");
    m_peerStatus->setWordWrap(true);
    reference->addWidget(m_startTimeLabel);
    reference->addWidget(m_kpStatus);
    reference->addWidget(m_peerStatus);
    clocks->addLayout(reference, 2);
    main->addWidget(timePanel);
    auto *status = new QHBoxLayout;
    status->addWidget(
        statusChip(surface, QStringLiteral("КАСУПП"), &m_debugDot, &m_debugStatus));
    status->addWidget(
        statusChip(surface, QStringLiteral("Пиротестер"), &m_boardDot, &m_boardStatus));
    status->addWidget(statusChip(surface, QStringLiteral("Исполнение"), &m_bcvmDot, &m_bcvmStatus));
    m_phaseLabel = new QLabel(surface);
    status->addWidget(m_phaseLabel, 1);
    main->insertLayout(1, status);
    m_finishReason = new QLabel(surface);
    m_finishReason->setObjectName(QStringLiteral("finishReason"));
    m_finishReason->setTextFormat(Qt::PlainText);
    m_finishReason->setWordWrap(true);
    m_finishReason->hide();
    main->addWidget(m_finishReason);
    m_uploadStatus = new QLabel(surface);
    m_mappingStatus = new QLabel(surface);
    m_clockStatus = new QLabel(surface);
    m_streamStatus = new QLabel(surface);
    m_streamStatus->setObjectName("legacyStreamsStatus");
    for (auto *label : {m_uploadStatus, m_mappingStatus, m_clockStatus, m_streamStatus}) {
        label->setWordWrap(true);
        label->setStyleSheet("color:#a6b3be;font-size:11px");
        main->addWidget(label);
    }
    m_uploadStatus->hide();m_clockStatus->hide();
    m_nextEventLabel = new QLabel(surface);
    m_nextEventLabel->hide();
    m_eventCaption = new QLabel(surface);
    m_eventCaption->hide();
    m_timeline = new TimelineWidget(surface);
    m_timeline->setObjectName("eventTimeline");
    m_timeline->setFixedHeight(48);
    main->addWidget(m_timeline);
    auto *selectedEvent = new QLabel(surface);
    m_selectedPlanEvent = selectedEvent;
    selectedEvent->setObjectName(QStringLiteral("selectedPlanEvent"));
    selectedEvent->setTextFormat(Qt::PlainText);
    selectedEvent->setWordWrap(true);
    selectedEvent->hide();
    main->addWidget(selectedEvent);
    connect(m_timeline, &TimelineWidget::eventSelected, this, [this, selectedEvent](int id) {
        for (const auto &action : m_session.plan().actions) {
            if (action.ordinal != id) continue;
            selectedEvent->setText(QStringLiteral("%1 · %2 · план %3 мс · %4")
                .arg(id).arg(eventName(action)).arg(presentationOffset(m_session, action))
                .arg(m_session.mapping().hasAction(action) || (m_peer && m_peer->mapping().hasAction(action))
                    ? QStringLiteral("контролируется") : QStringLiteral("без назначения входов")));
            selectedEvent->show();
            return;
        }
    });
    m_canvas = new EventCanvas(surface);
    auto *workspace=new QSplitter(Qt::Horizontal,surface);
    workspace->setObjectName("measurementWorkspace");workspace->setChildrenCollapsible(false);
    auto *detailsPanel = new QWidget(workspace);
    auto *detailsLayout = new QVBoxLayout(detailsPanel);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    auto *detailScope = new QComboBox(detailsPanel);
    detailScope->setObjectName(QStringLiteral("channelDetailScope"));
    detailScope->addItems({QStringLiteral("Все назначенные входы"), QStringLiteral("Входы выбранного события")});
    detailScope->setAccessibleName(QStringLiteral("Область поканального обзора"));
    auto *detailTools = new QHBoxLayout;
    detailTools->addWidget(detailScope, 1);
    auto *rawTicks = new QCheckBox(QStringLiteral("Тики"), detailsPanel);
    rawTicks->setObjectName(QStringLiteral("showRawTicks"));
    rawTicks->setToolTip(QStringLiteral("Показывать исходные аппаратные отметки, мкс"));
    connect(rawTicks, &QCheckBox::toggled, m_canvas, &EventCanvas::setShowRawTicks);
    detailTools->addWidget(rawTicks);
    detailsLayout->addLayout(detailTools);
    auto *diagram=new QScrollArea(detailsPanel);
    diagram->setObjectName(QStringLiteral("channelDetailsScroll"));
    detailsLayout->addWidget(diagram, 1);
    diagram->setWidgetResizable(true);diagram->setWidget(m_canvas);
    diagram->setFrameShape(QFrame::NoFrame);
    diagram->setMinimumWidth(540 + diagram->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    // В узкой панели крайние значения остаются доступны, даже если их ширина выросла.
    diagram->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_eventTable = new QTableWidget(0, 7, surface);
    m_eventTable->setObjectName("eventTable");
    connect(detailScope, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_canvas->setSelectedOnly(index == 1);
    });
    connect(m_eventTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {
        if (row < 0 || !m_eventTable->item(row, 0)) return;
        QString key = m_eventTable->item(row, 0)->data(Qt::UserRole).toString();
        // Подстрока входа относится к ближайшему заголовку группы выше неё.
        while (key.isEmpty() && row > 0) {
            const auto *parent = m_eventTable->item(--row, 0);
            if (parent) key = parent->data(Qt::UserRole).toString();
        }
        m_canvas->setSelectedEvent(key);
    });
    m_eventTable->setHorizontalHeaderLabels(
        {"#", QStringLiteral("СОБЫТИЕ"), QStringLiteral("КАНАЛЫ"), QStringLiteral("ПЛАН МС"),
         QStringLiteral("ОТ КП, МС"), QStringLiteral("Δ, МС"), QStringLiteral("СТАТУС")});
    m_eventTable->horizontalHeaderItem(4)->setToolTip(QStringLiteral("(Аппаратная отметка входа − аппаратная отметка КП) / 1000; обновляется во время сбора"));
    m_eventTable->horizontalHeaderItem(5)->setToolTip(QStringLiteral("Факт от КП − план от КП; вычисляется сразу после получения отметок"));
    m_eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_eventTable->setWordWrap(false);m_eventTable->setTextElideMode(Qt::ElideRight);
    m_eventTable->verticalHeader()->hide();m_eventTable->setMinimumWidth(410);
    m_eventTable->setColumnHidden(0,true);m_eventTable->setColumnHidden(2,true);m_eventTable->setColumnHidden(3,true);
    m_eventTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_eventTable->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    m_eventTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QFont tableFont("Segoe UI");tableFont.setPixelSize(14);m_eventTable->setFont(tableFont);
    m_eventTable->setItemDelegateForColumn(1, new EventTitleDelegate(m_eventTable));
    m_eventTable->setItemDelegateForColumn(6, new ResultStatusDelegate(m_eventTable));
    m_eventTable->setStyleSheet("QTableWidget{background:#10161c;color:#e8edf1;gridline-color:#35434f;} "
        "QTableWidget::item{background:#10161c;} QTableWidget::item:selected{background:#1c2d3a;} "
        "QHeaderView::section{background:#19222b;color:#a6b3be;padding:6px 4px;font-size:11px;border:0;border-bottom:1px solid #35434f;}");
    workspace->addWidget(m_eventTable);workspace->setStretchFactor(0,6);workspace->setStretchFactor(1,4);
    workspace->setSizes({650,460});main->addWidget(workspace,1);
    const auto toggleEvent = [this](int row,int){
        const auto key=m_eventTable->item(row,0)->data(Qt::UserRole).toString();
        if(key.isEmpty()) return;
        if(m_expandedEvents.contains(key)) m_expandedEvents.remove(key);else m_expandedEvents.insert(key);
        refreshEventTable();
    };
    connect(m_eventTable, &QTableWidget::cellClicked, this, toggleEvent);
    connect(m_eventTable, &QTableWidget::cellActivated, this, toggleEvent);
    connect(m_timeline, &TimelineWidget::eventSelected, this, [this](int id) {
        for (int row = 0; row < m_eventTable->rowCount(); ++row) {
            const auto *item = m_eventTable->item(row, 0);
            if (item->text() == QString::number(id) ||
                (item->text() == QStringLiteral("КП") &&
                 std::any_of(m_session.plan().actions.cbegin(), m_session.plan().actions.cend(),
                     [id](const PlannedAction &action) {
                         return action.ordinal == id && action.key == QStringLiteral("LIFT_OFF_CONTACT");
                     }))) {
                m_eventTable->setCurrentCell(row, 1);
                m_eventTable->scrollToItem(m_eventTable->item(row, 1));
                return;
            }
        }
        m_eventTable->setCurrentCell(-1, -1);
        m_eventTable->clearSelection();
        m_canvas->setSelectedEvent(QString());
    });
    m_summaryLabel = new QLabel(surface);
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setStyleSheet("font-size:12px;color:#e8edf1");
    main->addWidget(m_summaryLabel);
    auto *note =
        new QLabel(QStringLiteral("Аппаратный тик: мкс · план / факт / Δ: мс · ≈ — расчёт без "
                                  "подтверждения допуска · плавные часы: индикация ПК"),
                   surface);
    note->setStyleSheet("font-size:11px;color:#a6b3be");
    main->addWidget(note);
    auto *journal = new QDialog(this);
    journal->setWindowTitle(QStringLiteral("Журнал сессии"));
    journal->resize(900, 500);
    auto *jl = new QVBoxLayout(journal);
    auto *caption = new QLabel(QStringLiteral("ЖУРНАЛ СЕССИИ"), journal);
    caption->setObjectName("journalCaption");
    jl->addWidget(caption);
    m_eventLog = new QTextEdit(journal);
    m_eventLog->setObjectName("eventJournal");
    m_eventLog->setReadOnly(true);
    m_eventLog->document()->setMaximumBlockCount(1000);
    jl->addWidget(m_eventLog);
    connect(log, &QPushButton::clicked, journal, &QDialog::show);
    auto *journalToggle=new QToolButton(surface);
    journalToggle->setText(QStringLiteral("Последние события"));journalToggle->setCheckable(true);
    journalToggle->setObjectName("recentJournalToggle");main->addWidget(journalToggle);
    auto *recent=new QTextEdit(surface);m_recentLog=recent;recent->setObjectName("recentJournal");
    recent->setReadOnly(true);recent->setDocument(m_eventLog->document());recent->setFixedHeight(85);
    recent->hide();main->addWidget(recent);
    connect(journalToggle,&QToolButton::toggled,recent,&QWidget::setVisible);
    connect(journalToggle,&QToolButton::toggled,this,[this](bool visible){
        if(visible) m_recentLog->verticalScrollBar()->setValue(m_recentLog->verticalScrollBar()->maximum());
    });
    m_liveDialog = new QDialog(this);
    m_liveDialog->setObjectName("liveChannelsDialog");
    m_liveDialog->setWindowTitle(QStringLiteral("Живые каналы пиротестеров"));
    m_liveDialog->resize(770, 440);
    auto *liveLayout = new QVBoxLayout(m_liveDialog);
    auto *liveNote = new QLabel(QStringLiteral(
        "Сырые входы прибора: не зависят от КП, разрешения БЦВМ и оценки циклограммы."),
        m_liveDialog);
    liveNote->setWordWrap(true);
    liveLayout->addWidget(liveNote);
    m_liveSource = new QComboBox(m_liveDialog);
    m_liveSource->setObjectName("liveSourceChoice");
    liveLayout->addWidget(m_liveSource);
    m_liveStatus = new QLabel(m_liveDialog);
    m_liveStatus->setObjectName("liveTransportStatus");
    m_liveStatus->setWordWrap(true);
    liveLayout->addWidget(m_liveStatus);
    m_liveTable = new QTableWidget(8, 6, m_liveDialog);
    m_liveTable->setObjectName("liveChannelsTable");
    m_liveTable->setHorizontalHeaderLabels({QStringLiteral("Канал"),
        QStringLiteral("Сейчас"), QStringLiteral("Был фронт"),
        QStringLiteral("Подъём, мс"), QStringLiteral("Спад, мс"),
        QStringLiteral("Длительность, мс")});
    m_liveTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_liveTable->verticalHeader()->hide();
    m_liveTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    liveLayout->addWidget(m_liveTable);
    connect(m_liveSource, &QComboBox::currentIndexChanged, this, [this] { refreshLive(); });
    connect(live, &QPushButton::clicked, this, [this] {
        refreshLive();
        m_liveDialog->show();
        m_liveDialog->raise();
    });
    connect(finish, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Завершение наблюдения"),
            QStringLiteral("Завершить сбор и зафиксировать измерения?\n"
                           "Команды остановки или управления БЦВМ отправлены НЕ будут.")) != QMessageBox::Yes)
            return;
        m_session.finishCollection();
        if (m_peer) m_peer->finishCollection();
        refresh();
    });
    auto *planDialog = new QDialog(this);
    planDialog->setObjectName("fullPlanDialog");
    planDialog->setWindowTitle(QStringLiteral("Полный план полётного задания"));
    planDialog->resize(1100, 650);
    auto *planLayout = new QVBoxLayout(planDialog);
    m_planTable = new QTableWidget(0, 9, planDialog);
    m_planTable->setObjectName("fullPlanTable");
    m_planTable->setHorizontalHeaderLabels({"#", QStringLiteral("КЛЮЧ"), QStringLiteral("VALUE"),
        QStringLiteral("КОММЕНТАРИЙ"), QStringLiteral("КЛАСС"), QStringLiteral("ОСНОВНОЙ ИСТОЧНИК"),
        QStringLiteral("ДОП. ИСТОЧНИК"), QStringLiteral("ID ВХОЖДЕНИЯ"), QStringLiteral("НАЗНАЧЕНИЕ / ИСТОЧНИК")});
    m_planTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_planTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_planTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_planTable->horizontalHeader()->setStretchLastSection(true);
    m_planTable->setStyleSheet("QTableWidget{background:#10161c;color:#e8edf1;gridline-color:#35434f;} "
        "QHeaderView::section{background:#19222b;color:#a6b3be;padding:5px;}");
    planLayout->addWidget(m_planTable);
    connect(planButton, &QPushButton::clicked, planDialog, &QDialog::show);
    connect(map, &QPushButton::clicked, this, &IntegrationWindow::configureTester);
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(
                this, QStringLiteral("Новый сбор"),
                QStringLiteral(
                    "Очистить измерения всех выбранных источников? Новая плата получит команду "
                    "сброса; старый COM начнёт новый сбор. Требуются неактивные входы.")) ==
            QMessageBox::Yes) {
            m_session.resetBoard();
            if (m_peer)
                m_peer->resetBoard();
            m_visualAnchor.reset();
        }
    });
    connect(save, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Отчёт испытания"),
                                                 "pyro_stand_report.json", "JSON (*.json)");
        if (path.isEmpty())
            return;
        QString error;
        if (!m_session.saveReport(path, &error)) {
            QMessageBox::warning(this, QStringLiteral("Отчёт"), error);
            return;
        }
        if (m_peer) {
            const QFileInfo info(path);
            const QString peerPath =
                info.absolutePath() + "/" + info.completeBaseName() + ".legacy.json";
            if (!m_peer->saveReport(peerPath, &error)) {
                QMessageBox::warning(this, QStringLiteral("Отчёт старой платы"), error);
                return;
            }
        }
        appendMessage(
            QStringLiteral("Отчёты сохранены с отдельными аппаратными шкалами: %1").arg(path));
    });
}

void IntegrationWindow::launchCyclogramEditor(const QString &documentPath) {
    // Редактор является внешней программой. Эта операция не выбирает локальное
    // ПЗ для исполнения и никак не меняет состояние сбора или обмена с БЦВМ.
    QSettings settings(QStringLiteral("PyroStand"), QStringLiteral("Operator"));
    QString executable = settings.value(QStringLiteral("editor/executable")).toString();
    const auto executableExists = [](const QString &path) {
        const QFileInfo file(path);
        return file.isFile() && file.isExecutable();
    };
    if (!executableExists(executable)) {
#ifdef Q_OS_WIN
        const QString adjacent = QCoreApplication::applicationDirPath() +
            QStringLiteral("/cyclogram-editor.exe");
#else
        const QString adjacent = QCoreApplication::applicationDirPath() +
            QStringLiteral("/cyclogram-editor");
#endif
        if (executableExists(adjacent)) {
            executable = adjacent;
        } else {
            executable = QFileDialog::getOpenFileName(
                this, QStringLiteral("Укажите программу редактора циклограмм"),
                QCoreApplication::applicationDirPath(),
#ifdef Q_OS_WIN
                QStringLiteral("Программы (*.exe);;Все файлы (*)")
#else
                QStringLiteral("Исполняемые файлы (*)")
#endif
            );
            if (executable.isEmpty()) return;
        }
    }
    if (!executableExists(executable)) {
        QMessageBox::warning(this, QStringLiteral("Редактор циклограмм"),
            QStringLiteral("Выбранный файл не является исполняемой программой редактора."));
        return;
    }

    QStringList arguments;
    if (!documentPath.isEmpty()) {
        const QFileInfo file(documentPath);
        if (!file.isFile()) {
            QMessageBox::warning(this, QStringLiteral("Редактор циклограмм"),
                QStringLiteral("Файл циклограммы не найден: %1").arg(documentPath));
            return;
        }
        arguments << QStringLiteral("--open") << file.absoluteFilePath();
    }
    // QProcess передаёт argv без интерпретации shell: пробелы в пути безопасны.
    if (!QProcess::startDetached(executable, arguments, QFileInfo(executable).absolutePath())) {
        QMessageBox::warning(this, QStringLiteral("Редактор циклограмм"),
            QStringLiteral("Не удалось запустить редактор: %1").arg(executable));
        return;
    }
    settings.setValue(QStringLiteral("editor/executable"), executable);
    appendMessage(QStringLiteral("Редактор циклограмм открыт отдельно"
                                 " · это не загрузка и не разрешение ПЗ БЦВМ"));
}

void IntegrationWindow::configurePeer() {
    m_peer.reset();
    const auto &options = m_session.options();
    if (options.comparisonLegacyPort.isEmpty() ||
        options.boardTransport == IntegrationSession::BoardTransport::LegacySerial)
        return;
    auto legacy = options;
    legacy.boardTransport = IntegrationSession::BoardTransport::LegacySerial;
    legacy.serialPort = options.comparisonLegacyPort;
    legacy.secondLegacySerialPort.clear();
    legacy.comparisonLegacyPort.clear();
    legacy.wiringProfilePath = options.comparisonWiringProfilePath;
    legacy.comparisonWiringProfilePath.clear();
    legacy.legacySecondStage = options.comparisonLegacySecondStage;
    legacy.legacyHasKp = options.comparisonLegacyHasKp;
    m_peer = std::make_unique<IntegrationSession>(legacy, this);
    if (m_session.localPlanMode() && (!m_peer->localPlanMode() ||
        m_peer->plan().sha256 != m_session.plan().sha256)) {
        // Редактор мог изменить файл после открытия основной сессии.
        // Не запускаем второй поток с другим снимком полётного задания.
        appendMessage(QStringLiteral("ПЗ дополнительного прибора отличается от открытого локального плана. "
            "Дополнительный прибор не подключён; откройте новый сеанс с нужной версией файла."));
        m_peer.reset();
        return;
    }
    connect(m_peer.get(), &IntegrationSession::changed, this, [this] {
        if (!m_refreshTimer.isActive())
            m_refreshTimer.start();
    });
    connect(m_peer.get(), &IntegrationSession::message, this, [this](const QString &text) {
        appendMessage(QStringLiteral("Старый пиротестер: ") + text);
    });
    m_peer->start();
}

IntegrationSession &IntegrationWindow::clockSession() {
    if (m_peer && m_session.mapping().actions.isEmpty() && !m_peer->mapping().actions.isEmpty())
        return *m_peer;
    return m_session;
}

void IntegrationWindow::refreshLive() {
    if (!m_liveSource || !m_liveTable || !m_liveStatus) return;
    const QString previous = m_liveSource->currentData().toString();
    QStringList sources;
    struct Choice { QString id; QString title; };
    QVector<Choice> choices;
    const auto addSources = [&](const IntegrationSession &session, const QString &prefix,
                                const QString &name) {
        const auto &options = session.options();
        if (options.boardTransport == IntegrationSession::BoardTransport::LegacySerial) {
            choices.append({prefix + QStringLiteral(":0"), name + QStringLiteral(" · COM ") + options.serialPort});
            if (!options.secondLegacySerialPort.isEmpty())
                choices.append({prefix + QStringLiteral(":8"), name + QStringLiteral(" · COM ") + options.secondLegacySerialPort});
        } else {
            for (int group = 0; group < 12; ++group)
                choices.append({prefix + QStringLiteral(":") + QString::number(group * 8),
                    name + QStringLiteral(" · ЯПС ") + QString::number(group+1)});
        }
    };
    addSources(m_session, QStringLiteral("main"), QStringLiteral("Основной"));
    if (m_peer) addSources(*m_peer, QStringLiteral("peer"), QStringLiteral("Дополнительный"));
    for (const auto &choice : choices) sources.append(choice.id);
    bool rebuild = m_liveSource->count() != choices.size();
    if (!rebuild)
        for (int i = 0; i < choices.size(); ++i)
            if (m_liveSource->itemData(i).toString() != choices[i].id) { rebuild = true; break; }
    if (rebuild) {
        m_liveSource->blockSignals(true);
        m_liveSource->clear();
        for (const auto &choice : choices) m_liveSource->addItem(choice.title, choice.id);
        int selected = m_liveSource->findData(previous);
        if (selected < 0 && m_session.options().firstStageYps > 0 &&
            m_session.options().boardTransport != IntegrationSession::BoardTransport::LegacySerial)
            selected = m_liveSource->findData(
                QStringLiteral("main:%1").arg((m_session.options().firstStageYps - 1) * 8));
        m_liveSource->setCurrentIndex(selected < 0 ? 0 : selected);
        m_liveSource->blockSignals(false);
    }
    const QString id = m_liveSource->currentData().toString();
    const bool peer = id.startsWith(QStringLiteral("peer:"));
    const auto &session = peer && m_peer ? *m_peer : m_session;
    const int start = id.section(u':', 1, 1).toInt();
    const BoardFrame &frame = session.liveFrame();
    const auto formatTick = [](quint32 tick) {
        return tick == 0 ? QStringLiteral("—") : QString::number(double(tick) / 1000.0, 'f', 3);
    };
    for (int channel = 0; channel < 8; ++channel) {
        const auto &raw = frame.channels[size_t(start + channel)];
        const QColor highlight = raw.ready ? kGreen : raw.triggered ? kAmber : kDim;
        cell(m_liveTable, channel, 0, QString::number(channel + 1));
        cell(m_liveTable, channel, 1,
             raw.ready ? QStringLiteral("АКТИВЕН") : QStringLiteral("не активен"), highlight);
        cell(m_liveTable, channel, 2,
             raw.triggered ? QStringLiteral("ДА") : QStringLiteral("нет"),
             raw.triggered ? kGreen : kDim);
        cell(m_liveTable, channel, 3, formatTick(raw.riseUs));
        cell(m_liveTable, channel, 4, formatTick(raw.fallUs));
        const bool hasDuration = raw.triggered && raw.fallUs != 0;
        cell(m_liveTable, channel, 5,
             hasDuration ? QString::number(double(quint32(raw.fallUs - raw.riseUs)) / 1000.0, 'f', 3)
                         : QStringLiteral("—"));
    }
    m_liveStatus->setText(
        QStringLiteral("Приём: %1 · валидных: %2 · ошибок кадров: %3 · отброшено по IP: %4 · адрес последнего UDP: %5")
            .arg(session.boardReceiving() ? QStringLiteral("есть поток") : QStringLiteral("нет потока"))
            .arg(session.acceptedFrames()).arg(session.rejectedFrames())
            .arg(session.discardedUdpSenders())
            .arg(session.lastUdpSender().isEmpty() ? QStringLiteral("—") : session.lastUdpSender()));
}

void IntegrationWindow::refreshCanvas() {
    m_forecastPosition.reset();
    QVector<CanvasEvent> groups;
    int measured = 0, estimate = 0, bad = 0, missing = 0, total = 0;
    const auto add = [&](IntegrationSession &session, const QString &source) {
        const int begin = groups.size();
        const bool legacy =
            session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        QVector<QVector<double>> points(legacy ? 2 : 1);
        const auto median = [](QVector<double> v) {
            std::sort(v.begin(), v.end());
            return v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
        };
        // Прогноз до физического КП допустим только по однозначным отметкам готового и разрешённого сбора.
        for (const auto &action : session.plan().actions) {
            if (action.key == "LIFT_OFF_CONTACT")
                continue;
            if (!StandProtocol::plannedKpOffset(session.plan(), session.mapping())) continue;
            auto eval = session.measureAction(action);
            if ((eval.state != ActionMeasurement::State::WaitingForKp && eval.state != ActionMeasurement::State::WaitingForClockAlignment))
                continue;
            auto map = session.mapping().forAction(action);
            QVector<double> candidates;
            int stream = -1;
            for (const auto &in : map.inputs) {
                const auto &r = session.boardFrame().channels[size_t(in.index)];
                int st = legacy ? in.index / 8 : 0;
                if (stream >= 0 && stream != st) {
                    candidates.clear();
                    break;
                }
                stream = st;
                if (in.falling ? r.fallUs == 0 : (!r.triggered || r.riseUs == 0)) {
                    candidates.clear();
                    break;
                }
                candidates.append(double(in.falling ? r.fallUs : r.riseUs) -
                                  double(presentationOffset(session, action)) * 1000);
            }
            if (!candidates.isEmpty() && stream < points.size() &&
                *std::max_element(candidates.cbegin(), candidates.cend()) -
                        *std::min_element(candidates.cbegin(), candidates.cend()) <
                    2147483648.0)
                points[stream].append(median(candidates));
        }
        for (auto &values : points)
            if (!values.isEmpty() && *std::max_element(values.cbegin(), values.cend()) -
                                             *std::min_element(values.cbegin(), values.cend()) >=
                                         2147483648.0)
                values.clear();
        if (&session == &clockSession() && !points[0].isEmpty()) {
            const double zero = median(points[0]);
            double last = -std::numeric_limits<double>::infinity();
            for (const auto &action : session.plan().actions) {
                if ((session.measureAction(action).state != ActionMeasurement::State::WaitingForKp && session.measureAction(action).state != ActionMeasurement::State::WaitingForClockAlignment))
                    continue;
                for (const auto &in : session.mapping().forAction(action).inputs) {
                    if (legacy && in.index >= 8)
                        continue;
                    const auto &r = session.boardFrame().channels[size_t(in.index)];
                    if (r.triggered)
                        last = std::max(last, double(in.falling ? r.fallUs : r.riseUs));
                }
            }
            if (std::isfinite(last))
                m_forecastPosition = (last - zero) / 1000;
            const auto limits = std::minmax_element(points[0].cbegin(), points[0].cend());
            m_kpStatus->setText(
                QStringLiteral("КП ≈ %1 мкс · %2 пунктов · разброс %3 мс\nРасчётная опора; разброс "
                               "не определяет точность")
                    .arg(zero, 0, 'f', 0)
                    .arg(points[0].size())
                    .arg((*limits.second - *limits.first) / 1000, 0, 'f', 3));
        }
        for (const auto &action : session.plan().actions) {
            if (action.key == "LIFT_OFF_CONTACT" || !session.mapping().hasAction(action))
                continue;
            const auto map = session.mapping().forAction(action);
            auto eval = session.measureAction(action);
            CanvasEvent g;
            g.id = QString::number(action.ordinal);
            g.title = eventName(action);
            g.source = source;
            g.detail=eval.reason+QStringLiteral("\nИсточник карты: ")+session.mapping().version;
            g.offsetMs = presentationOffset(session, action);
            g.plan = QStringLiteral("ПЛАН %1%2").arg(g.offsetMs).arg(
                StandProtocol::plannedKpOffset(session.plan(), session.mapping()) ? QString() : QStringLiteral(" T0"));
            if (legacy && !session.options().secondLegacySerialPort.isEmpty() &&
                !map.inputs.isEmpty() && map.inputs.first().index >= 8)
                g.source = QStringLiteral("Старый · ") + session.options().secondLegacySerialPort;
            for (const auto &in : map.inputs) {
                CanvasChannel row;
                row.input =
                    legacy ? QString::number(in.index % 8 + 1)
                           : QStringLiteral("%1:%2").arg(in.index / 8+1).arg(in.index % 8 + 1);
                row.role = action.comment.trimmed().isEmpty()
                    ? g.title : action.comment.trimmed();
                row.color = kDim;
                row.actual = row.delta = row.rawTick = QStringLiteral("—");
                row.status = QStringLiteral("ожидание");
                const auto &raw = session.boardFrame().channels[size_t(in.index)];
                if (raw.triggered)
                    row.rawTick = QString::number(in.falling ? raw.fallUs : raw.riseUs);
                for (const auto &sample : eval.channels)
                    if (sample.input.index == in.index && sample.input.falling == in.falling &&
                        sample.actualMs) {
                        row.actual = QString::number(*sample.actualMs, 'f', 3);
                        row.delta = signedMs(*sample.deviationMs);
                        row.status = QStringLiteral("измерено");
                        row.color = kNeutral;
                        ++measured;
                        if (eval.toleranceMs) {
                            bool ok = std::abs(*sample.deviationMs) <= *eval.toleranceMs;
                            row.status =
                                ok ? QStringLiteral("в допуске") : QStringLiteral("отклонение");
                            row.color = ok ? kGreen : kRed;
                            if (!ok)
                                ++bad;
                        }
                    }
                int stream = legacy ? in.index / 8 : 0;
                if (row.actual == QStringLiteral("—") && raw.triggered &&
                    (eval.state == ActionMeasurement::State::WaitingForKp || eval.state == ActionMeasurement::State::WaitingForClockAlignment) &&
                    stream < points.size() && !points[stream].isEmpty()) {
                    double actual =
                        (double(in.falling ? raw.fallUs : raw.riseUs) - median(points[stream])) /
                        1000;
                    row.actual = QStringLiteral("≈") + QString::number(actual, 'f', 3);
                    row.delta = QStringLiteral("≈") + signedMs(actual - double(presentationOffset(session, action)));
                    row.status = QStringLiteral("расчёт");
                    row.color = kAmber;
                    ++estimate;
                }
                if (row.actual == QStringLiteral("—") &&
                    eval.state != ActionMeasurement::State::WaitingForInputs &&
                    eval.state != ActionMeasurement::State::Partial) {
                    const bool missed = eval.state == ActionMeasurement::State::MissingAfterFinish ||
                        eval.state == ActionMeasurement::State::PartialAfterFinish;
                    row.status = missed ? QStringLiteral("НЕ СРАБОТАЛ") : QStringLiteral("без оценки");
                    row.color = missed ? kRed : kAmber;
                    if (missed) ++missing;
                }
                if(session.collectionFinished() && row.actual==QStringLiteral("—") &&
                   eval.state!=ActionMeasurement::State::MissingAfterFinish &&
                   eval.state!=ActionMeasurement::State::PartialAfterFinish) {
                    row.status=QStringLiteral("не измерено");row.color=kDim;
                }
                g.channels.append(row);
                ++total;
            }
            g.actual=g.delta=QStringLiteral("—");
            g.status=QStringLiteral("Ожидание");g.color=kDim;
            bool allMeasured=!g.channels.isEmpty(),anyMeasured=false;
            double latest=-std::numeric_limits<double>::infinity();
            double worst=0;
            for(const auto &channel:g.channels) {
                bool valid=false;const double actual=channel.actual.toDouble(&valid);
                allMeasured &= valid;anyMeasured |= valid;
                if(valid) {latest=std::max(latest,actual);const double delta=channel.delta.toDouble();
                    if(std::abs(delta)>std::abs(worst)) worst=delta;}
                if(channel.color==kRed) {g.color=kRed;g.status=QStringLiteral("Отклонение");}
            }
            using State=ActionMeasurement::State;
            if(anyMeasured) {
                g.actual=QString::number(latest,'f',3);g.delta=signedMs(latest-g.offsetMs);
                g.detail+=QStringLiteral("\nФакт группы: последний вход. Δ = факт − план. Худшее Δ входа: %1 мс").arg(signedMs(worst));
            }
            if(g.color!=kRed) {
                if(allMeasured) {g.status=eval.toleranceMs?QStringLiteral("В допуске"):QStringLiteral("Измерено");
                    g.color=eval.toleranceMs?kGreen:kNeutral;}
                else if(eval.state==State::StoppedEarly) {g.status=QStringLiteral("Не измерено");g.color=kDim;}
                else if(eval.state==State::Partial || anyMeasured) {g.status=QStringLiteral("Не все входы");g.color=kAmber;}
                else if(eval.state!=State::WaitingForInputs) {
                    g.color=kAmber;
                    switch(eval.state) {
                    case State::WaitingForKp:g.status=QStringLiteral("Нет КП");break;
                    case State::WaitingForClockAlignment:g.status=QStringLiteral("Нет опоры");break;
                    case State::InvalidAssignment:g.status=QStringLiteral("Назначение");break;
                    case State::UploadUnconfirmed:g.status=QStringLiteral("Нет загрузки");break;
                    case State::ExecutionUnconfirmed:g.status=QStringLiteral("Нет разрешения");break;
                    case State::BoardNotArmed:g.status=QStringLiteral("Нет сброса");break;
                    case State::RepeatedAction:g.status=QStringLiteral("Повтор");break;
                    case State::RepeatedInput:g.status=QStringLiteral("Повторные фронты");break;
                    default:g.status=QStringLiteral("Недостоверно");break;
                    }
                }
            } else if(eval.state==State::MissingAfterFinish) g.status=QStringLiteral("Не сработало");
            else if(eval.state==State::PartialAfterFinish) g.status=QStringLiteral("Не все входы");
            groups.append(g);
        }
        const int kp = session.mapping().kp.index;
        if (kp >= 0) {
            const auto &raw = session.boardFrame().channels[size_t(kp)];
            const bool confirmed = session.measurementEligible() && session.kpTimeUs().has_value();
            CanvasEvent g;
            g.id = QStringLiteral("КП");
            g.title = QStringLiteral("Контакт подъёма");
            g.source = source;
            g.plan = QStringLiteral("ПЛАН 0");
            g.actual=confirmed?QStringLiteral("0.000"):QStringLiteral("—");g.delta=QStringLiteral("—");
            g.status=confirmed?QStringLiteral("КП"):QStringLiteral("Ожидание КП");g.color=confirmed?kGreen:kAmber;
            g.detail=QStringLiteral("Физический КП · ")+session.mapping().version;
            const auto reference=session.mapping().kp;
            const auto rawKp=reference.falling?raw.fallUs:raw.riseUs;
            g.channels.append({QString::number(reference.index%8+1), QStringLiteral("Опорный ноль"),
                               rawKp ? QString::number(rawKp) : QStringLiteral("—"),
                               confirmed ? QStringLiteral("0.000") : QStringLiteral("—"),
                               QStringLiteral("—"),
                               confirmed ? QStringLiteral("КП") : QStringLiteral("нет КП"),
                               confirmed ? kGreen : kAmber});
            groups.append(g);
            if (legacy && !session.options().secondLegacySerialPort.isEmpty()) {
                const auto &other = session.boardFrame().channels[15];
                const bool otherKnown =
                    session.measurementEligible() && session.legacyStreamKpObserved(1);
                g.source = QStringLiteral("Старый · ") + session.options().secondLegacySerialPort;
                g.actual=otherKnown?QStringLiteral("0.000"):QStringLiteral("—");
                g.status=otherKnown?QStringLiteral("КП"):QStringLiteral("Ожидание КП");g.color=otherKnown?kGreen:kAmber;
                g.channels[0] = {"8",
                                 QStringLiteral("КП8 БКД-Б"),
                                 other.triggered ? QString::number(other.riseUs)
                                                 : QStringLiteral("—"),
                                 otherKnown ? QStringLiteral("0.000") : QStringLiteral("—"),
                                 QStringLiteral("—"),
                                 otherKnown ? QStringLiteral("КП") : QStringLiteral("нет КП"),
                                 otherKnown ? kGreen : kAmber};
                groups.append(g);
            }
        }
        std::stable_sort(groups.begin() + begin, groups.end(),
                         [](const auto &a, const auto &b) { return a.offsetMs < b.offsetMs; });
    };
    add(m_session,
        m_session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial
            ? QStringLiteral("Старый · ") + m_session.options().serialPort
            : QStringLiteral("Новый пиротестер"));
    if (m_peer) {
        add(*m_peer, QStringLiteral("Старый · ") + m_peer->options().serialPort);
        m_peerStatus->setText(
            QStringLiteral("%1 → %2 · одна ЯПС\nДанные: %3 · Debug: %4 · план: %5 · КП: %6")
                .arg(m_peer->options().serialPort,
                     m_peer->options().legacySecondStage ? QStringLiteral("БКД-Б") : QStringLiteral("БКД-А"),
                     m_peer->boardReceiving() ? QStringLiteral("данные идут")
                                              : QStringLiteral("нет потока"),
                     m_peer->debugConnected() ? QStringLiteral("связь есть")
                                              : QStringLiteral("нет связи"),
                     m_peer->hasPlan() ? QStringLiteral("получен") : QStringLiteral("ожидание"),
                     !m_peer->options().legacyHasKp ? QStringLiteral("нет на старой плате") : m_peer->legacyStreamKpObserved(0) ? QStringLiteral("есть")
                                                       : QStringLiteral("нет")));
    } else
        m_peerStatus->clear();
    if (&clockSession() != &m_session) {
        auto &source = clockSession();
        // Не подменяем полную шкалу сокращённым списком при выборе второго источника.
        for (const auto &action : source.plan().actions) {
            auto result = source.measureAction(action);
            if (result.state != ActionMeasurement::State::Measured)
                continue;
            bool good = result.toleranceMs.has_value();
            for (const auto &row : result.channels)
                if (row.deviationMs && result.toleranceMs &&
                    std::abs(*row.deviationMs) > *result.toleranceMs)
                    good = false;
            m_timeline->markEventFired(action.ordinal, good                 ? "ok"
                                                       : result.toleranceMs ? "fail"
                                                                            : "");
        }
        const auto kpAction = std::find_if(source.plan().actions.cbegin(),
            source.plan().actions.cend(), [](const PlannedAction &action) {
                return action.key == QStringLiteral("LIFT_OFF_CONTACT");
            });
        if (kpAction != source.plan().actions.cend() &&
            source.measurementEligible() && source.kpTimeUs())
            m_timeline->markEventFired(kpAction->ordinal, QStringLiteral("ok"));
        if (source.measurementEligible() && source.kpTimeUs())
            m_kpStatus->setText(
                QStringLiteral("КП старого %1: %2 мкс · собственная аппаратная опора")
                    .arg(source.options().serialPort)
                    .arg(*source.kpTimeUs()));
    }
    m_canvas->setEvents(std::move(groups));

    // Показываем, почему реально полученный КП не стал измерительной опорой.
    // Не подменяем его фронтом соседней ЯПС по эвристике.
    if (m_session.options().boardTransport != IntegrationSession::BoardTransport::LegacySerial &&
        m_session.hasBoardFrame() && m_session.mapping().kp.index >= 0) {
        const int expectedIndex = m_session.mapping().kp.index;
        const auto &expected = m_session.boardFrame().channels[size_t(expectedIndex)];
        const bool physicallySeen = expected.triggered && expected.riseUs != 0;
        if (!m_session.measurementEligible() && physicallySeen) {
            QStringList restrictions;
            if (!m_session.boardArmed()) restrictions << QStringLiteral("не подтверждён новый сбор/сброс");
            if (m_session.uploadState() != QStringLiteral("CONFIRMED"))
                restrictions << QStringLiteral("не подтверждена загрузка ПЗ");
            if (!m_session.executionPermissionObserved())
                restrictions << QStringLiteral("нет подтверждения исполнения БЦВМ");
            if (m_session.boardArmed() && m_session.executionPermissionObserved() &&
                m_session.uploadState() == QStringLiteral("CONFIRMED"))
                restrictions << QStringLiteral("нарушены условия достоверности");
            m_kpStatus->setText(QStringLiteral(
                "Сырой фронт КП на ЯПС %1/8: %2 мкс. Опора не принята: %3")
                .arg(expectedIndex / 8 + 1)
                .arg(expected.riseUs)
                .arg(restrictions.join(QStringLiteral("; "))));
        } else if (!physicallySeen) {
            QStringList possible;
            for (int yps = 0; yps < 12; ++yps) {
                const int index = yps * 8 + 7;
                if (index == expectedIndex) continue;
                const auto &candidate = m_session.boardFrame().channels[size_t(index)];
                if (candidate.triggered && candidate.riseUs != 0)
                    possible << QStringLiteral("ЯПС %1/8 (%2 мкс)")
                                    .arg(yps + 1).arg(candidate.riseUs);
            }
            if (!possible.isEmpty())
                m_kpStatus->setText(QStringLiteral(
                    "КП отсутствует на назначенном ЯПС %1/8. Фронты других каналов 8: %2. "
                    "Проверьте карту подключения; это только кандидаты, не подтверждённый КП.")
                    .arg(expectedIndex / 8 + 1)
                    .arg(possible.join(QStringLiteral(", "))));
            else
                m_kpStatus->setText(QStringLiteral(
                    "КП не обнаружен на назначенном ЯПС %1/8; проверьте вход 8, карту ## и сырые данные.")
                    .arg(expectedIndex / 8 + 1));
        }
    }
    m_summaryLabel->setText(QStringLiteral("Входы: измерено %1/%2 · отклонение %3 · не зарегистрировано %4 · без оценки %5%6")
                                .arg(measured)
                                .arg(total)
                                .arg(bad)
                                .arg(missing)
                                .arg(total - measured - missing - estimate)
                                .arg(estimate ? QStringLiteral(" · прогноз %1 (без оценки допуска)").arg(estimate) : QString()));
}

void IntegrationWindow::appendJournal(const QString &level, const QString &message,
                                      const QString &color) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    // Вертикальный маркер журнала рисуем символом: свойство border-left поддерживается не всеми версиями Qt.
    m_eventLog->append(
        QStringLiteral("<span style=\"color:%1;\">▌</span>  "
                       "<span style=\"color:#8b949e;\">%2</span>  "
                       "<span style=\"color:%1; font-weight:600;\">%3</span>  "
                       "<span style=\"color:#c8d0dc;\">%4</span>")
            .arg(color, stamp.toHtmlEscaped(), level.toHtmlEscaped(), message.toHtmlEscaped()));
    if(m_recentLog) m_recentLog->verticalScrollBar()->setValue(m_recentLog->verticalScrollBar()->maximum());
}

void IntegrationWindow::appendMessage(const QString &message) {
    const bool error = message.contains(QStringLiteral("ошиб"), Qt::CaseInsensitive) ||
                       message.contains(QStringLiteral("не удалось"), Qt::CaseInsensitive) ||
                       message.contains(QStringLiteral("сбой"), Qt::CaseInsensitive);
    appendJournal(error ? QStringLiteral("ОШИБКА") : QStringLiteral("INFO"), message,
                  error ? QStringLiteral("#f85149") : QStringLiteral("#8b949e"));
}

void IntegrationWindow::updatePlannedClock() {
    auto &source = clockSession();
    if (!source.hasPlan()) {
        m_firstClock->hide();
        m_timerLabel->setText("--:--");
        m_firstClock->setText(QStringLiteral("ДО ПЕРВОГО СОБЫТИЯ\n—"));
        m_clockCaption->setText(QStringLiteral("ОЖИДАНИЕ ЦИКЛОГРАММЫ"));
        m_clockSource->clear();
        m_visualAnchor.reset();
        return;
    }
    if (!source.boardArmed() && !source.collectionFinished()) {
        m_firstClock->hide();
        m_timerLabel->setText(QStringLiteral("—"));
        m_firstClock->setText(QStringLiteral("ДО ПЕРВОГО СОБЫТИЯ\n—"));
        m_clockCaption->setText(QStringLiteral("ПОДГОТОВКА СБОРА"));
        m_clockSource->setText(source.boardReceiving()
            ? QStringLiteral("В плате прежние отметки · нажмите «Новый сбор»")
            : QStringLiteral("Ожидание данных пиротестера"));
        m_visualAnchor.reset();
        return;
    }
    const QString collection =
        source.collectionId() + "|" + QString::number(source.measurementEligible());
    if (collection != m_visualCollection) {
        m_visualCollection = collection;
        m_visualAnchor.reset();
    }
    const auto measured = source.measuredPositionMs();
    const auto kpPlan = StandProtocol::plannedKpOffset(source.plan(), source.mapping());
    // После завершения без КП часы ПК замораживаем на момент окончания.
    // Это не измерение; привязка к физическому КП отсутствует.
    const QDateTime indicatedUtc = source.collectionFinished() &&
            source.finishedAtUtc().isValid()
        ? source.finishedAtUtc() : QDateTime::currentDateTimeUtc();
    double position = double(source.plan().t0Utc.msecsTo(indicatedUtc)) -
        kpPlan.value_or(0);
    const auto anchor = measured ? measured : m_forecastPosition;
    if (anchor) {
        if (!m_visualAnchor || *m_visualAnchor != *anchor) {
            m_visualAnchor = *anchor;
            m_visualElapsed.restart();
            m_lastVisualPosition = *anchor;
        }
        position =
            source.boardReceiving() && !source.collectionFinished()
                ? *anchor + m_visualElapsed.elapsed() : *anchor;
        m_lastVisualPosition = position;
    } else
        m_visualAnchor.reset();
    const auto show = [](double value) {
        const qint64 ms = qint64(std::abs(value));
        return QStringLiteral("%1%2:%3.%4")
            .arg(value < 0 ? QStringLiteral("−") : QStringLiteral("+"))
            .arg(ms / 60000, 2, 10, QChar('0'))
            .arg((ms / 1000) % 60, 2, 10, QChar('0'))
            .arg((ms % 1000) / 100);
    };
    m_timerLabel->setText((source.collectionFinished() && measured?QString():QStringLiteral("≈ "))+show(position));
    m_timerLabel->setStyleSheet(
        QStringLiteral("font:28px 'Consolas';color:%1").arg(measured ? "#81b6d0" : "#e5bd78"));
    m_clockCaption->setText(source.collectionFinished() && !measured
                                ? QStringLiteral("СБОР ЗАВЕРШЁН · ОПОРНОГО КП НЕТ")
                                : measured ? (source.collectionFinished()
                                    ? (source.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial
                                        ? QStringLiteral("КОНЕЦ СБОРА ОТ КП")
                                        : QStringLiteral("ПОСЛЕДНЯЯ ОТМЕТКА ОТ КП"))
                                    : QStringLiteral("ПОСЛЕ ФИЗИЧЕСКОГО КП"))
                            : !kpPlan ? QStringLiteral("ОТ ПЛАНОВОГО T0 · КП НЕ ОПРЕДЕЛЁН")
                            : position < 0
                                ? (m_forecastPosition ? QStringLiteral("ДО РАСЧЁТНОГО КП")
                                                      : QStringLiteral("ДО ПЛАНОВОГО КП"))
                                : QStringLiteral("ПОСЛЕ ПЛАНОВОГО КП"));
    m_clockSource->setText(
        measured ? (source.collectionFinished()
            ? QStringLiteral("Аппаратное время от КП · зафиксировано")
            : QStringLiteral("Опора платы + индикация ПК; факты не интерполируются"))
        : source.collectionFinished() ? QStringLiteral("Зафиксировано по часам ПК; это не измерение от КП")
        : m_forecastPosition ? QStringLiteral("Прогноз КП по отметкам + ПК · неточный")
                             : QStringLiteral("Расчёт по UTC / часам ПК · неточный"));
    double first = std::numeric_limits<double>::infinity();
    for (const auto &a : source.plan().actions)
        if (source.mapping().hasAction(a))
            first = std::min(first, double(presentationOffset(source, a)));
    m_firstClock->setText(std::isfinite(first)
                              ? QStringLiteral("ДО ПЕРВОГО СОБЫТИЯ\n%1")
                                    .arg(position < first
                                             ? show(first - position)
                                             : QStringLiteral("первое событие пройдено"))
                              : QStringLiteral("ДО ПЕРВОГО СОБЫТИЯ\nнет назначений"));
    m_firstClock->setVisible(std::isfinite(first) && position < first && !source.collectionFinished());
    m_timeline->setPlayheadMs(qint64(position));
}

void IntegrationWindow::configureTester() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("testerConfigurationDialog"));
    dialog.setWindowTitle(QStringLiteral("Подключение пиротестера и БКД"));
    auto *root = new QVBoxLayout(&dialog);
    auto *preset = new QPushButton(QStringLiteral("Мой стенд: новая плата · ЯПС 8 — БКХО-А + КП · ЯПС 7 — БКХО-Б"), &dialog);
    preset->setObjectName(QStringLiteral("historicalPresetBtn"));
    preset->setMinimumHeight(40);
    root->addWidget(preset);
    auto *summary = new QLabel(&dialog);
    summary->setObjectName(QStringLiteral("testerConfigurationSummary"));
    summary->setWordWrap(true);
    summary->setMinimumWidth(530);
    root->addWidget(summary);
    auto *details = new QCheckBox(QStringLiteral("Другие подключения и профили"), &dialog);
    details->setObjectName(QStringLiteral("advancedConnectionsToggle"));
    details->setStyleSheet(QStringLiteral("color:#a6b3be;font-size:12px;"));
    root->addWidget(details);
    auto *advanced = new QWidget(&dialog);
    advanced->setObjectName(QStringLiteral("advancedConnections"));
    auto *layout = new QFormLayout(advanced);
    layout->setContentsMargins(0, 0, 0, 0);
    root->addWidget(advanced);
    advanced->hide();
    connect(details, &QCheckBox::toggled, &dialog, [&, advanced](bool visible) {
        advanced->setVisible(visible);
        dialog.adjustSize();
    });
    auto options = m_session.options();
    const auto profileField = [&](const QString &title, const QString &path) {
        auto *container = new QWidget(&dialog);
        auto *line = new QHBoxLayout(container);
        line->setContentsMargins(0, 0, 0, 0);
        auto *value = new QLineEdit(path, container);
        value->setPlaceholderText(QStringLiteral("Пусто — встроенная историческая проводка"));
        auto *choose = new QPushButton(QStringLiteral("Выбрать…"), container);
        line->addWidget(value, 1); line->addWidget(choose);
        connect(choose, &QPushButton::clicked, &dialog, [&, value] {
            const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Профиль сигналов"), value->text(), "JSON (*.json)");
            if (!file.isEmpty()) value->setText(file);
        });
        layout->addRow(title, container);
        return value;
    };
    auto *profile = profileField(QStringLiteral("Профиль основного источника"), options.wiringProfilePath);
    auto *comparisonProfile = profileField(QStringLiteral("Профиль дополнительного COM"), options.comparisonWiringProfilePath);
    profile->setObjectName(QStringLiteral("primaryWiringProfile"));
    comparisonProfile->setObjectName(QStringLiteral("comparisonWiringProfile"));
    auto *transport = new QComboBox(&dialog);
    transport->setObjectName(QStringLiteral("testerTransportChoice"));
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
    auto *comparison = new QComboBox(&dialog);comparison->setEditable(true);comparison->addItem("");
    comparison->setObjectName(QStringLiteral("comparisonLegacyPortChoice"));
    for(const auto &info:QSerialPortInfo::availablePorts())comparison->addItem(info.portName());
    comparison->setCurrentText(options.comparisonLegacyPort);
    auto *comparisonBlock=new QComboBox(&dialog);comparisonBlock->addItem(QStringLiteral("БКД-А-1"),false);comparisonBlock->addItem(QStringLiteral("БКД-Б"),true);comparisonBlock->setCurrentIndex(options.comparisonLegacySecondStage?1:0);
    layout->addRow(QStringLiteral("Одновременно старый COM (пусто = выключен)"),comparison);
    layout->addRow(QStringLiteral("БКД старого COM · одна ЯПС"),comparisonBlock);
    auto *comparisonKp = new QCheckBox(QStringLiteral("КП подключён к каналу 8 старой платы"), &dialog);
    comparisonKp->setChecked(options.comparisonLegacyHasKp);
    layout->addRow(comparisonKp);
    auto *legacyKp = new QCheckBox(QStringLiteral("КП подключён к каналу 8 старого COM"), &dialog);
    legacyKp->setChecked(options.legacyHasKp);
    auto *address = new QLineEdit(options.boardIp.toString(), &dialog);
    address->setObjectName(QStringLiteral("testerBoardIp"));
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
    layout->addRow(QStringLiteral("БКД-А-1 · историческая проводка"), first);
    layout->addRow(QStringLiteral("БКД-Б · историческая проводка"), second);
    layout->addRow(QStringLiteral("КП · вход 8"), kp);
    layout->addRow(QStringLiteral("БКД на старой плате"), block);
    layout->addRow(QStringLiteral("Второй COM · БКД-Б (необязательно)"), secondPort);
    layout->addRow(legacyKp);
    connect(preset, &QPushButton::clicked, &dialog, [=] {
        profile->clear(); comparisonProfile->clear();
        address->setText(QStringLiteral("192.168.17.144"));
        transport->setCurrentIndex(transport->findData(int(IntegrationSession::BoardTransport::Udp)));
        first->setCurrentIndex(first->findData(8)); second->setCurrentIndex(second->findData(7)); kp->setCurrentIndex(kp->findData(8));
        comparison->setCurrentText(QString()); comparisonBlock->setCurrentIndex(comparisonBlock->findData(true)); comparisonKp->setChecked(false);
        secondPort->setCurrentText(QString());
    });
    auto *note = new QLabel(QStringLiteral(
        "Выберите «Мой стенд» и нажмите «Применить». Настройка сохранится.\n"
        "Циклограмму загружайте через КАСУПП — пиростенд получит её через Debug.\n"
        "ЯПС 8 и ЯПС 7 принадлежат одной новой плате. Время обоих блоков\n"
        "считается по её аппаратным отметкам относительно КП на ЯПС 8, вход 8."), &dialog);
    note->setWordWrap(true);
    root->addWidget(note);
    const auto update = [=] {
        const auto value = IntegrationSession::BoardTransport(transport->currentData().toInt());
        const bool legacy = value == IntegrationSession::BoardTransport::LegacySerial;
        comparison->setEnabled(!legacy);comparisonBlock->setEnabled(!legacy);comparisonKp->setEnabled(!legacy);legacyKp->setEnabled(legacy);
        port->setEnabled(value != IntegrationSession::BoardTransport::Udp);
        address->setEnabled(!legacy);
        first->setEnabled(!legacy); second->setEnabled(!legacy); kp->setEnabled(!legacy);
        block->setEnabled(legacy);
        secondPort->setEnabled(legacy);
        QString text;
        if (legacy) {
            text = QStringLiteral("Старый тестер: %1 → %2; %3")
                .arg(port->currentText(), block->currentText(), legacyKp->isChecked()
                     ? QStringLiteral("КП на входе 8") : QStringLiteral("без КП"));
            if (!secondPort->currentText().isEmpty())
                text += QStringLiteral("\nВторой старый тестер: %1 → БКД-Б").arg(secondPort->currentText());
        } else {
            text = QStringLiteral("Новая плата: %1 · IP %2 → БКХО-А, %3\nКП: %4, вход 8")
                .arg(value == IntegrationSession::BoardTransport::Udp ? QStringLiteral("Ethernet") : QStringLiteral("RS-485"),
                     address->text(), first->currentText(), kp->currentText());
            if (second->currentData().toInt() != 0)
                text += QStringLiteral("\nБКХО-Б на новой плате: %1").arg(second->currentText());
            if (!comparison->currentText().isEmpty())
                text += QStringLiteral("\nСтарый тестер: %1 → %2 · одна ЯПС · %3")
                    .arg(comparison->currentText(), comparisonBlock->currentText(), comparisonKp->isChecked()
                         ? QStringLiteral("КП на входе 8") : QStringLiteral("без КП"));
        }
        if (!profile->text().isEmpty() || !comparisonProfile->text().isEmpty())
            text += QStringLiteral("\nНазначения входов переопределены внешним профилем.");
        summary->setText(text);
    };
    for (auto *choice : {transport, port, secondPort, comparison, comparisonBlock, first, second, kp, block})
        connect(choice, &QComboBox::currentTextChanged, &dialog, update);
    for (auto *check : {comparisonKp, legacyKp})
        connect(check, &QCheckBox::toggled, &dialog, update);
    for (auto *field : {profile, comparisonProfile, address})
        connect(field, &QLineEdit::textChanged, &dialog, update);
    connect(preset, &QPushButton::clicked, &dialog, update);
    update();
    auto *validationError = new QLabel(&dialog);
    validationError->setObjectName(QStringLiteral("testerConfigurationError"));
    validationError->setWordWrap(true);
    validationError->setStyleSheet(QStringLiteral("color:#f85149;"));
    validationError->hide();
    root->addWidget(validationError);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Применить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        validationError->hide();
        const auto showError = [&](const QString &message, QWidget *field) {
            validationError->setText(message);
            validationError->show();
            details->setChecked(true);
            field->setFocus();
            if (auto *line = qobject_cast<QLineEdit *>(field)) line->selectAll();
            dialog.adjustSize();
        };
        for (const auto *field : {profile, comparisonProfile}) {
            if (field->text().trimmed().isEmpty()) continue;
            QFile file(field->text().trimmed()); StandMapping parsed; QString error;
            if (!file.open(QIODevice::ReadOnly) || !StandProtocol::parseMapping(file.readAll(), &parsed, &error)) {
                showError(QStringLiteral("Профиль сигналов: %1").arg(error.isEmpty() ? file.errorString() : error),
                          field == profile ? profile : comparisonProfile);
                return;
            }
        }
        options.wiringProfilePath = profile->text().trimmed();
        options.comparisonWiringProfilePath = comparisonProfile->text().trimmed();
        options.boardTransport = IntegrationSession::BoardTransport(transport->currentData().toInt());
        options.serialPort = port->currentText().trimmed();
        options.comparisonLegacyPort = options.boardTransport==IntegrationSession::BoardTransport::LegacySerial?QString():comparison->currentText().trimmed();
        options.comparisonLegacySecondStage=comparisonBlock->currentData().toBool();
        options.comparisonLegacyHasKp=comparisonKp->isChecked();
        options.legacyHasKp=legacyKp->isChecked();
        options.secondLegacySerialPort = secondPort->currentText().trimmed();
        options.firstStageYps = first->currentData().toInt();
        options.secondStageYps = second->currentData().toInt();
        options.kpYps = kp->currentData().toInt();
        options.legacySecondStage = block->currentData().toBool();
        const bool legacy = options.boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        if (!legacy && (!options.boardIp.setAddress(address->text().trimmed()) ||
                        options.boardIp.protocol() != QAbstractSocket::IPv4Protocol ||
                        address->text().trimmed().split(u'.').size() != 4)) {
            showError(QStringLiteral("Некорректный IP новой платы: «%1». Введите полный IPv4 без запятых, например 192.168.17.144.")
                          .arg(address->text()), address);
            return;
        }
        if (options.boardTransport != IntegrationSession::BoardTransport::Udp && options.serialPort.isEmpty()) {
            showError(QStringLiteral("Не указан COM-порт основного тестера."), port);
            return;
        }
        if (!options.comparisonLegacyPort.isEmpty() && options.boardTransport == IntegrationSession::BoardTransport::Serial &&
            options.comparisonLegacyPort.compare(options.serialPort, Qt::CaseInsensitive) == 0) {
            showError(QStringLiteral("Один COM-порт выбран для двух разных тестеров. Укажите разные порты."), comparison);
            return;
        }
        if (!legacy && options.firstStageYps != 0 && options.firstStageYps == options.secondStageYps) {
            showError(QStringLiteral("БКД-А и БКД-Б на новой плате назначены на одну ЯПС. Выберите разные группы либо «Не назначен»."), second);
            return;
        }
        if (legacy && !options.secondLegacySerialPort.isEmpty() &&
            options.serialPort.compare(options.secondLegacySerialPort, Qt::CaseInsensitive) == 0) {
            showError(QStringLiteral("Первый и второй старый тестер используют один COM-порт. Укажите разные порты."), secondPort);
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted) return;
    QSettings settings(QStringLiteral("PyroStand"), QStringLiteral("Operator"));
    settings.setValue("tester/comparisonLegacyPort",options.comparisonLegacyPort);
    settings.setValue("tester/comparisonLegacySecondStage",options.comparisonLegacySecondStage);
    settings.setValue("tester/comparisonLegacyHasKp",options.comparisonLegacyHasKp);
    settings.setValue("tester/legacyHasKp",options.legacyHasKp);
    settings.setValue(QStringLiteral("tester/transport"), int(options.boardTransport));
    settings.setValue(QStringLiteral("tester/serial"), options.serialPort);
    settings.setValue(QStringLiteral("tester/secondSerial"), options.secondLegacySerialPort);
    settings.setValue(QStringLiteral("tester/ip"), options.boardIp.toString());
    settings.setValue(QStringLiteral("tester/firstYps"), options.firstStageYps);
    settings.setValue(QStringLiteral("tester/secondYps"), options.secondStageYps);
    settings.setValue(QStringLiteral("tester/kpYps"), options.kpYps);
    settings.setValue(QStringLiteral("tester/legacySecondStage"), options.legacySecondStage);
    settings.setValue(QStringLiteral("tester/wiringProfile"), options.wiringProfilePath);
    settings.setValue(QStringLiteral("tester/comparisonWiringProfile"), options.comparisonWiringProfilePath);
    m_loggedStates.clear();
    m_timelineSignature.clear();
    m_eventLog->clear();
    m_peer.reset();
    m_visualAnchor.reset();
    m_session.configureBoard(options);
    configurePeer();
    refresh();
}

void IntegrationWindow::refresh() {
    if (m_peer && m_session.collectionFinished() && !m_peer->collectionFinished())
        m_peer->finishCollection(QStringLiteral(
            "Основной измеритель завершил окно наблюдения; точность связи часов не подтверждена"));
    const bool legacy = m_session.options().boardTransport == IntegrationSession::BoardTransport::LegacySerial;
    const bool secondOnly = legacy && m_session.options().secondLegacySerialPort.isEmpty() && m_session.options().legacySecondStage;
    m_resetButton->setText(QStringLiteral("Новый сбор"));
    const bool hasPlan = m_session.hasPlan();
    const bool eligible = m_session.measurementEligible();
    const bool dualLegacy = legacy && !m_session.options().secondLegacySerialPort.isEmpty();
    const bool firstKp = legacy ? m_session.legacyStreamKpObserved(0)
                                 : m_session.kpTimeUs().has_value();
    const bool secondKp = dualLegacy && m_session.legacyStreamKpObserved(1);
    const bool kpKnown = eligible && firstKp && (!dualLegacy || secondKp);
    const bool uploaded = m_session.uploadState() == QStringLiteral("CONFIRMED");

    setChip(m_debugDot, m_debugStatus, m_session.debugConnected(),
            m_session.debugConnected() ? QStringLiteral("подключён") : QStringLiteral("нет связи"));
    const bool allStreamsReceiving = m_session.boardReceiving();
    const bool validCollection = !legacy || m_session.boardArmed();
    setChip(m_boardDot, m_boardStatus, allStreamsReceiving && validCollection,
            !allStreamsReceiving
                ? (legacy && dualLegacy ? QStringLiteral("не все COM активны")
                                        : legacy ? QStringLiteral("нет COM-данных")
                                                 : QStringLiteral("нет кадров"))
                : legacy && !validCollection ? QStringLiteral("сбор не готов")
                                              : legacy ? QStringLiteral("COM-потоки идут")
                                                       : QStringLiteral("кадры идут"));
    setChip(m_bcvmDot, m_bcvmStatus, m_session.executionEnabled(),
            m_session.localPlanMode()
                ? QStringLiteral("БЦВМ не подтверждала")
                : m_session.executionEnabled() ? QStringLiteral("разрешено")
                                               : QStringLiteral("не разрешено"));

    QString phaseText;
    QString phaseColor = QStringLiteral("#8b949e");
    if (m_session.collectionFinished()) {
        phaseText = m_session.plannedWindowComplete() ? QStringLiteral("СБОР ЗАВЕРШЁН")
            : QStringLiteral("СБОР ЗАВЕРШЁН · НЕПОЛНЫЙ");
        phaseColor = m_session.plannedWindowComplete() ? QStringLiteral("#81b6d0")
            : QStringLiteral("#e3b341");
        m_phaseLabel->setToolTip(m_session.finishReason());
    } else if (!hasPlan) {
        phaseText = QStringLiteral("ОЖИДАНИЕ ЦИКЛОГРАММЫ");
    } else if (m_session.localPlanMode()) {
        phaseText = !m_session.boardArmed()
            ? QStringLiteral("ЛОКАЛЬНЫЙ ПЛАН · ОЖИДАНИЕ СБОРА")
            : firstKp ? QStringLiteral("ЛОКАЛЬНЫЙ ПЛАН · КП ПОЛУЧЕН")
                      : QStringLiteral("ЛОКАЛЬНЫЙ ПЛАН · ОЖИДАНИЕ КП");
        phaseColor = QStringLiteral("#e3b341");
    } else if (!uploaded) {
        phaseText = QStringLiteral("ЗАГРУЗКА НЕ ПОДТВЕРЖДЕНА");
        phaseColor = QStringLiteral("#e3b341");
    } else if (!m_session.boardArmed()) {
        phaseText = m_session.boardReceiving() ? QStringLiteral("НУЖЕН НОВЫЙ СБОР")
                                              : QStringLiteral("ОЖИДАНИЕ ДАННЫХ ПЛАТЫ");
        phaseColor = QStringLiteral("#e3b341");
    } else if (kpKnown) {
        phaseText = QStringLiteral("СБОР ИДЁТ · КП ЗАФИКСИРОВАН");
        phaseColor = QStringLiteral("#3fb950");
    } else if (m_session.executionPermissionObserved()) {
        phaseText = dualLegacy && firstKp && !secondKp
                ? QStringLiteral("ОЖИДАНИЕ КП БКД-Б")
            : dualLegacy && !firstKp && secondKp
                ? QStringLiteral("ОЖИДАНИЕ КП БКД-А-1")
            : QStringLiteral("ОЖИДАНИЕ ОПОРНОГО КП");
        phaseColor = QStringLiteral("#e3b341");
    } else {
        phaseText = QStringLiteral("ПЛАН ПРИНЯТ, ОЖИДАНИЕ");
        phaseColor = QStringLiteral("#58a6ff");
    }
    m_phaseLabel->setText(QStringLiteral("● ") + phaseText);
    m_finishReason->setVisible(m_session.collectionFinished());
    m_finishReason->setText(m_session.finishReason());
    m_finishReason->setStyleSheet(QStringLiteral("color:%1;").arg(phaseColor));
    m_finishButton->setEnabled(hasPlan && !m_session.collectionFinished());
    m_phaseLabel->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:600; letter-spacing:0.08em; color:%1;").arg(phaseColor));
    m_startTimeLabel->setText(hasPlan
        ? QStringLiteral("%1T0 %2 UTC")
            .arg(m_session.localPlanMode() ? QStringLiteral("ЛОКАЛЬНО · ") : QString(),
                 m_session.plan().t0Utc.toString(QStringLiteral("dd.MM HH:mm:ss")))
        : QStringLiteral("--:--:--"));
    m_startTimeLabel->setToolTip(hasPlan ? m_session.plan().t0Utc.toString(Qt::ISODateWithMs)
                                        : QString());

    m_uploadStatus->setText(QStringLiteral("ЦИКЛОГРАММА · %1")
        .arg(m_session.localPlanMode() ? QStringLiteral("ЛОКАЛЬНЫЙ ФАЙЛ · БЦВМ НЕ ПОДТВЕРЖДАЛА")
            : uploaded ? QStringLiteral("ЗАГРУЗКА ПОДТВЕРЖДЕНА")
                      : QStringLiteral("ЗАГРУЗКА НЕ ПОДТВЕРЖДЕНА")));
    m_uploadStatus->setToolTip(QStringLiteral("Состояние: %1\nID загрузки: %2")
        .arg(m_session.uploadState(), m_session.uploadId()));
    if (dualLegacy) {
        m_kpStatus->setText(QStringLiteral("КП8 А-1: %1 · КП8 Б: %2 · %3")
            .arg(firstKp ? QStringLiteral("зафиксирован") : QStringLiteral("нет"),
                 secondKp ? QStringLiteral("зафиксирован") : QStringLiteral("нет"),
                 eligible ? QStringLiteral("независимые шкалы каждого COM")
                          : QStringLiteral("сбор недостоверен или не подтверждён")));
    } else {
        m_kpStatus->setText(kpKnown
            ? QStringLiteral("КП · зафиксирован: %1 мкс; относительный ноль = 0 мс")
                  .arg(*m_session.kpTimeUs())
            : QStringLiteral("КП · нет подтверждённой опорной отметки; относительные факты не вычисляются"));
    }
    if (legacy) {
        const QString firstLabel = dualLegacy || !secondOnly
            ? QStringLiteral("БКД-А-1") : QStringLiteral("БКД-Б");
        const auto streamText = [&](int stream, const QString &name) {
            return QStringLiteral("%1: COM%2 %3, КП8 %4")
                .arg(name)
                .arg(stream + 1)
                .arg(m_session.legacyStreamReceiving(stream)
                         ? QStringLiteral("принимает") : QStringLiteral("нет потока"))
                .arg(m_session.legacyStreamKpObserved(stream)
                         ? QStringLiteral("есть") : QStringLiteral("нет"));
        };
        m_streamStatus->setText(streamText(0, firstLabel) +
            (dualLegacy ? QStringLiteral(" · ") +
                streamText(1, QStringLiteral("БКД-Б")) : QString()) +
            (m_session.boardArmed() ? QString() : QStringLiteral(" · СБОР НЕДОСТОВЕРЕН")));
        m_streamStatus->show();
    } else {
        m_streamStatus->hide();
    }
    m_mappingStatus->setText(QStringLiteral("ПОДКЛЮЧЕНИЕ БКД · %1")
        .arg(m_session.mapping().version.isEmpty()
            ? QStringLiteral("не загружена") : m_session.mapping().version));
    const auto &connection = m_session.options();
    if (connection.boardTransport == IntegrationSession::BoardTransport::Udp &&
        connection.firstStageYps == 8 && connection.secondStageYps == 7 && connection.kpYps == 8 &&
        connection.comparisonLegacyPort.isEmpty() && connection.wiringProfilePath.isEmpty() &&
        !m_session.mapping().occurrenceBased && m_session.mapping().warnings.isEmpty())
        m_mappingStatus->setText(QStringLiteral("НОВАЯ · Ethernet %1 · ЯПС 8 — БКХО-А + КП (вход 8) · ЯПС 7 — БКХО-Б · общая аппаратная шкала")
            .arg(connection.boardIp.toString()));
    if (connection.boardTransport == IntegrationSession::BoardTransport::Udp &&
        connection.firstStageYps == 8 && connection.secondStageYps == 0 && connection.kpYps == 8 &&
        connection.comparisonLegacyPort.compare(QStringLiteral("COM9"), Qt::CaseInsensitive) == 0 &&
        connection.comparisonLegacySecondStage && !connection.comparisonLegacyHasKp &&
        connection.wiringProfilePath.isEmpty() && connection.comparisonWiringProfilePath.isEmpty())
        m_mappingStatus->setText(QStringLiteral("НОВАЯ · Ethernet %1 → БКД-А, ЯПС 8, КП на входе 8   |   СТАРЫЙ · COM9 → БКД-Б, одна ЯПС, без КП")
            .arg(connection.boardIp.toString()));
    if (!m_session.mapping().warnings.isEmpty())
        m_mappingStatus->setText(m_mappingStatus->text() + QStringLiteral(" · КАРТА ТРЕБУЕТ ПРОВЕРКИ"));
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

    // Полный план содержит отдельную строку для каждого вхождения команды.
    m_planTable->setRowCount(m_session.plan().actions.size());
    int planRow = 0;
    for (const auto &action : m_session.plan().actions) {
        const auto *definition = findCommand(action.key);
        const bool assigned = m_session.mapping().hasAction(action) ||
            (m_peer && m_peer->mapping().hasAction(action));
        const QString classification = !definition ? QStringLiteral("НЕИЗВЕСТНА БЦВМ")
            : assigned ? (action.wiring.isEmpty() ? QStringLiteral("КОНТРОЛИРУЕМОЕ")
                : QStringLiteral("## · РАЗВОДКА НЕ ПОДТВЕРЖДЕНА"))
            : definition->mask ? QStringLiteral("ТРЕБУЕТ НАСТРОЙКИ")
                               : QStringLiteral("ИНФОРМАЦИОННОЕ (КОД)");
        QStringList wiringDetails;
        for(const auto &annotation:action.wiring) wiringDetails << annotation.raw;
        const QString assignmentTip=action.wiring.isEmpty()
            ? QStringLiteral("Источник: %1").arg(m_session.mapping().version)
            : QStringLiteral("Источник: ## ПЗ; разводка не подтверждена\n%1").arg(wiringDetails.join('\n'));
        const QStringList values{QString::number(action.ordinal), action.key, action.rawValue,
            action.comment, classification, m_session.actionResult(action),
            m_peer ? m_peer->actionResult(action) : QStringLiteral("—"), action.actionId,
            wiringDetails.isEmpty() ? assignmentTip : wiringDetails.join(QStringLiteral(" | "))};
        for (int col = 0; col < values.size(); ++col)
            cell(m_planTable, planRow, col, values[col], kNeutral, QColor(),
                 Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Строка %1; %2")
                     .arg(action.sourceLine).arg(action.actionId)+u'\n'+assignmentTip);
        ++planRow;
    }

    // Общая шкала содержит ВСЁ полётное задание. Пироканалы лишь отмечают
    // те пункты, которые действительно наблюдаются на одном из источников.
    const QString signature = m_session.uploadId() + QStringLiteral("|") +
        m_session.mapping().version + QStringLiteral("|") +
        (m_peer ? m_peer->mapping().version : QString()) +
        QString::number(m_session.plan().actions.size()) +
        m_session.plan().sha256 + m_session.collectionId();
    if (signature != m_timelineSignature) {
        m_timelineSignature = signature;
        m_selectedPlanEvent->hide();
        QVector<EventRow> events;
        for (const auto &action : m_session.plan().actions) {
            EventRow event;
            event.id = action.ordinal;
            event.key = action.key;
            event.description = eventName(action);
            event.time_ms = presentationOffset(m_session, action);
            event.hasChannels = m_session.mapping().hasAction(action) ||
                (m_peer && m_peer->mapping().hasAction(action));
            events.append(event);
        }
        m_timeline->setEvents(events);
    }
    m_timeline->setVisible(hasPlan);
    refreshCanvas();
    refreshEventTable();
    updatePlannedClock();
}

void IntegrationWindow::refreshEventTable() {
    const auto &groups=m_canvas->events();
    int count=0;
    for(const auto &group:groups) count+=1+(group.channels.size()>1?group.channels.size():0);
    if(m_eventTable->rowCount()!=count) m_eventTable->setRowCount(count);
    int row=0;
    QHash<int,QString> timelineStates;
    for(const auto &group:groups) {
        const QString key=group.source+u'/'+group.id;
        const bool expanded=m_expandedEvents.contains(key);
        const QString detail=group.title+u'\n'+group.source+QStringLiteral(" · ")+group.plan+u'\n'+group.detail;
        cell(m_eventTable,row,0,group.id);
        m_eventTable->item(row,0)->setData(Qt::UserRole,key);
        const QString compactSource=QString(group.source).replace(QStringLiteral("Новый пиротестер"),QStringLiteral("Новая плата"));
        cell(m_eventTable,row,1,(group.channels.size()>1?(expanded?QStringLiteral("▾ "):QStringLiteral("▸ ")):QString())+
            group.title+u'\n'+group.plan+QStringLiteral(" · ")+compactSource,kNeutral,{},Qt::AlignLeft|Qt::AlignVCenter,detail);
        cell(m_eventTable,row,2,group.source);
        cell(m_eventTable,row,3,QString::number(group.offsetMs));
        cell(m_eventTable,row,4,group.actual,kNeutral,{},Qt::AlignRight|Qt::AlignVCenter,detail);
        cell(m_eventTable,row,5,group.delta,kNeutral,{},Qt::AlignRight|Qt::AlignVCenter,detail);
        cell(m_eventTable,row,6,group.status,group.color,{},Qt::AlignLeft|Qt::AlignVCenter,detail);
        m_eventTable->resizeRowToContents(row);m_eventTable->setRowHidden(row,false);++row;
        if(group.channels.size()>1) for(const auto &channel:group.channels) {
            cell(m_eventTable,row,0,QString());m_eventTable->item(row,0)->setData(Qt::UserRole,QString());
            cell(m_eventTable,row,1,QStringLiteral("   ↳ ")+channel.input,kDim);
            cell(m_eventTable,row,2,channel.input);cell(m_eventTable,row,3,QString::number(group.offsetMs));
            cell(m_eventTable,row,4,channel.actual,kNeutral,{},Qt::AlignRight|Qt::AlignVCenter,detail);
            cell(m_eventTable,row,5,channel.delta,channel.color,{},Qt::AlignRight|Qt::AlignVCenter,detail);
            cell(m_eventTable,row,6,channel.status,channel.color,{},Qt::AlignLeft|Qt::AlignVCenter,detail);
            m_eventTable->setRowHeight(row,30);m_eventTable->setRowHidden(row,!expanded);++row;
        }
        const QString state=group.status+u'|'+group.actual+u'|'+group.delta;
        if(m_loggedStates.value(key)!=state) {
            appendJournal(QStringLiteral("СОБЫТИЕ"),group.title+QStringLiteral(" · ")+group.source+
                QStringLiteral(" · ")+group.status+QStringLiteral(" · факт ")+group.actual+
                QStringLiteral(" · Δ ")+group.delta,group.color.name());
            m_loggedStates.insert(key,state);
        }
        bool numeric=false;int id=group.id.toInt(&numeric);
        if(!numeric) {
            for(const auto &action:m_session.plan().actions) if(action.key=="LIFT_OFF_CONTACT") id=action.ordinal;
        }
        const auto status=group.color==kRed?QStringLiteral("fail"):
            group.color==kGreen?QStringLiteral("ok"):QString();
        if(timelineStates.value(id)!="fail") timelineStates.insert(id,status);
    }
    for(auto it=timelineStates.cbegin();it!=timelineStates.cend();++it) m_timeline->markEventFired(it.key(),it.value());
}
