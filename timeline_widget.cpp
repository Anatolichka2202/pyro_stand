#include "timeline_widget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFontMetrics>
#include <algorithm>
#include <climits>

// ─── Оформление ───────────────────────────────────────────────────────────────

static const QColor BG_COLOR      { 0x16, 0x1b, 0x22 };
static const QColor AXIS_COLOR    { 0x21, 0x26, 0x2d };
static const QColor TICK_PENDING  { 0x8b, 0x94, 0x9e };
static const QColor TICK_OK       { 0x3f, 0xb9, 0x50 };
static const QColor TICK_FAIL     { 0xf8, 0x51, 0x49 };
static const QColor T0_COLOR      { 0xf8, 0x51, 0x49 };
static const QColor PLAYHEAD_COLOR{ 0x58, 0xa6, 0xff };
static const QColor LABEL_COLOR   { 0x8b, 0x94, 0x9e };
static const QColor CLUSTER_BG    { 0x30, 0x36, 0x3d };

static constexpr int DOT_R        = 5;   // радиус кружка события
static constexpr int T0_R         = 7;   // радиус кружка T0
static constexpr int AXIS_Y_FRAC  = 55;  // % от высоты виджета — ось
static constexpr int CLUSTER_PX   = 14;  // пикселей: расстояние для кластеризации
static constexpr int GLOW_W       = 3;   // ширина свечения плейхеда

// ─────────────────────────────────────────────────────────────────────────────

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(72);
    setMaximumHeight(72);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

// ─── Публичный API ────────────────────────────────────────────────────────────

void TimelineWidget::setEvents(const QVector<EventRow> &events)
{
    m_events.clear();

    int idx = 1;
    for (const auto &e : events) {
        if (!e.hasChannels) continue;
        TimelineEvent te;
        te.id         = e.id;
        te.time_ms    = e.time_ms;
        te.status     = e.status;
        te.isT0       = (e.time_ms == 0);
        te.displayIdx = idx++;
        m_events.append(te);
    }

    if (m_events.isEmpty()) {
        m_rangeMin = -2000;
        m_rangeMax =  2000;
    } else {
        int64_t minMs = m_events.first().time_ms;
        int64_t maxMs = m_events.first().time_ms;
        for (const auto &te : m_events) {
            minMs = std::min(minMs, (int64_t)te.time_ms);
            maxMs = std::max(maxMs, (int64_t)te.time_ms);
        }
        m_rangeMin = minMs - 2000;
        m_rangeMax = maxMs + 2000;
    }

    m_playheadMs = INT64_MIN;
    update();
}

void TimelineWidget::setPlayheadMs(int64_t msFromT0)
{
    m_playheadMs = msFromT0;
    update();
}

void TimelineWidget::markEventFired(int eventId, const QString &status)
{
    for (auto &te : m_events) {
        if (te.id == eventId) {
            te.status = status;
            break;
        }
    }
    update();
}

void TimelineWidget::reset()
{
    for (auto &te : m_events)
        te.status = "";
    m_playheadMs = INT64_MIN;
    update();
}

// ─── Вспомогательные методы ───────────────────────────────────────────────────

double TimelineWidget::msToX(int64_t ms, int width) const
{
    const int64_t span = m_rangeMax - m_rangeMin;
    if (span == 0) return width / 2.0;
    return (double)(ms - m_rangeMin) / (double)span * (double)width;
}

// ─── Рисование ────────────────────────────────────────────────────────────────

