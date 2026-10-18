#include "plot_widget.h"
#include <QGraphsTheme>
#include <QLineSeries>
#include <QQuickWidget>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QLabel>
#include <algorithm>
#include <cmath>

namespace imagecxx {
PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent), quick_(new QQuickWidget(this)),
    series_(new QLineSeries(this)), axisX_(new QValueAxis(this)), axisY_(new QValueAxis(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(quick_);
    series_->setColor(QColor("#1A1C1F"));
    series_->setWidth(2);
    auto *theme = new QGraphsTheme(this);
    theme->setColorScheme(QGraphsTheme::ColorScheme::Light);
    theme->setBackgroundColor(QColor("#FAF7F2"));
    theme->setPlotAreaBackgroundColor(QColor("#FAF7F2"));
    auto labelFont = font();
    labelFont.setPointSizeF(11);
    theme->setAxisXLabelFont(labelFont);
    theme->setAxisYLabelFont(labelFont);
    axisX_->setTitleFont(labelFont);
    axisY_->setTitleFont(labelFont);
    auto grid = theme->grid();
    grid.setMainColor(QColor("#D8D2C9")); grid.setSubColor(QColor("#E8E2D9"));
    grid.setMainWidth(1); grid.setSubWidth(.5); theme->setGrid(grid);
    auto axis = theme->axisX(); axis.setMainColor(QColor("#1A1C1F")); axis.setLabelTextColor(QColor("#1A1C1F"));
    theme->setAxisX(axis); theme->setAxisY(axis);
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setClearColor(QColor("#FAF7F2"));
    quick_->setMouseTracking(true);
    quick_->setToolTip(tr("Drag to pan. Scroll or pinch to zoom. Move the pointer over the plot for values."));
    quick_->setInitialProperties({{"theme", QVariant::fromValue(theme)},
        {"axisX", QVariant::fromValue(axisX_)}, {"axisY", QVariant::fromValue(axisY_)},
        {"seriesList", QVariant::fromValue(series_)}});
    quick_->loadFromModule("ImageCXX.Plots", "Plot");
    connect(quick_, &QQuickWidget::statusChanged, this, [this, layout](QQuickWidget::Status status) {
        if (status == QQuickWidget::Error) {
            auto *error = new QLabel(tr("Plot renderer could not start. Check the Qt Graphs installation."), this);
            error->setWordWrap(true);
            layout->addWidget(error);
        }
    });
}

void PlotWidget::setData(const std::vector<QPointF> &points, const QString &xTitle, const QString &yTitle)
{
    QList<QPointF> finite;
    for (const auto &point : points) if (std::isfinite(point.x()) && std::isfinite(point.y())) finite.append(point);
    series_->replace(finite);
    double xMin = 0, xMax = 1, yMin = 0, yMax = 1;
    if (!finite.empty()) {
        xMin = xMax = finite.front().x();
        yMin = yMax = finite.front().y();
        for (const auto &point : finite) {
            xMin = std::min(xMin, point.x()); xMax = std::max(xMax, point.x());
            yMin = std::min(yMin, point.y()); yMax = std::max(yMax, point.y());
        }
    }
    if (xMax <= xMin) xMax = xMin + 1;
    if (yMax <= yMin) yMax = yMin + 1;
    const double padding = (yMax - yMin) * .05;
    axisX_->setRange(xMin, xMax);
    axisY_->setRange(yMin >= 0 ? std::max(0.0, yMin - padding) : yMin - padding, yMax + padding);
    axisX_->setZoom(1);
    axisY_->setZoom(1);
    axisX_->setPan(0);
    axisY_->setPan(0);
    axisX_->setTitleText(xTitle);
    axisY_->setTitleText(yTitle);
    axisX_->setTitleVisible(true);
    axisY_->setTitleVisible(true);
    series_->setName(yTitle);
    axisX_->setLabelFormat("%.6g");
    axisY_->setLabelFormat("%.6g");
    axisX_->setLabelDecimals(-1);
    axisY_->setLabelDecimals(yTitle == "Pixel count" ? 0 : -1);
}

bool PlotWidget::ready() const { return quick_->status() == QQuickWidget::Ready; }
} // namespace imagecxx
