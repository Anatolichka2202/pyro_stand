#include "integration_window.h"
#include "free_recording.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QtConcurrentRun>

void IntegrationWindow::openFreeRecording() {
    refreshLive();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("freeRecordingDialog"));
    dialog.setWindowTitle(QStringLiteral("Свободная регистрация · без ПЗ и оценки допуска"));
    dialog.resize(1000, 580);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("freeSource"));
    for (int i = 0; i < m_liveSource->count(); ++i)
        source->addItem(m_liveSource->itemText(i), m_liveSource->itemData(i));
    source->setCurrentIndex(m_liveSource->currentIndex());
    form->addRow(QStringLiteral("Прибор / группа"), source);
    auto *kp = new QComboBox(&dialog);
    kp->setObjectName(QStringLiteral("freeKp"));
    auto *edge = new QComboBox(&dialog);
    edge->addItems({QStringLiteral("Фронт"), QStringLiteral("Спад")});
    form->addRow(QStringLiteral("Физический КП на том же приборе"), kp);
    form->addRow(QStringLiteral("Опорный переход"), edge);
    layout->addLayout(form);
    auto *help = new QLabel(QStringLiteral("Выберите входы галочками. Старт не сбрасывает плату. Нужен чистый кадр / нулевая маска.\n"
        "Показана первая пара фронт–спад. NEW не передаёт историю повторов; OLD сохраняет переходы в отчёте.\n"
        "Загрузка другого ПЗ или очистка общего транспорта завершает запись как неполную.\n"
        "Без физического КП показаны исходные тики. Часы ПК используются только в журнале."), &dialog);
    help->setWordWrap(true); layout->addWidget(help);
    auto *table = new QTableWidget(8, 7, &dialog);
    table->setObjectName(QStringLiteral("freeInputs"));
    table->setHorizontalHeaderLabels({QStringLiteral("Вход"), QStringLiteral("Уровень"),
        QStringLiteral("Фронт, мкс"), QStringLiteral("Спад, мкс"), QStringLiteral("От КП, мс"),
        QStringLiteral("Длит., мс"), QStringLiteral("Фронтов")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setStyleSheet(QStringLiteral("QTableWidget { background:#0f171d; color:#e6edf3; gridline-color:#34434e; }"
        "QHeaderView::section { background:#18232c; color:#9fc7df; border:1px solid #34434e; padding:4px; }"
        "QTableWidget::item:selected { background:#203d52; color:#ffffff; }"));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 7; ++col) table->setItem(row, col, new QTableWidgetItem(QStringLiteral("—")));
        table->item(row, 0)->setText(QString::number(row + 1));
        table->item(row, 0)->setCheckState(Qt::Checked);
    }
    layout->addWidget(table);
    auto *status = new QLabel(QStringLiteral("Готово к независимой регистрации"), &dialog);
    status->setWordWrap(true); status->setObjectName(QStringLiteral("freeStatus")); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(&dialog);
    auto *start = buttons->addButton(QStringLiteral("Начать"), QDialogButtonBox::ActionRole);
    auto *stop = buttons->addButton(QStringLiteral("Остановить"), QDialogButtonBox::ActionRole);
    auto *save = buttons->addButton(QStringLiteral("Сохранить JSON…"), QDialogButtonBox::ActionRole);
    auto *close = buttons->addButton(QDialogButtonBox::Close);
    close->setText(QStringLiteral("Закрыть"));
    start->setObjectName(QStringLiteral("freeStart")); stop->setObjectName(QStringLiteral("freeStop"));
    save->setObjectName(QStringLiteral("freeSave"));
    layout->addWidget(buttons);

    FreeRecording recording;
    IntegrationSession *selectedSession = nullptr;
    int selectedStream = 0;
    bool everReceived = false, saved = true, saving = false;
    QFutureWatcher<QString> writer;
    const auto sessionForChoice = [&]() -> IntegrationSession * {
        return source->currentData().toString().startsWith(QStringLiteral("peer:")) ? m_peer.get() : &m_session;
    };
    const auto updateKp = [&] {
        kp->clear(); kp->addItem(QStringLiteral("Без КП"), -1);
        const auto *session = sessionForChoice();
        const bool legacy = session && session->options().boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        for (int i = 0; i < (legacy ? 8 : 96); ++i)
            kp->addItem(legacy ? QStringLiteral("Вход %1").arg(i + 1)
                : QStringLiteral("ЯПС %1 · вход %2").arg(i / 8 + 1).arg(i % 8 + 1), i);
    };
    updateKp();
    connect(source, &QComboBox::currentIndexChanged, &dialog, updateKp);
    const auto update = [&] {
        recording.checkDeadline();
        const bool active = recording.active();
        const bool hasRecord = !recording.report().value("recording_id").toString().isEmpty();
        source->setEnabled(!hasRecord && !saving); kp->setEnabled(!hasRecord && !saving); edge->setEnabled(!hasRecord && !saving);
        start->setEnabled(!active && !saving); stop->setEnabled(active);
        start->setText(hasRecord ? QStringLiteral("Новая запись") : QStringLiteral("Начать"));
        save->setEnabled(!active && !saving && hasRecord);
        status->setText(saving ? QStringLiteral("Сохранение архива…") : recording.status().isEmpty()
            ? QStringLiteral("Готово к независимой регистрации") : recording.status());
        for (int row = 0; row < 8; ++row) {
            auto *selection = table->item(row, 0);
            selection->setFlags(hasRecord ? selection->flags() & ~Qt::ItemIsUserCheckable : selection->flags() | Qt::ItemIsUserCheckable);
            const auto &c = recording.frame().channels[size_t(recording.config().group * 8 + row)];
            const bool selected = !hasRecord || (recording.config().selected & (1 << row))
                || recording.config().kp == recording.config().group * 8 + row;
            const auto relative = c.riseUs ? recording.relativeMs(c.riseUs) : std::nullopt;
            const quint32 duration = c.fallUs - c.riseUs;
            table->item(row, 1)->setText(!selected ? QStringLiteral("Не регистрируется")
                : c.ready ? QStringLiteral("Активен") : QStringLiteral("Не активен"));
            table->item(row, 2)->setText(c.riseUs ? QString::number(c.riseUs) : QStringLiteral("—"));
            table->item(row, 3)->setText(c.fallUs ? QString::number(c.fallUs) : QStringLiteral("—"));
            table->item(row, 4)->setText(relative ? QString::number(*relative, 'f', 3) : QStringLiteral("—"));
            table->item(row, 5)->setText(c.riseUs && c.fallUs && duration < 0x80000000u
                ? QString::number(double(duration) / 1000.0, 'f', 3) : QStringLiteral("—"));
            table->item(row, 6)->setText(recording.config().legacy ? QString::number(c.observedRises) : QStringLiteral("нет истории"));
        }
    };
    connect(start, &QPushButton::clicked, &dialog, [&] {
        if (!saved && QMessageBox::question(&dialog, QStringLiteral("Новая запись"),
            QStringLiteral("Предыдущая запись не сохранена. Заменить её?")) != QMessageBox::Yes) return;
        if (!recording.report().value("recording_id").toString().isEmpty()) {
            recording = {}; selectedSession = nullptr; saved = true; update(); return;
        }
        selectedSession = sessionForChoice();
        if (!selectedSession) return;
        FreeRecording::Config config;
        const auto &options = selectedSession->options();
        config.legacy = options.boardTransport == IntegrationSession::BoardTransport::LegacySerial;
        const int offset = source->currentData().toString().section(u':', 1, 1).toInt();
        selectedStream = config.legacy ? offset / 8 : 0;
        config.group = config.legacy ? 0 : offset / 8;
        config.source = source->currentText() + QStringLiteral(" · ") + (config.legacy
            ? selectedStream ? options.secondLegacySerialPort : options.serialPort
            : options.boardTransport == IntegrationSession::BoardTransport::Udp ? options.boardIp.toString() : options.serialPort);
        config.kp = kp->currentData().toInt(); config.fallingKp = edge->currentIndex() == 1;
        config.selected = 0;
        for (int i = 0; i < 8; ++i) if (table->item(i, 0)->checkState() == Qt::Checked) config.selected |= quint8(1 << i);
        if (!recording.start(config)) {
            QMessageBox::warning(&dialog, QStringLiteral("Регистрация"), QStringLiteral("Выберите хотя бы один вход")); return;
        }
        everReceived = false; saved = false; update();
    });
    connect(stop, &QPushButton::clicked, &dialog, [&] { recording.stop(QStringLiteral("Остановлено оператором")); update(); });
    const auto subscribe = [&](IntegrationSession *session) {
        if (!session) return;
        connect(session, &IntegrationSession::frameObserved, &dialog, [&, session](const BoardFrame &frame, const QByteArray &raw) {
            if (session == selectedSession && recording.active() && !recording.config().legacy) {
                everReceived = true; recording.acceptFrame(frame, raw);
            }
        });
        connect(session, &IntegrationSession::legacyBytesObserved, &dialog, [&, session](const QByteArray &raw, int stream) {
            if (session == selectedSession && recording.active() && recording.config().legacy && stream == selectedStream) {
                everReceived = true; recording.acceptLegacy(raw);
            }
        });
        connect(session, &IntegrationSession::changed, &dialog, [&, session] {
            if (session != selectedSession || !recording.active() || !everReceived) return;
            const bool receiving = recording.config().legacy ? session->legacyStreamReceiving(selectedStream) : session->boardReceiving();
            if (!receiving) recording.stop(QStringLiteral("Запись неполная: связь с выбранным источником потеряна"));
        });
        connect(session, &IntegrationSession::inputDiscontinuity, &dialog, [&, session](const QString &reason) {
            if (session == selectedSession) recording.stop(reason);
        });
    };
    subscribe(&m_session); subscribe(m_peer.get());
    connect(save, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, QStringLiteral("Архив свободной регистрации"),
            QStringLiteral("free-recording.json"), QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) return;
        const auto snapshot = recording.report();
        saving = true; update();
        writer.setFuture(QtConcurrent::run([path, snapshot]() -> QString {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return file.errorString();
            const QByteArray bytes = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
            if (file.write(bytes) != bytes.size() || !file.commit()) return file.errorString();
            return {};
        }));
    });
    connect(&writer, &QFutureWatcher<QString>::finished, &dialog, [&] {
        saving = false;
        const QString error = writer.result();
        if (error.isEmpty()) saved = true;
        else QMessageBox::warning(&dialog, QStringLiteral("Ошибка сохранения"), error);
        update();
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    // Проверяем также Escape и системную кнопку закрытия до разрушения записи.
    connect(&dialog, &QDialog::finished, &dialog, [&] { recording.stop(QStringLiteral("Окно закрыто")); });
    QTimer timer;
    connect(&timer, &QTimer::timeout, &dialog, update); timer.start(100);
    update();
    dialog.exec();
    if (!saved) {
        // После закрытия возвращаем окно, чтобы оператор мог сохранить снимок.
        while (!saved && QMessageBox::question(this, QStringLiteral("Несохранённая запись"),
            QStringLiteral("Закрыть без сохранения? Выберите «Нет», чтобы вернуться к записи.")) != QMessageBox::Yes)
            dialog.exec();
    }
}