void TimelineWidget::paintEvent(QPaintEvent */*event*/)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int W = width();
    const int H = height();
    const int axisY = H * AXIS_Y_FRAC / 100;

    // Фон
    p.fillRect(0, 0, W, H, BG_COLOR);

    // Горизонтальная ось
    p.setPen(QPen(AXIS_COLOR, 2));
    p.drawLine(0, axisY, W, axisY);

    // Шрифт меток
    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    QFontMetrics fm(font);

    // ── Кластеризация событий ────────────────────────────────────────────────
    // Группируем события, чьи X-координаты ближе CLUSTER_PX пикселей.
    struct ClusterNode {
        int     x        = 0;
        int     count    = 0;
        bool    hasT0    = false;
        QString status;          // "" | "ok" | "fail" — статус кластера
        int     displayIdx = 0;  // индекс первого события в кластере
    };

    QVector<ClusterNode> nodes;
    for (const auto &te : m_events) {
        const int x = (int)std::round(msToX(te.time_ms, W));
        if (!nodes.isEmpty() && std::abs(nodes.last().x - x) <= CLUSTER_PX) {
            auto &last = nodes.last();
            last.count++;
            if (te.isT0) last.hasT0 = true;
            // Кластер "fail" если хоть одно событие fail, иначе "ok" если все ok
            if (te.status == "fail") last.status = "fail";
            else if (last.status != "fail" && te.status == "ok") last.status = "ok";
        } else {
            ClusterNode cn;
            cn.x          = x;
            cn.count      = 1;
            cn.hasT0      = te.isT0;
            cn.status     = te.status;
            cn.displayIdx = te.displayIdx;
            nodes.append(cn);
        }
    }

    // ── Рисуем узлы ─────────────────────────────────────────────────────────
    for (const auto &cn : nodes) {
        QColor dotColor;
        if (cn.hasT0) {
            dotColor = T0_COLOR;
        } else if (cn.status == "ok") {
            dotColor = TICK_OK;
        } else if (cn.status == "fail") {
            dotColor = TICK_FAIL;
        } else {
            dotColor = TICK_PENDING;
        }

        const int r = cn.hasT0 ? T0_R : DOT_R;

        // Кружок
        p.setPen(Qt::NoPen);
        if (cn.count > 1) {
            // Кластер: закрашенный кружок с тёмным ободком
            p.setBrush(CLUSTER_BG);
            p.drawEllipse(QPoint(cn.x, axisY), r + 2, r + 2);
            p.setBrush(dotColor.darker(130));
            p.drawEllipse(QPoint(cn.x, axisY), r, r);
        } else {
            p.setBrush(dotColor);
            p.drawEllipse(QPoint(cn.x, axisY), r, r);
        }

        // Метка над кружком: для кластера — "N×", иначе — порядковый номер
        const QString topLabel = (cn.count > 1)
            ? QString("%1×").arg(cn.count)
            : (cn.hasT0 ? "T0" : QString::number(cn.displayIdx));

        const int tw = fm.horizontalAdvance(topLabel);
        p.setPen(cn.hasT0 ? T0_COLOR : dotColor);
        p.drawText(cn.x - tw / 2, axisY - r - 3, topLabel);

        // Временная метка под осью
        if (!cn.hasT0) {
            QString timeLabel;
            // Используем первое событие кластера (cn.displayIdx соответствует исходному te)
            // Время показываем только для одиночных событий
            if (cn.count == 1) {
                const auto it = std::find_if(m_events.cbegin(), m_events.cend(),
                                             [&cn](const TimelineEvent &e){ return e.displayIdx == cn.displayIdx; });
                if (it != m_events.cend()) {
                    const int ms = it->time_ms;
                    timeLabel = (ms < 0)
                        ? QString("%1с").arg(ms / 1000)
                        : QString("+%1с").arg(ms / 1000);
                }
                const int lw = fm.horizontalAdvance(timeLabel);
                p.setPen(LABEL_COLOR);
                p.drawText(cn.x - lw / 2, axisY + r + fm.ascent() + 3, timeLabel);
            }
        } else {
            // T0 подпись под осью
            const int lw = fm.horizontalAdvance("0");
            p.setPen(LABEL_COLOR);
            p.drawText(cn.x - lw / 2, axisY + T0_R + fm.ascent() + 3, "0");
        }
    }

    // ── Плейхед с эффектом свечения ──────────────────────────────────────────
    if (m_playheadMs != INT64_MIN) {
        const int px = (int)std::round(msToX(m_playheadMs, W));
        if (px >= 0 && px < W) {
            // Свечение: несколько полупрозрачных линий с нарастающей альфой
            for (int glow = GLOW_W; glow >= 1; --glow) {
                QColor gc = PLAYHEAD_COLOR;
                gc.setAlpha(30 + (GLOW_W - glow) * 30);
                p.setPen(QPen(gc, glow * 2 + 1));
                p.drawLine(px, 0, px, H);
            }
            // Основная линия
            p.setPen(QPen(PLAYHEAD_COLOR, 1));
            p.drawLine(px, 0, px, H);
        }
    }
}
