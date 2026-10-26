#include "event_canvas.h"
#include <QPainter>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QPointer>
#include <QWheelEvent>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>
#include <limits>

QString CanvasEvent::inputGroupLabel() const {
    QStringList groups;
    for (const auto &channel : channels) {
        if (!channel.input.contains(u':')) continue;
        const auto group = channel.input.section(u':', 0, 0);
        if (!groups.contains(group)) groups.append(group);
    }
    if (!groups.isEmpty()) return QStringLiteral("ЯПС %1").arg(groups.join(QStringLiteral(", ")));
    const QString prefix = QStringLiteral("Старый · ");
    return source.startsWith(prefix) ? source.mid(prefix.size()) : source;
}

namespace {
// Ось живёт вне прокрутки, но использует геометрию и диапазон самой диаграммы.
class DiagramTimeAxis final : public QWidget {
  public:
    DiagramTimeAxis(EventCanvas *canvas, QWidget *parent) : QWidget(parent), m_canvas(canvas) {
        setObjectName(QStringLiteral("diagramTimeAxis"));
        setAccessibleName(QStringLiteral("Временная шкала диаграммы"));
        setFixedHeight(32);
        connect(canvas, &EventCanvas::presentationChanged, this, [this] { refresh(); });
        refresh();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        if (!m_canvas) return;
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#0e1820"));
        QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(12); painter.setFont(font);
        painter.setPen(QColor("#a6b3be"));
        const auto range = m_canvas->timeWindow();
        const int left = qRound(m_canvas->diagramPoint(range.first, 0).x());
        const int right = qRound(m_canvas->diagramPoint(range.second, 0).x());
        painter.drawLine(left, 0, right, 0);
        for (double tick : m_canvas->timeTicks()) {
            const int x = qRound(m_canvas->diagramPoint(tick, 0).x());
            const QString label = m_canvas->formatAxisTime(tick);
            const int labelWidth = painter.fontMetrics().horizontalAdvance(label) + 8;
            const int labelX = std::clamp(x - labelWidth / 2, 0, (std::max)(0, width() - labelWidth));
            painter.drawLine(x, 0, x, 5);
            painter.drawText(QRect(labelX, 6, labelWidth, 22), Qt::AlignCenter, label);
        }
    }

