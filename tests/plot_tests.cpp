#include "plots/plot_widget.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QLineSeries>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWidget>
#include <QThread>
#include <QValueAxis>
#include <QWheelEvent>
#include <cmath>
#include <limits>

using namespace imagecxx;

namespace {
void renderPlot()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 100) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

void plotMouse(QWidget *target, QEvent::Type type, QPointF position, Qt::MouseButton button,
               Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, target->mapToGlobal(position.toPoint()), button, buttons, {});
    QCoreApplication::sendEvent(target, &event);
}
}

TEST(Plots, ReplacingDataResetsNavigationAndKeepsOnlyFiniteSamples)
{
    PlotWidget plot;
    plot.setData({{0, 10}, {1, 15}, {2, 12}}, "Distance", "Intensity");
    auto *quick = plot.findChild<QQuickWidget *>();
    ASSERT_NE(quick, nullptr);
    ASSERT_TRUE(plot.ready());
    auto *graph = quick->rootObject();
    auto *x = qobject_cast<QValueAxis *>(graph->property("axisX").value<QObject *>());
    auto *y = qobject_cast<QValueAxis *>(graph->property("axisY").value<QObject *>());
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    x->setZoom(3);
    y->setZoom(2);
    x->setPan(4);
    y->setPan(-10);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    plot.setData({{10, 4}, {20, 5}, {nan, 2}, {30, nan}}, "Intensity", "Pixel count");
    EXPECT_DOUBLE_EQ(x->zoom(), 1);
    EXPECT_DOUBLE_EQ(y->zoom(), 1);
    EXPECT_DOUBLE_EQ(x->pan(), 0);
    EXPECT_DOUBLE_EQ(y->pan(), 0);
    EXPECT_DOUBLE_EQ(x->min(), 10);
    EXPECT_DOUBLE_EQ(x->max(), 20);
    auto *series = plot.findChild<QLineSeries *>();
    ASSERT_NE(series, nullptr);
    EXPECT_EQ(series->points(), (QList<QPointF>{{10, 4}, {20, 5}}));
    EXPECT_EQ(y->labelDecimals(), 0);
    plot.setData({}, "Distance", "Intensity");
    EXPECT_EQ(series->count(), 0);
    EXPECT_LT(x->min(), x->max());
    EXPECT_LT(y->min(), y->max());
    plot.setData({{2, 5}}, "Distance", "Intensity");
    EXPECT_LT(x->min(), x->max());
    EXPECT_LT(y->min(), y->max());
}

