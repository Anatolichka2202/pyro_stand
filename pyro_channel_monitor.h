#pragma once

#include "integration.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <array>
#include <functional>
#include <limits>
#include <optional>

// Постоянный пассивный монитор всех 96 входов. Не отправляет команд прибору
// и не зависит от состояния разрешения БЦВМ / наличия циклограммы.
class PyroChannelMonitor final : public QWidget {
public:
    explicit PyroChannelMonitor(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("pyroChannelMonitor"));
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(8, 6, 8, 6);
        root->setSpacing(5);
        auto *heading = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("ЖИВЫЕ ВХОДЫ · 12 ЯПС / 96 каналов"), this);
        title->setStyleSheet(QStringLiteral("font-weight:600;color:#b7d9eb;"));
        heading->addWidget(title);
        heading->addStretch();
        m_link = new QLabel(QStringLiteral("Ожидание кадров"), this);
        heading->addWidget(m_link);
        root->addLayout(heading);

        m_gnss = new QLabel(QStringLiteral("GNSS: ожидаются данные AA58"), this);
        m_gnss->setObjectName(QStringLiteral("gnssTelemetry"));
        m_gnss->setWordWrap(true);
        root->addWidget(m_gnss);

        auto *scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("ypsMonitorScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumHeight(214);
        scroll->setMaximumHeight(310);
        auto *surface = new QWidget(scroll);
        surface->setMinimumWidth(1050);
        auto *grid = new QGridLayout(surface);
        grid->setSpacing(6);
        grid->setContentsMargins(0, 0, 0, 0);
        for (int yps = 0; yps < 12; ++yps) {
            auto *card = new QFrame(surface);
            card->setStyleSheet(QStringLiteral(
                "QFrame{background:#17232d;border:1px solid #2d4554;border-radius:5px;}"));
            auto *cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(5, 4, 5, 5);
            cardLayout->setSpacing(3);
            auto *name = new QLabel(QStringLiteral("ЯПС %1").arg(yps + 1), card);
            name->setAlignment(Qt::AlignCenter);
            name->setStyleSheet(QStringLiteral("border:none;color:#cce0eb;font-weight:600;"));
            cardLayout->addWidget(name);
            auto *inputs = new QGridLayout;
            inputs->setSpacing(2);
            for (int channel = 0; channel < 8; ++channel) {
                const int index = yps * 8 + channel;
                auto *button = new QPushButton(QString::number(channel + 1), card);
                button->setObjectName(QStringLiteral("ypsInput_%1_%2").arg(yps + 1).arg(channel + 1));
                button->setMinimumWidth(34);
                button->setMinimumHeight(32);
                button->setToolTip(QStringLiteral("Текущий уровень / накопленный фронт"));
                connect(button, &QPushButton::clicked, this, [this, index] {
                    m_selected = index;
                    refreshSelection();
                    if (onChannelSelected) onChannelSelected(index);
                });
                m_inputs[size_t(index)] = button;
                inputs->addWidget(button, channel / 4, channel % 4);
            }
            cardLayout->addLayout(inputs);
            grid->addWidget(card, yps / 6, yps % 6);
        }
        scroll->setWidget(surface);
        root->addWidget(scroll);

        auto *selection = new QHBoxLayout;
        m_selection = new QLabel(QStringLiteral("Выберите канал для проверки и назначения"), this);
        m_selection->setObjectName(QStringLiteral("selectedYpsInput"));
        m_selection->setWordWrap(true);
        selection->addWidget(m_selection, 1);
        m_assign = new QPushButton(QStringLiteral("Назначить выбранный вход…"), this);
        m_assign->setObjectName(QStringLiteral("assignObservedInput"));
        m_assign->setEnabled(false);
        connect(m_assign, &QPushButton::clicked, this, [this] {
            if (m_selected >= 0 && onAssignmentRequested)
                onAssignmentRequested(m_selected);
        });
        selection->addWidget(m_assign);
        root->addLayout(selection);
    }

    std::function<void(int)> onChannelSelected;
    std::function<void(int)> onAssignmentRequested;

    void updateTelemetry(const BoardFrame &live, const BoardFrame &collected, bool receiving) {
        m_live = live;
        m_collected = collected;
        m_receiving = receiving;
        m_link->setText(receiving ? QStringLiteral("● КАДРЫ ИДУТ") : QStringLiteral("○ НЕТ СВЕЖИХ КАДРОВ"));
        m_link->setStyleSheet(receiving ? QStringLiteral("color:#60d394;")
                                        : QStringLiteral("color:#e3b341;"));
        for (int i = 0; i < 96; ++i) {
            const auto &channel = live.channels[size_t(i)];
            const bool current = receiving && channel.ready;
            const bool fired = channel.triggered || channel.fallSeen;
            auto *button = m_inputs[size_t(i)];
            const QString label = QStringLiteral("%1 %2%3")
                .arg(i % 8 + 1)
                .arg(current ? QStringLiteral("●") : QStringLiteral("○"))
                .arg(fired ? QStringLiteral("◆") : QStringLiteral("◇"));
            if (button->text() != label) button->setText(label);
            const QString bg = !receiving ? QStringLiteral("#303741")
                : fired ? QStringLiteral("#235b42")
                : current ? QStringLiteral("#204b69") : QStringLiteral("#202d37");
            const QString border = i == m_selected ? QStringLiteral("#f1c66d")
                                                   : QStringLiteral("#39505e");
            const QString style = QStringLiteral(
                "QPushButton{background:%1;color:#edf2f7;border:2px solid %2;"
                "border-radius:4px;padding:1px;font-size:12px;}").arg(bg, border);
            if (button->styleSheet() != style) button->setStyleSheet(style);
        }
        updateGnss();
        refreshSelection();
    }

