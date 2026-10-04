#include "integration_window.h"

#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

namespace {
void setCell(QTableWidget *table, int row, int column, const QString &value) {
    QTableWidgetItem *item = table->item(row, column);
    if (!item) { item = new QTableWidgetItem; table->setItem(row, column, item); }
    item->setText(value);
}
}

IntegrationWindow::IntegrationWindow(IntegrationSession::Options options,
                                     const QString &mappingPath, QWidget *parent)
    : QMainWindow(parent), m_session(std::move(options), this) {
    setWindowTitle(QStringLiteral("Пиростенд — план КАСУ ПП и измерения платы"));
    resize(1480, 900);
    auto *root = new QWidget(this);
    auto *layout = new QVBoxLayout(root);
    auto *status = new QGridLayout;
    const QStringList names = {
        QStringLiteral("Debug КАСУ ПП"), QStringLiteral("Плата пиротестера"),
        QStringLiteral("Загрузка"), QStringLiteral("БЦВМ"),
        QStringLiteral("Плановое T0 UTC"), QStringLiteral("Физический КП"),
        QStringLiteral("Карта каналов"), QStringLiteral("Часы БЦВМ при загрузке"),
        QStringLiteral("SHA-256 циклограммы")};
    QLabel **values[] = {&m_debugStatus, &m_boardStatus, &m_uploadStatus, &m_bcvmStatus,
                         &m_t0Status, &m_kpStatus, &m_mappingStatus, &m_clockStatus,
                         &m_hashStatus};
    for (int i = 0; i < names.size(); ++i) {
        status->addWidget(new QLabel(names.at(i) + QStringLiteral(": "), root), i / 2, (i % 2) * 2);
        *values[i] = new QLabel(QStringLiteral("—"), root);
        (*values[i])->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status->addWidget(*values[i], i / 2, (i % 2) * 2 + 1);
    }
    status->setColumnStretch(1, 1);
    status->setColumnStretch(3, 1);
    layout->addLayout(status);

    auto *buttons = new QHBoxLayout;
    auto *resetButton = new QPushButton(QStringLiteral("Сбросить результаты платы"), root);
    auto *mappingButton = new QPushButton(QStringLiteral("Открыть карту каналов"), root);
    auto *saveButton = new QPushButton(QStringLiteral("Сохранить отчёт"), root);
    buttons->addWidget(resetButton);
    buttons->addWidget(mappingButton);
    buttons->addWidget(saveButton);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(resetButton, &QPushButton::clicked, &m_session, &IntegrationSession::resetBoard);
    connect(mappingButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Карта каналов"),
                                                          {}, QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) m_session.loadMapping(path);
    });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Отчёт"),
                                                          QStringLiteral("pyro_stand_report.json"),
                                                          QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) return;
        QString error;
        if (!m_session.saveReport(path, &error)) QMessageBox::warning(this, QStringLiteral("Отчёт"), error);
        else appendMessage(QStringLiteral("Отчёт сохранён: %1").arg(path));
    });

    auto *split = new QSplitter(Qt::Horizontal, root);
    m_planTable = new QTableWidget(split);
    m_planTable->setColumnCount(4);
    m_planTable->setHorizontalHeaderLabels({QStringLiteral("№"), QStringLiteral("Смещение, мс"),
                                            QStringLiteral("Действие"), QStringLiteral("Измерение")});
    m_planTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_planTable->setColumnWidth(0, 50);
    m_planTable->setColumnWidth(1, 130);
    m_planTable->setColumnWidth(2, 360);
    m_planTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_channelTable = new QTableWidget(split);
    m_channelTable->setColumnCount(6);
    m_channelTable->setRowCount(96);
    m_channelTable->setHorizontalHeaderLabels({QStringLiteral("ЯПС"), QStringLiteral("Канал"),
                                               QStringLiteral("Уровень"), QStringLiteral("Фронт, мкс"),
                                               QStringLiteral("Спад, мкс"),
                                               QStringLiteral("Длительность, мкс")});
    m_channelTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_channelTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int i = 0; i < 96; ++i) {
        setCell(m_channelTable, i, 0, QString::number(i / 8 + 1));
        setCell(m_channelTable, i, 1, QString::number(i % 8 + 1));
    }
    split->addWidget(m_planTable);
    split->addWidget(m_channelTable);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split, 1);
    m_log = new QTextEdit(root);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(150);
    layout->addWidget(m_log);
    setCentralWidget(root);

    connect(&m_session, &IntegrationSession::changed, this, &IntegrationWindow::refresh);
    connect(&m_session, &IntegrationSession::message, this, &IntegrationWindow::appendMessage);
    if (!mappingPath.isEmpty()) m_session.loadMapping(mappingPath);
    m_session.start();
    refresh();
}

