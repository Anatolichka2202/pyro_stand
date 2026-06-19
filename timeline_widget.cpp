#include "timeline_widget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFontMetrics>
#include <algorithm>
#include <climits>

// ─── Константы оформления ──────────────────────────────────────────────────────

static const QColor BG_COLOR      { 0x16, 0x1b, 0x22 };
static const QColor TICK_PENDING  { 0x8b, 0x94, 0x9e };
static const QColor TICK_OK       { 0x3f, 0xb9, 0x50 };
static const QColor TICK_FAIL     { 0xf8, 0x51, 0x49 };
static const QColor T0_COLOR      { 0xf8, 0x51, 0x49 };
static const QColor PLAYHEAD_COLOR{ 0x58, 0xa6, 0xff };
static const QColor LABEL_COLOR   { 0x8b, 0x94, 0x9e };

static constexpr int TICK_H      = 8;   // высота обычной метки
static constexpr int T0_TICK_H   = 12;  // высота метки T0
static constexpr int TICK_Y      = 4;   // отступ от верха до начала метки
static constexpr int LABEL_Y_OFF = 4;   // расстояние от нижнего края метки до текста

// ─────────────────────────────────────────────────────────────────────────────

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(52);
    setMaximumHeight(52);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

// ─── Публичный API ────────────────────────────────────────────────────────────

void TimelineWidget::setEvents(const QVector<EventRow> &events)
{
    m_events.clear();

    // Берём только события с каналами.
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

    // Вычисляем диапазон.
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
    p.setRenderHint(QPainter::Antialiasing, false);

    const int W = width();
    const int H = height();

    // Фон
    p.fillRect(0, 0, W, H, BG_COLOR);

    // Горизонтальная осевая линия
    p.setPen(QPen(LABEL_COLOR, 1));
    const int axisY = TICK_Y + T0_TICK_H + 1;
    p.drawLine(0, axisY, W, axisY);

    // Шрифт меток
    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    QFontMetrics fm(font);

    // Рисуем каждое событие
    for (const auto &te : m_events) {
        const int x = (int)std::round(msToX(te.time_ms, W));

        // Выбор цвета метки
        QColor tickColor;
        if (te.isT0) {
            tickColor = T0_COLOR;
        } else if (te.status == "ok") {
            tickColor = TICK_OK;
        } else if (te.status == "fail") {
            tickColor = TICK_FAIL;
        } else {
            tickColor = TICK_PENDING;
        }

        const int tickH = te.isT0 ? T0_TICK_H : TICK_H;
        const int tickY = TICK_Y + (te.isT0 ? 0 : (T0_TICK_H - TICK_H));

        // Вертикальная линия-метка
        p.setPen(QPen(tickColor, te.isT0 ? 2 : 1));
        p.drawLine(x, tickY, x, tickY + tickH);

        // Номер события (над меткой)
        const QString idxStr = QString::number(te.displayIdx);
        const int tw = fm.horizontalAdvance(idxStr);
        p.setPen(tickColor);
        p.drawText(x - tw / 2, tickY - 1, idxStr);

        // Временная метка (под осью)
        QString timeLabel;
        if (te.isT0) {
            timeLabel = "0";
        } else if (te.time_ms < 0) {
            timeLabel = QString("%1с").arg(te.time_ms / 1000);
        } else {
            timeLabel = QString("+%1с").arg(te.time_ms / 1000);
        }
        const int lw = fm.horizontalAdvance(timeLabel);
        const int labelY = axisY + LABEL_Y_OFF + fm.ascent();
        p.setPen(LABEL_COLOR);
        p.drawText(x - lw / 2, labelY, timeLabel);
    }

    // Плейхед
    if (m_playheadMs != INT64_MIN) {
        const int px = (int)std::round(msToX(m_playheadMs, W));
        if (px >= 0 && px < W) {
            p.setPen(QPen(PLAYHEAD_COLOR, 1));
            p.drawLine(px, 0, px, H);
        }
    }
}