private:
    static QString utcText(quint64 micros) {
        const qint64 millis = qint64(micros / 1000);
        return QDateTime::fromMSecsSinceEpoch(millis, Qt::UTC)
            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz 'UTC'"));
    }

    void updateGnss() {
        if (!m_receiving) {
            m_gnss->setText(QStringLiteral("GNSS / UTC: нет свежей телеметрии; время ПК не подставляется"));
            return;
        }
        if (!m_live.isAa58) {
            m_gnss->setText(QStringLiteral("AA55: GNSS и UTC в этом протоколе не передаются"));
            return;
        }
        const bool fix = (m_live.timeFlags & 0x01) != 0;
        const bool valid = (m_live.timeFlags & 0x02) != 0;
        const bool fallback = (m_live.timeFlags & 0x10) != 0;
        const bool overflow = (m_live.timeFlags & 0x80) != 0;
        const bool monotonic = m_live.localNowUs >= m_live.anchorLocalUs;
        const quint64 age = monotonic ? m_live.localNowUs - m_live.anchorLocalUs : 0;
        const bool trustworthy = valid && !fallback && !overflow && monotonic
            && m_live.utcAnchorUs != 0 && m_live.anchorLocalUs != 0
            && m_live.utcAnchorUs <= std::numeric_limits<quint64>::max() - age;
        const QString quality = overflow ? QStringLiteral("ПЕРЕПОЛНЕНИЕ")
            : fallback ? QStringLiteral("РЕЗЕРВНАЯ ДАТА")
            : !monotonic ? QStringLiteral("ОШИБКА ОПОРЫ")
            : !valid ? QStringLiteral("НЕТ ОПОРЫ UTC")
            : age > 3000000ULL ? QStringLiteral("УДЕРЖАНИЕ (RMC УСТАРЕЛ)")
            : QStringLiteral("RMC (ОЦЕНКА, БЕЗ PPS)");
        const QString utc = trustworthy ? utcText(m_live.utcAnchorUs + age)
                                         : QStringLiteral("недоступно");
        m_gnss->setText(QStringLiteral("GNSS: %1 · UTC платы: %2 · %3 · возраст RMC: %4 мс · "
                                      "координаты: %5°, %6° · спутники: число не передаётся")
            .arg(fix ? QStringLiteral("FIX") : QStringLiteral("НЕТ FIX"), utc, quality)
            .arg(age / 1000)
            .arg(double(m_live.latE7) / 1e7, 0, 'f', 7)
            .arg(double(m_live.lonE7) / 1e7, 0, 'f', 7));
        m_gnss->setStyleSheet(trustworthy ? QStringLiteral("color:#bbdbea;")
                                         : QStringLiteral("color:#e3b341;"));
    }

    void refreshSelection() {
        m_assign->setEnabled(m_selected >= 0 && m_receiving);
        if (m_selected < 0) return;
        const auto &channel = m_live.channels[size_t(m_selected)];
        const auto rise = m_live.timestampUs(m_selected);
        const auto fall = m_live.timestampUs(m_selected, true);
        const auto &saved = m_collected.channels[size_t(m_selected)];
        const QString eventUtc = saved.riseUtcUs
            ? utcText(*saved.riseUtcUs) : QStringLiteral("не установлено");
        m_selection->setText(QStringLiteral(
            "ЯПС %1 / вход %2 · уровень: %3 · фронт: %4 · спад: %5 · UTC фронта: %6")
            .arg(m_selected / 8 + 1).arg(m_selected % 8 + 1)
            .arg(channel.ready ? 1 : 0)
            .arg(rise ? QString::number(*rise) + QStringLiteral(" мкс")
                      : QStringLiteral("нет отметки"))
            .arg(fall ? QString::number(*fall) + QStringLiteral(" мкс")
                      : QStringLiteral("нет отметки"), eventUtc));
    }

    std::array<QPushButton *, 96> m_inputs{};
    QLabel *m_gnss = nullptr;
    QLabel *m_link = nullptr;
    QLabel *m_selection = nullptr;
    QPushButton *m_assign = nullptr;
    BoardFrame m_live{};
    BoardFrame m_collected{};
    bool m_receiving = false;
    int m_selected = -1;
};