void IntegrationWindow::appendMessage(const QString &text) {
    m_log->append(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss ")) + text.toHtmlEscaped());
}

void IntegrationWindow::refresh() {
    m_debugStatus->setText(m_session.debugConnected() ? QStringLiteral("подключён")
                                                   : QStringLiteral("нет связи"));
    m_boardStatus->setText(QStringLiteral("%1; принято %2, отклонено %3; %4")
        .arg(m_session.boardReceiving() ? QStringLiteral("кадры идут") : QStringLiteral("нет свежих кадров"))
        .arg(m_session.acceptedFrames()).arg(m_session.rejectedFrames())
        .arg(m_session.boardArmed() ? QStringLiteral("получен пустой кадр после сброса")
                                    : QStringLiteral("нужен пустой кадр после сброса")));
    m_uploadStatus->setText(QStringLiteral("%1; upload_id=%2")
        .arg(m_session.uploadState(), m_session.uploadId()));
    m_bcvmStatus->setText(QStringLiteral("%1; исполнение %2")
        .arg(m_session.lockState(), m_session.executionEnabled()
            ? QStringLiteral("разрешено") : QStringLiteral("не подтверждено")));
    m_t0Status->setText(m_session.hasPlan() ? m_session.plan().t0Utc.toString(Qt::ISODate)
                                           : QStringLiteral("нет плана"));
    m_kpStatus->setText(m_session.kpTimeUs() ? QStringLiteral("%1 мкс по счётчику платы")
                                                    .arg(*m_session.kpTimeUs())
                                              : QStringLiteral("не измерен / вход не назначен"));
    m_mappingStatus->setText(m_session.mapping().version.isEmpty() ? QStringLiteral("не загружена")
                                                             : m_session.mapping().version);
    m_clockStatus->setText(m_session.bcvmUtc().isEmpty() ? QStringLiteral("неизвестно")
                                                       : m_session.bcvmUtc() + QStringLiteral(" (синхронизация неизвестна)"));
    m_hashStatus->setText(m_session.cyclogramSha256().isEmpty() ? QStringLiteral("нет файла")
                                                               : m_session.cyclogramSha256());

    const auto &actions = m_session.plan().actions;
    if (m_planTable->rowCount() != actions.size()) m_planTable->setRowCount(actions.size());
    for (int row = 0; row < actions.size(); ++row) {
        const auto &action = actions.at(row);
        setCell(m_planTable, row, 0, QString::number(action.ordinal));
        setCell(m_planTable, row, 1, QString::number(action.offsetMs));
        setCell(m_planTable, row, 2, action.key);
        setCell(m_planTable, row, 3, m_session.actionResult(action));
    }
    if (m_session.hasBoardFrame()) {
        const auto &channels = m_session.boardFrame().channels;
        for (int row = 0; row < 96; ++row) {
            const auto &ch = channels[size_t(row)];
            setCell(m_channelTable, row, 2, ch.ready ? QStringLiteral("1") : QStringLiteral("0"));
            setCell(m_channelTable, row, 3, ch.triggered ? QString::number(ch.riseUs) : QStringLiteral("—"));
            setCell(m_channelTable, row, 4, ch.fallUs ? QString::number(ch.fallUs) : QStringLiteral("—"));
            setCell(m_channelTable, row, 5, ch.triggered && ch.fallUs
                ? QString::number(quint32(ch.fallUs - ch.riseUs)) : QStringLiteral("—"));
        }
    }
}
