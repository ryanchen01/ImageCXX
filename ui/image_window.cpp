#include "image_window.h"
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace imagecxx {
void keepWindowOnScreen(QWidget *window)
{
    if (!window->screen() || window->isMaximized() || window->isFullScreen()) return;
    const QRect available = window->screen()->availableGeometry();
    const QSize borders = window->frameGeometry().size() - window->size();
    window->resize(window->size().boundedTo(available.size() - borders));
    const QRect frame = window->frameGeometry();
    const QPoint topLeft(
        std::clamp(frame.x(), available.left(), std::max(available.left(), available.right() - frame.width() + 1)),
        std::clamp(frame.y(), available.top(), std::max(available.top(), available.bottom() - frame.height() + 1)));
    window->move(window->pos() + topLeft - frame.topLeft());
}

// Establish the separate window before constructing its RHI viewer. Reparenting
// a viewer afterward can leave the old backing store registered for composition.
ImageWindow::ImageWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setAcceptDrops(true);
    setMinimumSize(240, 180);
    auto *contents = new QVBoxLayout(this);
    contents->setContentsMargins(0, 0, 0, 0);
    contents->setSpacing(0);
    viewer = new ImageViewer(this);
    contents->addWidget(viewer, 1);
}

void ImageWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    keepWindowOnScreen(this);
    // Native frame margins can arrive after the initial show event.
    QTimer::singleShot(0, this, [this] { keepWindowOnScreen(this); });
}

void ImageWindow::resizeToImage(double pixelAspectRatio)
{
    if (pixelAspectRatio == 0) {
        const auto &axes = viewer->image().metadata().axes;
        pixelAspectRatio = axes[0].unit == axes[1].unit ? axes[1].spacing / axes[0].spacing : 1;
    }
    const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 900);
    const int maximumWidth = std::max(320, std::min(1000, available.width() * 4 / 5));
    const int maximumHeight = std::max(240, std::min(800, available.height() * 4 / 5));
    layout()->activate();
    const int controlsHeight = height() - viewer->height();
    const double imageWidth = double(viewer->image().width());
    const double imageHeight = double(viewer->image().height()) * pixelAspectRatio;
    const double scale = std::min(
        std::max({1.0, 320.0 / imageWidth, (240.0 - controlsHeight) / imageHeight}),
        std::min(double(maximumWidth) / imageWidth, (maximumHeight - controlsHeight) / imageHeight));
    resize(std::clamp(qRound(imageWidth * scale), 320, maximumWidth),
           std::clamp(qRound(imageHeight * scale) + controlsHeight, 240, maximumHeight));
}
} // namespace imagecxx