TEST(Plots, NativeMouseZoomPanHoverAndReset)
{
    if (QApplication::platformName() == "offscreen") GTEST_SKIP() << "Requires a native Quick scene";
    PlotWidget plot;
    plot.resize(640, 360);
    plot.setData({{0, 0}, {140, 70000}, {281.63, 0}}, "Distance [mm]", "Intensity");
    plot.show();
    renderPlot();
    auto *quick = plot.findChild<QQuickWidget *>();
    ASSERT_NE(quick, nullptr);
    ASSERT_TRUE(plot.ready());
    auto *graph = quick->rootObject();
    auto *x = qobject_cast<QValueAxis *>(graph->property("axisX").value<QObject *>());
    auto *y = qobject_cast<QValueAxis *>(graph->property("axisY").value<QObject *>());
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    auto area = graph->property("plotArea").toRectF();
    ASSERT_GT(area.width(), 400);
    ASSERT_GT(area.height(), 230);
    const QPointF center = area.center();
    QWheelEvent wheel(center, quick->mapToGlobal(center.toPoint()), {}, {0, 120}, {}, {},
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(quick, &wheel);
    renderPlot();
    EXPECT_NE(x->zoom(), 1);
    EXPECT_NE(y->zoom(), 1);
    plotMouse(quick, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    plotMouse(quick, QEvent::MouseMove, center + QPointF(15, 10), Qt::NoButton, Qt::LeftButton);
    plotMouse(quick, QEvent::MouseMove, center + QPointF(40, 25), Qt::NoButton, Qt::LeftButton);
    plotMouse(quick, QEvent::MouseButtonRelease, center + QPointF(40, 25), Qt::LeftButton, {});
    renderPlot();
    EXPECT_NE(x->pan(), 0);
    EXPECT_NE(y->pan(), 0);
    auto *reset = graph->findChild<QQuickItem *>("plotReset");
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(reset->isEnabled());
    const auto resetCenter = reset->mapToItem(graph, QPointF(reset->width() / 2, reset->height() / 2));
    plotMouse(quick, QEvent::MouseButtonPress, resetCenter, Qt::LeftButton, Qt::LeftButton);
    plotMouse(quick, QEvent::MouseButtonRelease, resetCenter, Qt::LeftButton, {});
    renderPlot();
    EXPECT_DOUBLE_EQ(x->zoom(), 1);
    EXPECT_DOUBLE_EQ(y->zoom(), 1);
    EXPECT_DOUBLE_EQ(x->pan(), 0);
    EXPECT_DOUBLE_EQ(y->pan(), 0);
    EXPECT_FALSE(reset->isEnabled());
    area = graph->property("plotArea").toRectF();
    const QPointF pointer(area.left() + area.width() * 84 / 281.63, area.bottom() - 20);
    plotMouse(quick, QEvent::MouseMove, pointer, Qt::NoButton, {});
    renderPlot();
    auto *cursor = graph->findChild<QQuickItem *>("plotCursor");
    auto *readout = graph->findChild<QQuickItem *>("plotReadoutText");
    auto *interpolation = graph->findChild<QQuickItem *>("plotInterpolation");
    ASSERT_NE(cursor, nullptr);
    ASSERT_NE(readout, nullptr);
    ASSERT_NE(interpolation, nullptr);
    EXPECT_FALSE(interpolation->property("checked").toBool());
    EXPECT_TRUE(cursor->isVisible());
    EXPECT_TRUE(readout->isVisible());
    EXPECT_TRUE(readout->property("text").toString().contains("Distance [mm]"));
    EXPECT_EQ(graph->property("sample").toPointF(), QPointF(140, 70000));
    EXPECT_NEAR(cursor->x(), area.left() + area.width() * 140 / 281.63, 1);
    const auto capture = qEnvironmentVariable("IMAGECXX_PLOT_CAPTURE_PATH");
    if (!capture.isEmpty()) EXPECT_TRUE(plot.grab().save(capture + "snapped.png"));

    const auto checkboxCenter = interpolation->mapToItem(graph,
        QPointF(interpolation->width() / 2, interpolation->height() / 2));
    plotMouse(quick, QEvent::MouseButtonPress, checkboxCenter, Qt::LeftButton, Qt::LeftButton);
    plotMouse(quick, QEvent::MouseButtonRelease, checkboxCenter, Qt::LeftButton, {});
    plotMouse(quick, QEvent::MouseMove, pointer, Qt::NoButton, {});
    renderPlot();
    EXPECT_TRUE(interpolation->property("checked").toBool());
    EXPECT_NEAR(graph->property("sample").toPointF().x(), 84, .01);
    EXPECT_NEAR(graph->property("sample").toPointF().y(), 42000, 1);
    EXPECT_NEAR(cursor->x(), pointer.x(), 1);
    if (!capture.isEmpty()) EXPECT_TRUE(plot.grab().save(capture + "interpolated.png"));
    // A same-sized replacement must refresh the readout even if X and Y ranges stay unchanged.
    plot.setData({{0, 0}, {140, 50000}, {281.63, 70000}}, "Distance [mm]", "Intensity");
    renderPlot();
    EXPECT_NEAR(graph->property("sample").toPointF().y(), 30000, 1);
    plotMouse(quick, QEvent::MouseMove, {2, 2}, Qt::NoButton, {});
    renderPlot();
    EXPECT_FALSE(cursor->isVisible());
    EXPECT_FALSE(readout->isVisible());
}

TEST(Plots, CompactLayoutFitsLargeAndSmallValues)
{
    if (QApplication::platformName() == "offscreen") GTEST_SKIP() << "Requires a native Quick scene";
    PlotWidget plot;
    std::vector<QPointF> points;
    for (int i = 0; i < 100; ++i) {
        const double x = i * 281.63 / 99;
        const double y = 80000 * std::exp(-std::pow((x - 98) / 8, 2))
                       + 125000 * std::exp(-std::pow((x - 199) / 10, 2));
        points.emplace_back(x, y);
    }
    plot.setData(points, "Distance [mm]", "Intensity");
    plot.resize(640, 360);
    plot.show();
    renderPlot();
    auto *quick = plot.findChild<QQuickWidget *>();
    ASSERT_NE(quick, nullptr);
    auto *graph = quick->rootObject();
    const auto capture = qEnvironmentVariable("IMAGECXX_PLOT_CAPTURE_PATH");
    for (const auto size : {QSize(640, 360), QSize(420, 240)}) {
        plot.resize(size);
        renderPlot();
        const auto area = graph->property("plotArea").toRectF();
        EXPECT_LT(area.left(), 110);
        EXPECT_LT(plot.height() - area.bottom(), 85);
        EXPECT_GT(area.width(), plot.width() - 150);
        if (!capture.isEmpty())
            EXPECT_TRUE(plot.grab().save(capture + QString::number(size.width()) + ".png"));
    }
    plot.setData({{0.00001, -0.00003}, {0.00002, 0.00008}}, "Distance", "Intensity");
    renderPlot();
    const auto area = graph->property("plotArea").toRectF();
    EXPECT_GT(area.width(), 200);
    if (!capture.isEmpty()) EXPECT_TRUE(plot.grab().save(capture + "small-values.png"));
}
