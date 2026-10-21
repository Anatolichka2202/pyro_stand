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
    QString actual, delta, status, detail;
    QColor color;
    qint64 offsetMs = 0;
    QVector<CanvasChannel> channels;
};
// Показываем все назначенные входы; доступная высота определяется числом строк.
class EventCanvas final : public QWidget {
  public:
    explicit EventCanvas(QWidget *parent = nullptr);
    void setEvents(QVector<CanvasEvent> events);
    void setSelectedEvent(const QString &key);
    void setSelectedOnly(bool selectedOnly);
    int channelCount() const;
    int columnCount() const;
    bool fits() const;
    QVector<int> columnWidths(int availableWidth) const;
    const QVector<CanvasEvent> &events() const { return m_events; }
    QSize minimumSizeHint() const override { return {540, 220}; }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    QVector<int> breaks(int columns) const;
    int groupHeight(const CanvasEvent &) const;
    void updatePresentation();
    QVector<CanvasEvent> m_events;
    QVector<CanvasEvent> m_visibleEvents;
    QString m_selectedKey;
    bool m_selectedOnly = false;
};
