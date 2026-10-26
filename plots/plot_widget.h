#pragma once
#include <QWidget>
#include <QPointF>
#include <vector>
class QQuickWidget;
class QLineSeries;
class QValueAxis;
namespace imagecxx {
// Constructed only when a plot is requested. QML stays inside this module.
class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget *parent = nullptr);
    // Throws if the coordinate range exceeds what Qt Graphs can represent.
    void setData(const std::vector<QPointF> &points, const QString &xTitle, const QString &yTitle);
    bool ready() const;
private:
    QQuickWidget *quick_;
    QLineSeries *series_;
    QValueAxis *axisX_;
    QValueAxis *axisY_;
};
}
