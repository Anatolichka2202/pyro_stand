#ifndef TIMELINE_WIDGET_H
#define TIMELINE_WIDGET_H

#include <QWidget>
#include <QVector>
#include "types.h"

// TimelineWidget — горизонтальная шкала времени с маркерами событий и плейхедом.
// Отображает только события с hasChannels == true.
class TimelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget *parent = nullptr);

    // Задаёт набор событий после loadCyclogram.
    void setEvents(const QVector<EventRow> &events);

    // Перемещает плейхед. msFromT0 — миллисекунды относительно T0 (может быть < 0).
    void setPlayheadMs(int64_t msFromT0);

    // Помечает событие как выполненное ("ok") или нет ("fail").
    void markEventFired(int eventId, const QString &status);

    // Сбрасывает в начальное состояние.
    void reset();

    QSize sizeHint()        const override { return QSize(800, 52); }
    QSize minimumSizeHint() const override { return QSize(200, 52); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct TimelineEvent {
        int     id        = 0;
        int     time_ms   = 0;
        QString status;   // "" | "ok" | "fail"
        bool    isT0      = false;  // LIFT_OFF_CONTACT (time_ms == 0)
        int     displayIdx = 0;     // 1-based index among timeline events
    };

    // Преобразует время в пиксельную X-координату.
    double msToX(int64_t ms, int width) const;

    QVector<TimelineEvent> m_events;
    int64_t m_rangeMin = -2000;  // мс
    int64_t m_rangeMax =  2000;  // мс
    int64_t m_playheadMs = INT64_MIN;  // INT64_MIN = не показывать
};

#endif // TIMELINE_WIDGET_H
