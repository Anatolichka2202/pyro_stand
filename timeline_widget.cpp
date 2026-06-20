#include "timeline_widget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QFontMetrics>
#include <algorithm>
#include <climits>

// ─── Константы оформления ──────────────────────────────────────────────────────

static const QColor BG_COLOR        { 0x16, 0x1b, 0x22 };
static const QColor AXIS_COLOR      { 0x30, 0x36, 0x3d };
static const QColor TICK_PENDING    { 0x8b, 0x94, 0x9e };
static const QColor TICK_OK         { 0x3f, 0xb9, 0x50 };
static const QColor TICK_FAIL       { 0xf8, 0x51, 0x49 };
static const QColor T0_COLOR        { 0xf8, 0x51, 0x49 };
static const QColor PLAYHEAD_COLOR  { 0x58, 0xa6, 0xff };
static const QColor LABEL_COLOR     { 0x8b, 0x94, 0x9e };

static constexpr int WIDGET_H   = 64;
static constexpr int AXIS_Y     = 34;    // ось — чуть выше центра
static constexpr int DOT_R      = 5;     // радиус кружка события
static constexpr int T0_W       = 2;     // ширина T0-линии
static constexpr int PAD_H      = 20;    // горизонтальный отступ
static constexpr int NUM_Y_OFF  = 12;    // номер: расстояние от кружка вверх
static constexpr int TIME_Y_OFF = 10;    // время: расстояние от оси вниз

// ─────────────────────────────────────────────────────────────────────────────

TimelineWidget::TimelineWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(WIDGET_H);
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
    const double usable = width - 2.0 * PAD_H;
    return PAD_H + (double)(ms - m_rangeMin) / (double)span * usable;
}

// ─── Рисование ────────────────────────────────────────────────────────────────

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int W = width();
    const int H = height();

    // Фон
    p.fillRect(0, 0, W, H, BG_COLOR);

    // Горизонтальная ось
    p.setPen(QPen(AXIS_COLOR, 1));
    p.drawLine(PAD_H, AXIS_Y, W - PAD_H, AXIS_Y);

    // Шрифты
    QFont numFont = p.font();
    numFont.setPixelSize(9);
    numFont.setWeight(QFont::DemiBold);
    QFontMetrics numFm(numFont);

    QFont timeFont = p.font();
    timeFont.setPixelSize(9);
    QFontMetrics timeFm(timeFont);

    // ── Рисуем события (не T0) ────────────────────────────────────────────────
    for (const auto &te : m_events) {
        if (te.isT0) continue;

        const int x = (int)std::round(msToX(te.time_ms, W));

        // Цвет и заливка кружка
        QColor dotColor;
        bool filled = false;
        if (te.status == "ok") {
            dotColor = TICK_OK;   filled = true;
        } else if (te.status == "fail") {
            dotColor = TICK_FAIL; filled = true;
        } else {
            dotColor = TICK_PENDING;
        }

        p.setPen(QPen(dotColor, 1.5));
        p.setBrush(filled ? QBrush(dotColor) : Qt::NoBrush);
        p.drawEllipse(QPoint(x, AXIS_Y), DOT_R, DOT_R);

        // Номер события над кружком
        const QString idxStr = QString::number(te.displayIdx);
        const int tw = numFm.horizontalAdvance(idxStr);
        p.setFont(numFont);
        p.setPen(dotColor);
        p.drawText(x - tw / 2, AXIS_Y - DOT_R - NUM_Y_OFF + numFm.ascent(), idxStr);

        // Временная метка под осью
        const QString timeLabel = (te.time_ms < 0)
            ? QString("%1с").arg(te.time_ms / 1000)
            : QString("+%1с").arg(te.time_ms / 1000);
        const int lw = timeFm.horizontalAdvance(timeLabel);
        p.setFont(timeFont);
        p.setPen(LABEL_COLOR);
        p.drawText(x - lw / 2, AXIS_Y + TIME_Y_OFF + timeFm.ascent(), timeLabel);
    }

    // ── T0-маркер поверх всего ───────────────────────────────────────────────
    for (const auto &te : m_events) {
        if (!te.isT0) continue;
        const int x = (int)std::round(msToX(te.time_ms, W));

        // Красная вертикальная линия на всю высоту виджета
        p.setPen(QPen(T0_COLOR, T0_W));
        p.drawLine(x, 4, x, H - 4);

        // Надпись "T0" сверху
        QFont t0Font = p.font();
        t0Font.setPixelSize(9);
        t0Font.setWeight(QFont::Bold);
        p.setFont(t0Font);
        QFontMetrics t0Fm(t0Font);
        const QString t0Str = "T0";
        const int tw = t0Fm.horizontalAdvance(t0Str);
        p.setPen(T0_COLOR);
        p.drawText(x - tw / 2, 3 + t0Fm.ascent(), t0Str);

        break;
    }

    // ── Playhead с glow ───────────────────────────────────────────────────────
    if (m_playheadMs != INT64_MIN) {
        const int px = (int)std::round(msToX(m_playheadMs, W));
        if (px >= PAD_H && px <= W - PAD_H) {
            // Свечение: несколько широких полупрозрачных слоёв
            const int glowWidths[]  = { 7, 5, 3 };
            const int glowAlphas[]  = { 25, 50, 90 };
            for (int i = 0; i < 3; ++i) {
                QColor gc = PLAYHEAD_COLOR;
                gc.setAlpha(glowAlphas[i]);
                p.setPen(QPen(gc, glowWidths[i]));
                p.drawLine(px, 0, px, H);
            }
            // Яркое ядро
            p.setPen(QPen(PLAYHEAD_COLOR, 1));
            p.drawLine(px, 0, px, H);
        }
    }
}