  private:
    void refresh() {
        if (!m_canvas) return;
        QStringList labels;
        for (double tick : m_canvas->timeTicks()) labels << m_canvas->formatAxisTime(tick);
        setAccessibleDescription(m_canvas->timeAxisLabel() + QStringLiteral(": ") + labels.join(QStringLiteral("; ")));
        update();
    }
    QPointer<EventCanvas> m_canvas;
};
}
EventCanvas::EventCanvas(QWidget *p) : QWidget(p) {
    setObjectName("channelCanvas");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Все назначенные входы циклограммы"));
    setFocusPolicy(Qt::StrongFocus);
}
void EventCanvas::setEvents(QVector<CanvasEvent> e) {
    m_events = std::move(e);
    updatePresentation();
}
void EventCanvas::setSelectedEvent(const QString &key) {
    if (m_selectedKey == key) return;
    m_selectedKey = key;
    m_selectedInput = -1;
    m_autoWindow = true;
    updatePresentation();
}
void EventCanvas::setSelectedInput(int index) {
    if (m_selectedInput == index) return;
    m_selectedInput = index;
    update();
}
void EventCanvas::setSelectedOnly(bool selectedOnly) {
    if (m_selectedOnly == selectedOnly) return;
    m_selectedOnly = selectedOnly;
    updatePresentation();
}
void EventCanvas::setShowRawTicks(bool show) {
    if (m_showRawTicks == show) return;
    m_showRawTicks = show;
    updatePresentation();
}
void EventCanvas::updatePresentation() {
    const bool selectionExists = std::any_of(m_events.cbegin(), m_events.cend(), [this](const CanvasEvent &group) {
        return group.source + u'/' + group.id == m_selectedKey;
    });
    if (!selectionExists) m_selectedKey.clear();
    m_visibleEvents.clear();
    for (const auto &group : m_events) {
        if (!m_selectedOnly || group.source + u'/' + group.id == m_selectedKey)
            m_visibleEvents.append(group);
    }
    QStringList text;
    for (const auto &g : m_visibleEvents) {
        text << g.id + " " + g.title + " " + g.source + " " + g.plan;
        for (const auto &r : g.channels)
            text << r.input + " " + r.role + " " + r.rawTick + " " + r.actual + " " + r.delta +
                        " " + r.status;
    }
    setAccessibleDescription((m_selectedKey.isEmpty() ? QString()
        : QStringLiteral("Выбрано: %1; ").arg(m_selectedKey)) + text.join("; "));
    int contentHeight=m_diagramMode ? 48 : 24;
    for(const auto &group:m_visibleEvents)
        contentHeight += m_diagramMode ? diagramGroupHeight(group) : groupHeight(group)+8;
    setMinimumHeight(std::max(220,contentHeight));
    const auto widths = columnWidths(0);
    int minimumWidth = 0;
    for (int width : widths) minimumWidth += width;
    setMinimumWidth(m_diagramMode ? 540 : std::max(540, minimumWidth));
    setToolTip(text.join(QStringLiteral("\n")).toHtmlEscaped());
    update();
    emit presentationChanged();
}
int EventCanvas::groupHeight(const CanvasEvent &e) const { return 42 + e.channels.size() * 24; }
int EventCanvas::channelCount() const {
    int n = 0;
    for (const auto &e : m_visibleEvents)
        n += e.channels.size();
    return n;
}
QVector<int> EventCanvas::columnWidths(int availableWidth) const {
    QFont textFont(QStringLiteral("Segoe UI")); textFont.setPixelSize(13);
    QFont numberFont(QStringLiteral("Consolas")); numberFont.setPixelSize(13);
    QFont statusFont(QStringLiteral("Segoe UI")); statusFont.setPixelSize(11);
    const QFontMetrics text(textFont), number(numberFont), status(statusFont);
    QVector<int> widths{40, 80, text.horizontalAdvance(QStringLiteral("ТИК, МКС")) + 12,
        text.horizontalAdvance(QStringLiteral("ОТ КП, МС")) + 12,
        text.horizontalAdvance(QStringLiteral("Δ, МС")) + 12,
        text.horizontalAdvance(QStringLiteral("ОЦЕНКА")) + 12};
    for (const auto &group : m_visibleEvents) {
        for (const auto &channel : group.channels) {
            widths[0] = std::max(widths[0], text.horizontalAdvance(channel.input) + 12);
            widths[2] = std::max(widths[2], number.horizontalAdvance(channel.rawTick) + 12);
            widths[3] = std::max(widths[3], number.horizontalAdvance(channel.actual) + 12);
            widths[4] = std::max(widths[4], number.horizontalAdvance(channel.delta) + 12);
            widths[5] = std::max(widths[5], status.horizontalAdvance(channel.status) + 12);
        }
    }
    if (!m_showRawTicks) widths[2] = 0;
    int reserved = 0;
    for (int i = 0; i < widths.size(); ++i) if (i != 1) reserved += widths[i];
    widths[1] = std::max(80, availableWidth - reserved);
    return widths;
}
QVector<int> EventCanvas::breaks(int columns) const {
    const int n = m_visibleEvents.size();
    columns = std::min(columns, n);
    if (!n)
        return {0};
    QVector<int> sums(n + 1);
    for (int i = 0; i < n; i++)
        sums[i + 1] = sums[i] + groupHeight(m_visibleEvents[i]) + 8;
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
    if (m_diagramMode) return 1;
    int max = std::max(1, (width() + 14) / 554);
    for (int c = 1; c <= max; c++) {
        auto b = breaks(c);
        bool ok = true;
        for (int j = 0; j + 1 < b.size(); j++) {
            int h = 0;
            for (int i = b[j]; i < b[j + 1]; i++)
                h += groupHeight(m_visibleEvents[i]) + 8;
            ok &= h - 8 <= height() - 24;
        }
        if (ok)
            return c;
    }
    return max;
}
bool EventCanvas::fits() const {
    if (m_diagramMode) return width() >= 540 && height() >= minimumHeight();
    auto b = breaks(columnCount());
    for (int j = 0; j + 1 < b.size(); j++) {
        int h = 0;
        for (int i = b[j]; i < b[j + 1]; i++)
            h += groupHeight(m_visibleEvents[i]) + 8;
        if (h - 8 > height() - 24)
            return false;
    }
    return true;
}
QRect EventCanvas::selectedEventRect() const {
    if (m_diagramMode) {
        int y = 40;
        for (const auto &group : m_visibleEvents) {
            const int h = diagramGroupHeight(group);
            if (group.source + u'/' + group.id == m_selectedKey) return {0, y, width(), h};
            y += h;
        }
        return {};
    }
    const int columns = columnCount(), gap = 14;
    const int columnWidth = (width() - (columns - 1) * gap) / columns;
    const auto boundaries = breaks(columns);
    for (int column = 0; column + 1 < boundaries.size(); ++column) {
        int y = 24;
        for (int i = boundaries[column]; i < boundaries[column + 1]; ++i) {
            const auto &group = m_visibleEvents[i];
            const int height = groupHeight(group);
            if (group.source + u'/' + group.id == m_selectedKey)
                return {column * (columnWidth + gap), y, columnWidth, height};
            y += height + 8;
        }
    }
    return {};
}
void EventCanvas::setDiagramMode(bool enabled) {
    if (m_diagramMode == enabled) return;
    m_diagramMode = enabled;
    updatePresentation();
}

