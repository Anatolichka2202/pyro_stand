#pragma once
#include <QWidget>
#include <QColor>
#include <QVector>
#include <optional>
#include <utility>
struct CanvasChannel {
    QString input, role, rawTick, actual, delta, status;
    QColor color;
    // Только подтверждённая относительная отметка; приблизительное время сюда не попадает.
    std::optional<double> measuredMs = std::nullopt;
};
struct CanvasEvent {
    QString id, title, source, plan;
    QString actual, delta, status, detail;
    QColor color;
    qint64 offsetMs = 0;
    QVector<CanvasChannel> channels;
    bool plannedFromKp = true;
    QString inputGroupLabel() const;
};
// Диаграмма отметок и подробный поканальный обзор используют одни результаты измерений.
class EventCanvas final : public QWidget {
    Q_OBJECT
  public:
    explicit EventCanvas(QWidget *parent = nullptr);
    void setEvents(QVector<CanvasEvent> events);
    void setSelectedEvent(const QString &key);
    void setSelectedInput(int index);
    void setSelectedOnly(bool selectedOnly);
    void setShowRawTicks(bool show);
    void setDiagramMode(bool enabled);
    void setTimeWindow(double centerMs, double spanMs);
    void zoomTimeWindow(double factor, double anchorFraction = 0.5);
    void panTimeWindow(double fraction);
    void fitTimeWindow();
    std::pair<double, double> timeWindow() const;
    QString timeAxisLabel() const;
    QPointF diagramPoint(double timeMs, int row) const;
    QVector<double> timeTicks() const;
    QString formatAxisTime(double timeMs) const;
    QWidget *createTimeAxis(QWidget *parent);
    QRect selectedEventRect() const;
    QRect selectedInputRect() const;
    int channelCount() const;
    int columnCount() const;
    bool fits() const;
    QVector<int> columnWidths(int availableWidth) const;
    const QVector<CanvasEvent> &events() const { return m_events; }
    QSize minimumSizeHint() const override { return {540, 220}; }

  signals:
    void eventSelected(const QString &key, int inputIndex = -1);
    void presentationChanged();

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *) override;

  private:
    QVector<int> breaks(int columns) const;
    int groupHeight(const CanvasEvent &) const;
    void updatePresentation();
    void paintDiagram(QPainter &painter);
    bool usesPlanT0() const;
    int diagramGroupHeight(const CanvasEvent &group) const;
    QVector<CanvasEvent> m_events;
    QVector<CanvasEvent> m_visibleEvents;
    QString m_selectedKey;
    int m_selectedInput = -1;
    bool m_selectedOnly = false;
    bool m_showRawTicks = false;
    bool m_diagramMode = false;
    bool m_autoWindow = true;
    double m_centerMs = 0;
    double m_spanMs = 20;
};
