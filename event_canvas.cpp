#include "event_canvas.h"
#include <QPainter>
#include <QFontMetrics>
#include <algorithm>
#include <limits>
EventCanvas::EventCanvas(QWidget *p) : QWidget(p) {
    setObjectName("channelCanvas");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Все назначенные входы циклограммы"));
}
void EventCanvas::setEvents(QVector<CanvasEvent> e) {
    m_events = std::move(e);
    QStringList text;
    for (const auto &g : m_events) {
        text << g.id + " " + g.title + " " + g.source + " " + g.plan;
        for (const auto &r : g.channels)
            text << r.input + " " + r.role + " " + r.rawTick + " " + r.actual + " " + r.delta +
                        " " + r.status;
    }
    setAccessibleDescription(text.join("; "));
    int contentHeight=24;
    for(const auto &group:m_events) contentHeight+=groupHeight(group)+8;
    setMinimumHeight(std::max(220,contentHeight));
    setToolTip(text.join(QStringLiteral("\n")).toHtmlEscaped());
    update();
}
int EventCanvas::groupHeight(const CanvasEvent &e) const { return 42 + e.channels.size() * 24; }
int EventCanvas::channelCount() const {
    int n = 0;
    for (const auto &e : m_events)
        n += e.channels.size();
    return n;
}
QVector<int> EventCanvas::breaks(int columns) const {
    const int n = m_events.size();
    columns = std::min(columns, n);
    if (!n)
        return {0};
    QVector<int> sums(n + 1);
    for (int i = 0; i < n; i++)
        sums[i + 1] = sums[i] + groupHeight(m_events[i]) + 8;
    QVector<QVector<int>> dp(columns + 1, QVector<int>(n + 1, std::numeric_limits<int>::max() / 2)),
        cuts = dp;
    dp[0][0] = 0;
    for (int k = 1; k <= columns; k++)
        for (int i = k; i <= n; i++)
            for (int j = k - 1; j < i; j++) {
                int h = std::max(dp[k - 1][j], sums[i] - sums[j] - 8);
                if (h < dp[k][i]) {
                    dp[k][i] = h;
                    cuts[k][i] = j;
                }
            }
    QVector<int> out{n};
    int i = n;
    for (int k = columns; k > 0; k--) {
        i = cuts[k][i];
        out.prepend(i);
    }
    return out;
}
int EventCanvas::columnCount() const {
    int max = std::max(1, (width() + 14) / 554);
    for (int c = 1; c <= max; c++) {
        auto b = breaks(c);
        bool ok = true;
        for (int j = 0; j + 1 < b.size(); j++) {
            int h = 0;
            for (int i = b[j]; i < b[j + 1]; i++)
                h += groupHeight(m_events[i]) + 8;
            ok &= h - 8 <= height() - 24;
        }
        if (ok)
            return c;
    }
    return max;
}
bool EventCanvas::fits() const {
    auto b = breaks(columnCount());
    for (int j = 0; j + 1 < b.size(); j++) {
        int h = 0;
        for (int i = b[j]; i < b[j + 1]; i++)
            h += groupHeight(m_events[i]) + 8;
        if (h - 8 > height() - 24)
            return false;
    }
    return true;
}
void EventCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#10161c"));
    QFont f("Segoe UI");
    f.setPixelSize(13);
    p.setFont(f);
    const int columns = columnCount(), gap = 14, cw = (width() - (columns - 1) * gap) / columns;
    auto b = breaks(columns);
    for (int c = 0; c + 1 < b.size(); c++) {
        int y = 24, x = c * (cw + gap);
        for (int i = b[c]; i < b[c + 1]; i++) {
            const auto &g = m_events[i];
            const int h = groupHeight(g);
            p.fillRect(QRect(x, y, cw, 24), QColor("#19222b"));
            p.setPen(QColor("#81b6d0"));
            p.drawText(QRect(x + 6, y, cw - 150, 24), Qt::AlignVCenter,
                       p.fontMetrics().elidedText(g.id+"  "+g.title,Qt::ElideRight,cw-155));
            p.setPen(QColor("#e8edf1"));
            p.drawText(QRect(x + cw - 145, y, 139, 24), Qt::AlignRight | Qt::AlignVCenter, g.plan);
            // Сначала выделяем место числам, оставшуюся ширину отдаём названию сигнала.
            const int widths[] = {40, cw - 370, 92, 96, 64, 78};
            int colX = x;
            const QStringList heads = {QStringLiteral("ВХ."),      g.source,
                                       QStringLiteral("ТИК, МКС"), QStringLiteral("ОТ КП, МС"),
                                       QStringLiteral("Δ, МС"),    QStringLiteral("ОЦЕНКА")};
            p.setPen(QColor("#a6b3be"));
            for (int j = 0; j < 6; j++) {
                p.drawText(QRect(colX + 3, y + 24, widths[j] - 6, 18),
                           (j >= 2 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, heads[j]);
                colX += widths[j];
            }
            int rowY = y + 42;
            for (const auto &r : g.channels) {
                QStringList vals = {r.input, r.role, r.rawTick, r.actual, r.delta, r.status};
                colX = x;
                for (int j = 0; j < 6; j++) {
                    QFont rf(j >= 2 && j <= 4 ? "Consolas" : "Segoe UI");
                    rf.setPixelSize(j == 5 ? 11 : 13);
                    p.setFont(rf);
                    p.setPen(j >= 3 ? r.color : QColor("#e8edf1"));
                    p.drawText(QRect(colX + 3, rowY, widths[j] - 6, 24),
                               (j >= 2 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                               p.fontMetrics().elidedText(vals[j],Qt::ElideRight,std::max(0,widths[j]-6)));
                    colX += widths[j];
                }
                p.setPen(QColor("#35434f"));
                p.drawLine(x, rowY + 24, x + cw, rowY + 24);
                rowY += 24;
            }
            p.setFont(f);
            p.setPen(QColor("#35434f"));
            p.drawRect(x, y, cw - 1, h);
            y += h + 8;
        }
    }
    p.setPen(QColor(fits() ? "#a6b3be" : "#e5bd78"));
    p.drawText(QRect(0, 0, width(), 20), Qt::AlignVCenter,
               QStringLiteral("%1 входов · %2 столбца%3")
                   .arg(channelCount())
                   .arg(columns)
                   .arg(fits() ? QString()
                               : QStringLiteral(" · увеличьте окно: все строки не помещаются")));
    if (m_events.isEmpty())
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("Ожидание циклограммы и назначения входов"));
}