QRect EventCanvas::selectedInputRect() const {
    if (!m_diagramMode || m_selectedInput < 0) return {};
    for (const auto &group : m_visibleEvents) {
        if (group.source + u'/' + group.id != m_selectedKey || m_selectedInput >= group.channels.size()) continue;
        const auto header = selectedEventRect();
        return {header.left(), header.top() + 32 + m_selectedInput * 30, header.width(), 30};
    }
    return {};
}

void EventCanvas::setTimeWindow(double centerMs, double spanMs) {
    if (!std::isfinite(centerMs) || !std::isfinite(spanMs) || spanMs < 0.01 || spanMs > 1e12 ||
        !std::isfinite(centerMs - spanMs / 2) || !std::isfinite(centerMs + spanMs / 2) ||
        centerMs - spanMs / 2 >= centerMs + spanMs / 2) return;
    m_centerMs = centerMs;
    m_spanMs = spanMs;
    m_autoWindow = false;
    update();
    emit presentationChanged();
}

void EventCanvas::fitTimeWindow() {
    m_autoWindow = true;
    update();
    emit presentationChanged();
}

void EventCanvas::zoomTimeWindow(double factor, double anchorFraction) {
    if (!std::isfinite(factor) || factor <= 0 || !std::isfinite(anchorFraction)) return;
    anchorFraction = std::clamp(anchorFraction, 0.0, 1.0);
    const auto range = timeWindow();
    const double span = range.second - range.first;
    const double nextSpan = std::clamp(span * factor, 0.01, 1e12);
    const double anchor = range.first + anchorFraction * span;
    setTimeWindow(anchor + (0.5 - anchorFraction) * nextSpan, nextSpan);
}

void EventCanvas::panTimeWindow(double fraction) {
    if (!std::isfinite(fraction)) return;
    const auto range = timeWindow();
    const double span = range.second - range.first;
    setTimeWindow(range.first + span / 2 + fraction * span, span);
}

QVector<double> EventCanvas::timeTicks() const {
    const auto range = timeWindow();
    QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(12);
    const QFontMetrics metrics(font);
    const int labelWidth = (std::max)(metrics.horizontalAdvance(formatAxisTime(range.first)),
        metrics.horizontalAdvance(formatAxisTime(range.second))) + 24;
    const double plotWidth = diagramPoint(range.second, 0).x() - diagramPoint(range.first, 0).x();
    const int intervals = std::clamp(int(plotWidth / labelWidth), 1, 6);
    const double rawStep = (range.second - range.first) / intervals;
    const double unit = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double normalized = rawStep / unit;
    const double step = (normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10) * unit;
    const double start = std::ceil(range.first / step) * step;
    QVector<double> ticks;
    for (int i = 0; i < 16; ++i) {
        const double tick = start + i * step;
        if (tick > range.second + step * 1e-9) break;
        ticks.append(std::abs(tick) < step * 1e-9 ? 0 : tick);
    }
    return ticks;
}

