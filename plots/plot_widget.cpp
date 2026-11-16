#include "plot_widget.h"
#include <QCoreApplication>
#include <QGraphsTheme>
#include <QLineSeries>
#include <QScatterSeries>
#include <QQuickWidget>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace imagecxx {
PlotWidget::PlotWidget(QWidget *parent, bool scatter) : QWidget(parent), quick_(new QQuickWidget(this)),
    series_(scatter ? static_cast<QXYSeries *>(new QScatterSeries(this)) : new QLineSeries(this)),
    axisX_(new QValueAxis(this)), axisY_(new QValueAxis(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(quick_);
    series_->setColor(QColor("#18191B"));
    if (auto *line = qobject_cast<QLineSeries *>(series_)) line->setWidth(1.5);
    auto *theme = new QGraphsTheme(this);
    theme->setColorScheme(QGraphsTheme::ColorScheme::Light);
    theme->setBackgroundColor(QColor("#F7F4EE"));
    theme->setPlotAreaBackgroundColor(QColor("#FDFBF7"));
    auto labelFont = font();
    labelFont.setPointSizeF(11);
    theme->setAxisXLabelFont(labelFont);
    theme->setAxisYLabelFont(labelFont);
    axisX_->setTitleFont(labelFont);
    axisY_->setTitleFont(labelFont);
    auto grid = theme->grid();
    grid.setMainColor(QColor("#D8D1C5")); grid.setSubColor(QColor("#ECE6DC"));
    grid.setMainWidth(1); grid.setSubWidth(.5); theme->setGrid(grid);
    auto axis = theme->axisX(); axis.setMainColor(QColor("#3D3B37")); axis.setLabelTextColor(QColor("#18191B"));
    theme->setAxisX(axis); theme->setAxisY(axis);
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setClearColor(QColor("#F7F4EE"));
    quick_->setMouseTracking(true);
    quick_->setToolTip(tr("Drag to pan. Scroll or pinch to zoom. Move the pointer over the plot for values."));
    quick_->setInitialProperties({{"theme", QVariant::fromValue(theme)}, {"scatter", scatter},
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
    double xMin = 0, xMax = 1, yMin = 0, yMax = 1;
    if (!finite.empty()) {
        xMin = xMax = finite.front().x();
        yMin = yMax = finite.front().y();
        for (const auto &point : finite) {
            xMin = std::min(xMin, point.x()); xMax = std::max(xMax, point.x());
            yMin = std::min(yMin, point.y()); yMax = std::max(yMax, point.y());
        }
    }
    const auto expandRange = [](double &minimum, double &maximum) {
        if (maximum > minimum) return;
        const double expansion = std::max(1.0, std::abs(minimum) * 1e-6);
        maximum = minimum + expansion;
        if (!std::isfinite(maximum)) {
            maximum = minimum;
            minimum -= expansion;
        }
    };
    expandRange(xMin, xMax);
    expandRange(yMin, yMax);
    const auto paddedRange = [](double minimum, double maximum, bool clampZero) {
        constexpr double limit = std::numeric_limits<double>::max();
        const double padding = maximum * .05 - minimum * .05;
        const double lower = std::max(clampZero && minimum >= 0 ? 0.0 : -limit, minimum - padding);
        const double upper = std::min(limit, maximum + padding);
        return std::isfinite(upper - lower) ? std::pair{lower, upper} : std::pair{minimum, maximum};
    };
    const bool scatter = qobject_cast<QScatterSeries *>(series_);
    const auto [xLower, xUpper] = scatter ? paddedRange(xMin, xMax, false) : std::pair{xMin, xMax};
    const auto [yLower, yUpper] = paddedRange(yMin, yMax, !scatter);
    if (!std::isfinite(xUpper - xLower) || !std::isfinite(yUpper - yLower))
        throw std::length_error(QCoreApplication::translate("PlotWidget", "The values span a range too wide to plot.").toStdString());
    const auto setRange = [](QValueAxis *axis, double minimum, double maximum) {
        // Qt reconstructs the limits through their center. At a double limit,
        // rounding that reconstruction can overflow even though both ends fit.
        const double halfSpan = (maximum - minimum) / 2;
        const double center = minimum + halfSpan;
        if (std::isfinite(halfSpan)) {
            if (!std::isfinite(center - halfSpan))
                maximum = std::nextafter(maximum, std::numeric_limits<double>::infinity());
            if (!std::isfinite(center + halfSpan))
                minimum = std::nextafter(minimum, -std::numeric_limits<double>::infinity());
        }
        // Qt walks from tickAnchor to the first visible tick. Publish the new
        // range, anchor and a representable step together to bound that walk.
        QSignalBlocker blocker(axis);
        axis->setRange(minimum, maximum);
        axis->setTickAnchor(minimum);
        axis->setTickInterval(std::max({maximum / 8 - minimum / 8,
            std::max(std::abs(minimum), std::abs(maximum)) * std::numeric_limits<double>::epsilon() * 4,
            std::numeric_limits<double>::min()}));
        axis->setZoom(1);
        axis->setPan(0);
        blocker.unblock();
        emit axis->minChanged(minimum);
        emit axis->maxChanged(maximum);
        emit axis->rangeChanged(minimum, maximum);
        emit axis->tickAnchorChanged(minimum);
        emit axis->tickIntervalChanged(axis->tickInterval());
        emit axis->zoomChanged(1);
        emit axis->panChanged(0);
        emit axis->visualMinChanged(axis->visualMin());
        emit axis->visualMaxChanged(axis->visualMax());
        emit axis->update();
    };
    series_->replace(finite);
    setRange(axisX_, xLower, xUpper);
    setRange(axisY_, yLower, yUpper);
    axisX_->setTitleText(xTitle);
    axisY_->setTitleText(yTitle);
    axisX_->setTitleVisible(true);
    axisY_->setTitleVisible(true);
    series_->setName(yTitle);
    axisX_->setLabelFormat("%.6g");
    axisY_->setLabelFormat("%.6g");
    axisX_->setLabelDecimals(-1);
    axisY_->setLabelDecimals(yTitle == QCoreApplication::translate("MainWindow", "Pixel count") ? 0 : -1);
}

bool PlotWidget::ready() const { return quick_->status() == QQuickWidget::Ready; }
} // namespace imagecxx
