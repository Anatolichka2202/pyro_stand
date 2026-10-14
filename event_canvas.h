#pragma once
#include <QWidget>
#include <QColor>
#include <QVector>
struct CanvasChannel {
    QString input, role, rawTick, actual, delta, status;
    QColor color;
};
struct CanvasEvent {
    QString id, title, source, plan;
    int offsetMs = 0;
    QVector<CanvasChannel> channels;
};
// All assigned inputs are always rendered; capacity is explicit, never hidden by scrolling.
class EventCanvas final : public QWidget {
  public:
    explicit EventCanvas(QWidget *parent = nullptr);
    void setEvents(QVector<CanvasEvent> events);
    int channelCount() const;
    int columnCount() const;
    bool fits() const;
    const QVector<CanvasEvent> &events() const { return m_events; }
    QSize minimumSizeHint() const override { return {540, 220}; }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    QVector<int> breaks(int columns) const;
    int groupHeight(const CanvasEvent &) const;
    QVector<CanvasEvent> m_events;
};