QString EventCanvas::formatAxisTime(double timeMs) const {
    const auto range = timeWindow();
    const double span = range.second - range.first;
    return QString::number(timeMs, 'f', span < 0.1 ? 4 : span < 20 ? 3 : span < 2000 ? 1 : 0);
}

QWidget *EventCanvas::createTimeAxis(QWidget *parent) {
    return new DiagramTimeAxis(this, parent);
}

std::pair<double, double> EventCanvas::timeWindow() const {
    if (!m_autoWindow) return {m_centerMs - m_spanMs / 2, m_centerMs + m_spanMs / 2};
    double first = std::numeric_limits<double>::infinity();
    double last = -first;
    const bool planFromT0 = usesPlanT0();
    for (const auto &group : m_visibleEvents) {
        // Локальный масштаб следует за выбранным действием; остальные действия остаются в списке.
        if (!m_selectedKey.isEmpty() && group.source + u'/' + group.id != m_selectedKey) continue;
        if (group.plannedFromKp == planFromT0) continue;
        first = (std::min)(first, double(group.offsetMs));
        last = (std::max)(last, double(group.offsetMs));
        if (!group.plannedFromKp) continue;
        for (const auto &channel : group.channels) {
            if (!channel.measuredMs || !std::isfinite(*channel.measuredMs)) continue;
            first = (std::min)(first, *channel.measuredMs);
            last = (std::max)(last, *channel.measuredMs);
        }
    }
    if (!std::isfinite(first)) return {-10, 10};
    const double margin = (std::max)(2.0, (last - first) * 0.15);
    return {first - margin, last + margin};
}

QPointF EventCanvas::diagramPoint(double timeMs, int row) const {
    const auto range = timeWindow();
    const int left = std::clamp(width() / 3, 170, 260);
    return {left + (timeMs - range.first) / (range.second - range.first) * (width() - left - 24),
            56.0 + row * 30.0};
}

int EventCanvas::diagramGroupHeight(const CanvasEvent &group) const {
    return 32 + ((group.source + u'/' + group.id == m_selectedKey) ? group.channels.size() * 30 : 0);
}

void EventCanvas::mousePressEvent(QMouseEvent *event) {
    if (!m_diagramMode || event->button() != Qt::LeftButton) { QWidget::mousePressEvent(event); return; }
    int y = 40;
    for (const auto &group : m_visibleEvents) {
        const int h = diagramGroupHeight(group);
        if (event->position().y() >= y && event->position().y() < y + h) {
            const auto key = group.source + u'/' + group.id;
            const int inputIndex = key == m_selectedKey && event->position().y() >= y + 32
                ? int((event->position().y() - y - 32) / 30) : -1;
            setSelectedEvent(key);
            emit eventSelected(key, inputIndex);
            setFocus();
            return;
        }
        y += h;
    }
}

void EventCanvas::keyPressEvent(QKeyEvent *event) {
    const bool plainKey = event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier;
    const bool shiftedPlus = event->key() == Qt::Key_Plus && event->modifiers() == Qt::ShiftModifier;
    if (m_diagramMode && (plainKey || shiftedPlus)) {
        switch (event->key()) {
        case Qt::Key_Left: panTimeWindow(-0.5); return;
        case Qt::Key_Right: panTimeWindow(0.5); return;
        case Qt::Key_Plus: case Qt::Key_Equal: zoomTimeWindow(0.5); return;
        case Qt::Key_Minus: zoomTimeWindow(2); return;
        case Qt::Key_Home: fitTimeWindow(); return;
        default: break;
        }
    }
    if (m_diagramMode && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        if (m_visibleEvents.isEmpty()) return;
        int index = -1;
        for (int i = 0; i < m_visibleEvents.size(); ++i)
            if (m_visibleEvents[i].source + u'/' + m_visibleEvents[i].id == m_selectedKey) index = i;
        index = std::clamp(index + (event->key() == Qt::Key_Down ? 1 : -1), 0, int(m_visibleEvents.size()) - 1);
        const auto &group = m_visibleEvents[index];
        const auto key = group.source + u'/' + group.id;
        setSelectedEvent(key);
        emit eventSelected(key);
        return;
    }
    QWidget::keyPressEvent(event);
}

void EventCanvas::wheelEvent(QWheelEvent *event) {
    const bool zoom = event->modifiers().testFlag(Qt::ControlModifier);
    const bool pan = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!m_diagramMode || (!zoom && !pan)) { QWidget::wheelEvent(event); return; }
    const auto range = timeWindow();
    const double left = diagramPoint(range.first, 0).x(), right = diagramPoint(range.second, 0).x();
    const int delta = pan && event->angleDelta().x() ? event->angleDelta().x() : event->angleDelta().y();
    if (!delta || event->position().x() < left || event->position().x() > right) {
        QWidget::wheelEvent(event); return;
    }
    const double steps = std::clamp(delta / 120.0, -8.0, 8.0);
    if (zoom) zoomTimeWindow(std::pow(2.0, -steps), (event->position().x() - left) / (right - left));
    else panTimeWindow(-steps * 0.25);
    event->accept();
}

void EventCanvas::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_diagramMode) emit presentationChanged();
}

bool EventCanvas::event(QEvent *event) {
    if (m_diagramMode && event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        int y = 40;
        for (const auto &group : m_visibleEvents) {
            const int h = diagramGroupHeight(group);
            if (help->pos().y() >= y && help->pos().y() < y + h) {
                QString text = group.id + u' ' + group.title + u'\n' + group.source + u'\n' + group.plan + u'\n' + group.detail;
                for (const auto &channel : group.channels)
                    text += QStringLiteral("\n%1 · %2 · тик %3 мкс · от КП %4 мс · Δ %5 мс · %6")
                        .arg(channel.input, channel.role, channel.rawTick, channel.actual, channel.delta, channel.status);
                QToolTip::showText(help->globalPos(), text.toHtmlEscaped(), this);
                return true;
            }
            y += h;
        }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void EventCanvas::paintDiagram(QPainter &p) {
    p.fillRect(rect(), QColor("#0e1820"));
    p.setRenderHint(QPainter::Antialiasing);
    QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(14); p.setFont(font);
    const int left = std::clamp(width() / 3, 170, 260);
    const int right = width() - 24;
    const auto range = timeWindow();
    const bool planFromT0 = usesPlanT0();
    p.setPen(QColor("#a6b3be"));
    p.drawText(QRect(8, 0, left - 12, 34), Qt::AlignVCenter, QStringLiteral("Действие / вход"));
    const auto selected = std::find_if(m_visibleEvents.cbegin(), m_visibleEvents.cend(), [this](const CanvasEvent &group) {
        return group.source + u'/' + group.id == m_selectedKey;
    });
    if (selected != m_visibleEvents.cend()) {
        const QString label = QStringLiteral("План %1").arg(QString::number(double(selected->offsetMs), 'f', 3));
        const int labelWidth = (std::min)(right - left, p.fontMetrics().horizontalAdvance(label) + 16);
        const double planX = diagramPoint(selected->offsetMs, 0).x();
        const int x = qRound(std::clamp(planX - labelWidth / 2.0, double(left), double(right - labelWidth)));
        p.setPen(QColor("#7fcee5"));
        p.drawText(QRect(x, 0, labelWidth, 34), Qt::AlignVCenter | Qt::AlignHCenter, label);
    }
    int contentBottom = 40;
    for (const auto &group : m_visibleEvents) contentBottom += diagramGroupHeight(group);
    for (double time : timeTicks()) {
        const int x = qRound(diagramPoint(time, 0).x());
        p.setPen(QPen(QColor("#293d49"), 1, Qt::DashLine));
        p.drawLine(x, 40, x, contentBottom);
    }
    for (const auto &group : m_visibleEvents) {
        if (group.source + u'/' + group.id != m_selectedKey || group.plannedFromKp == planFromT0) continue;
        const double x = diagramPoint(group.offsetMs, 0).x();
        if (x < left || x > right) break;
        p.setPen(QPen(QColor("#4192ad"), 1, Qt::DashLine));
        p.drawLine(QPointF(x, 40), QPointF(x, contentBottom));
        break;
    }
    const auto mark = [&](double time, int y, const QColor &color, bool expected) {
        const double point = diagramPoint(time, 0).x();
        p.setPen(QPen(color, 1.5));
        p.setBrush(expected ? QColor("#0e1820") : color);
        if (point < left || point > right) {
            const int x = point < left ? left : right;
            const int direction = point < left ? 1 : -1;
            p.drawPolygon(QPolygonF{QPointF(x, y), QPointF(x + direction * 7, y - 5), QPointF(x + direction * 7, y + 5)});
        } else p.drawEllipse(QPointF(point, y), expected ? 5 : 4, expected ? 5 : 4);
    };
    int y = 40;
    for (const auto &group : m_visibleEvents) {
        const bool selected = group.source + u'/' + group.id == m_selectedKey;
        if (selected) {
            p.fillRect(QRect(0, y, width(), 32), QColor("#193747"));
            p.fillRect(QRect(0, y, 3, diagramGroupHeight(group)), QColor("#50c8eb"));
        }
        p.setPen(QColor(selected ? "#69d9f3" : "#c2d2dc"));
        const QString title = (selected ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + group.id + u' ' + group.title;
        p.drawText(QRect(8, y, left - 14, 18), Qt::AlignVCenter, p.fontMetrics().elidedText(title, Qt::ElideRight, left - 14));
        QFont sourceFont = font; sourceFont.setPixelSize(11); p.setFont(sourceFont); p.setPen(group.color);
        p.drawText(QRect(22, y + 18, left - 28, 14), Qt::AlignVCenter,
            p.fontMetrics().elidedText(group.status + QStringLiteral(" · ") + group.inputGroupLabel(), Qt::ElideRight, left - 28));
        p.setFont(font);
        const bool compatible = group.plannedFromKp != planFromT0;
        if (compatible) {
            mark(double(group.offsetMs), y + 16, QColor("#b8cedc"), true);
            for (const auto &channel : group.channels)
                if (!planFromT0 && channel.measuredMs && std::isfinite(*channel.measuredMs)) mark(*channel.measuredMs, y + 16, channel.color, false);
        } else {
            p.setPen(QColor("#8fa7b5"));
            p.drawText(QRect(left + 12, y, right-left-20, 32), Qt::AlignVCenter | Qt::AlignRight,
                QStringLiteral("Нет привязки к этой шкале"));
        }
        p.setPen(QColor("#273b47")); p.drawLine(0, y + 32, width(), y + 32);
        y += 32;
        if (!selected) continue;
        int inputIndex = 0;
        for (const auto &channel : group.channels) {
            const bool selectedInput = inputIndex++ == m_selectedInput;
            if (selectedInput) p.fillRect(QRect(3, y, width() - 3, 30), QColor("#163344"));
            p.setPen(QColor("#c2d2dc"));
            const auto label = channel.input + QStringLiteral("  ") + channel.role;
            p.drawText(QRect(16, y, left - 22, 30), Qt::AlignVCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, left - 22));
            if (compatible) mark(double(group.offsetMs), y + 15, QColor("#b8cedc"), true);
            if (compatible && !planFromT0 && channel.measuredMs && std::isfinite(*channel.measuredMs)) {
                const int x1 = qRound(std::clamp(diagramPoint(group.offsetMs, 0).x(), double(left), double(right)));
                const int x2 = qRound(std::clamp(diagramPoint(*channel.measuredMs, 0).x(), double(left), double(right)));
                p.setPen(QPen(channel.color, 1)); p.drawLine(x1, y + 15, x2, y + 15);
                mark(*channel.measuredMs, y + 15, channel.color, false);
                // Подпись факта остаётся в пределах графика и не перекрывает метку.
                const double actualX = diagramPoint(*channel.measuredMs, 0).x();
                if (actualX >= left && actualX <= right) {
                    const int labelWidth = p.fontMetrics().horizontalAdvance(channel.actual) + 8;
                    const double intervalLeft = (std::min)(double(x1), actualX);
                    const double intervalRight = (std::max)(double(x1), actualX);
                    const bool fitsRight = intervalRight + labelWidth + 12 <= right;
                    const bool fitsLeft = intervalLeft - labelWidth - 12 >= left;
                    if (fitsRight || fitsLeft) {
                        const int labelX = fitsRight ? qRound(intervalRight) + 9 : qRound(intervalLeft) - labelWidth - 9;
                        p.save(); p.setClipRect(QRect(left, y, right - left + 1, 30));
                        p.fillRect(QRect(labelX, y + 4, labelWidth, 22), QColor(selectedInput ? "#163344" : "#0e1820"));
                        p.setPen(channel.color);
                        p.drawText(QRect(labelX + 4, y, labelWidth - 8, 30), Qt::AlignVCenter, channel.actual);
                        p.restore();
                    }
                }
            } else {
                p.setPen(channel.color);
                p.drawText(QRect(left + 12, y, right-left-20, 30), Qt::AlignVCenter | Qt::AlignRight, channel.status);
            }
            y += 30;
            p.setPen(QColor("#20323d")); p.drawLine(0, y, width(), y);
        }
    }
}

QString EventCanvas::timeAxisLabel() const {
    return usesPlanT0() ? QStringLiteral("План от T0, мс; связь с КП не определена") : QStringLiteral("От физического КП, мс");
}

bool EventCanvas::usesPlanT0() const {
    return std::any_of(m_visibleEvents.cbegin(), m_visibleEvents.cend(), [this](const CanvasEvent &group) {
        return !group.plannedFromKp && (m_selectedKey.isEmpty() || group.source + u'/' + group.id == m_selectedKey);
    });
}

void EventCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    if (m_diagramMode) { paintDiagram(p); return; }
    p.fillRect(rect(), QColor("#10161c"));
    QFont f("Segoe UI");
    f.setPixelSize(13);
    p.setFont(f);
    const int columns = columnCount(), gap = 14, cw = (width() - (columns - 1) * gap) / columns;
    const auto widths = columnWidths(cw);
    auto b = breaks(columns);
    for (int c = 0; c + 1 < b.size(); c++) {
        int y = 24, x = c * (cw + gap);
        for (int i = b[c]; i < b[c + 1]; i++) {
            const auto &g = m_visibleEvents[i];
            const bool selected = g.source + u'/' + g.id == m_selectedKey;
            const int h = groupHeight(g);
            p.fillRect(QRect(x, y, cw, 24), QColor(selected ? "#203d52" : "#19222b"));
            p.setPen(QColor("#81b6d0"));
            p.drawText(QRect(x + 6, y, cw - 150, 24), Qt::AlignVCenter,
                       p.fontMetrics().elidedText((selected ? QStringLiteral("> ") : QString())+g.id+"  "+g.title,Qt::ElideRight,cw-155));
            p.setPen(QColor("#e8edf1"));
            p.drawText(QRect(x + cw - 145, y, 139, 24), Qt::AlignRight | Qt::AlignVCenter, g.plan);
            // Сначала выделяем место числам, оставшуюся ширину отдаём названию сигнала.
            int colX = x;
            const QStringList heads = {QStringLiteral("ВХ."),      g.source,
                                       QStringLiteral("ТИК, МКС"), QStringLiteral("ОТ КП, МС"),
                                       QStringLiteral("Δ, МС"),    QStringLiteral("ОЦЕНКА")};
            p.setPen(QColor("#a6b3be"));
            for (int j = 0; j < 6; j++) {
                if (!widths[j]) continue;
                p.drawText(QRect(colX + 3, y + 24, widths[j] - 6, 18),
                           (j >= 2 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, heads[j]);
                colX += widths[j];
            }
            int rowY = y + 42;
            for (const auto &r : g.channels) {
                QStringList vals = {r.input, r.role, r.rawTick, r.actual, r.delta, r.status};
                colX = x;
                for (int j = 0; j < 6; j++) {
                    if (!widths[j]) continue;
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
            p.setPen(QColor(selected ? "#58a6ff" : "#35434f"));
            p.drawRect(x, y, cw - 1, h);
            y += h + 8;
        }
    }
    p.setPen(QColor(fits() ? "#a6b3be" : "#e5bd78"));
    p.drawText(QRect(0, 0, width(), 20), Qt::AlignVCenter,
               QStringLiteral("Назначенных входов: %1%2")
                   .arg(channelCount())
                   .arg(fits() ? QString()
                               : QStringLiteral(" · увеличьте окно: все строки не помещаются")));
    if (m_visibleEvents.isEmpty())
        p.drawText(rect(), Qt::AlignCenter,
                   m_selectedOnly ? QStringLiteral("Нет выбранных назначенных входов")
                                  : QStringLiteral("Ожидание циклограммы и назначения входов"));
}
