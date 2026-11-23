#include "image_window.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#ifdef Q_OS_LINUX
#include <qpa/qplatformnativeinterface.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace imagecxx {
#ifdef Q_OS_MACOS
void activateMacApplication();
#endif

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

void activateApplicationWindow(QWidget *window, const QString &activationToken, const QString &startupId)
{
    if (!window) return;
#ifdef Q_OS_LINUX
    const QString platform = QGuiApplication::platformName();
    bool appliedWaylandToken = false;
    if (platform.startsWith(QStringLiteral("wayland"))) {
        const QString token = !activationToken.isEmpty() ? activationToken : startupId;
        if (!token.isEmpty()) {
            qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
            appliedWaylandToken = true;
        }
    } else if (platform == QStringLiteral("xcb")) {
        if (auto *native = QGuiApplication::platformNativeInterface()) {
            if (!startupId.isEmpty()) {
                if (auto setStartupId = native->nativeResourceFunctionForIntegration(QByteArrayLiteral("setstartupid"))) {
                    const QByteArray bytes = startupId.toUtf8();
                    reinterpret_cast<void (*)(const char *)>(reinterpret_cast<QFunctionPointer>(setStartupId))(bytes.constData());
                }
            }
            if (auto *screen = window->screen() ? window->screen() : QGuiApplication::primaryScreen()) {
                quint32 timestamp = 0;
                const qsizetype marker = startupId.lastIndexOf(QStringLiteral("_TIME"));
                if (marker >= 0) {
                    bool ok = false;
                    const quint32 parsed = startupId.mid(marker + 5).toUInt(&ok);
                    if (ok && parsed != 0) timestamp = parsed;
                }
                if (timestamp == 0)
                    timestamp = static_cast<quint32>(reinterpret_cast<quintptr>(
                        native->nativeResourceForScreen(QByteArrayLiteral("gettimestamp"), screen)));
                if (timestamp != 0) {
                    if (auto setAppTime = native->nativeResourceFunctionForScreen(QByteArrayLiteral("setapptime")))
                        reinterpret_cast<void (*)(QScreen *, quint32)>(reinterpret_cast<QFunctionPointer>(setAppTime))(screen, timestamp);
                    if (auto setAppUserTime = native->nativeResourceFunctionForScreen(QByteArrayLiteral("setappusertime")))
                        reinterpret_cast<void (*)(QScreen *, quint32)>(reinterpret_cast<QFunctionPointer>(setAppUserTime))(screen, timestamp);
                }
            }
        }
    }
#elif defined(Q_OS_MACOS)
    Q_UNUSED(activationToken);
    Q_UNUSED(startupId);
    activateMacApplication();
#else
    Q_UNUSED(activationToken);
    Q_UNUSED(startupId);
#endif
    if (window->isMinimized()) window->showNormal();
    else if (!window->isVisible()) window->show();
    window->raise();
    window->activateWindow();
#ifdef Q_OS_LINUX
    if (appliedWaylandToken) qunsetenv("XDG_ACTIVATION_TOKEN");
#endif
}

std::array<std::size_t, 3> orthogonalAxes(bool orthogonal, int index)
{
    if (orthogonal) {
        if (index == 1) return {0, 2, 1};
        if (index == 2) return {1, 2, 0};
    }
    return {0, 1, 2};
}

SliceControls addSliceControls(ImageWindow *window)
{
    window->setStyleSheet(R"(
        QWidget { background: #F7F4EE; color: #18191B; font-size: 12px; }
        QLabel#sliceAxisLabel { font-weight: 600; color: #4A4640; padding: 1px 6px; background: #EAE4D9; border-radius: 4px; }
        QLabel#slicePosition { color: #545049; font-weight: 500; min-width: 36px; }
        QSlider::groove:horizontal { height: 4px; background: #DED8CD; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #2E69B8; border-radius: 2px; }
        QSlider::sub-page:horizontal:disabled { background: #B8B1A6; }
        QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px;
            background: #FDFBF7; border: 1px solid #8A8275; }
        QSlider::handle:horizontal:hover { background: #FFFFFF; border-color: #2E69B8; }
        QSlider::handle:horizontal:disabled { background: #EFEAE0; border-color: #C5BEB2; }
    )");
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(14, 9, 14, 9);
    bar->setSpacing(10);
    SliceControls controls{};
    controls.axis = new QLabel(window);
    controls.axis->setObjectName("sliceAxisLabel");
    controls.slice = new QSlider(Qt::Horizontal, window);
    controls.slice->setObjectName("sliceSlider");
    controls.axis->setBuddy(controls.slice);
    controls.position = new QLabel(window);
    controls.position->setObjectName("slicePosition");
    controls.position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(controls.axis);
    bar->addWidget(controls.slice, 1);
    bar->addWidget(controls.position);
    static_cast<QVBoxLayout *>(window->layout())->addLayout(bar);
    return controls;
}

void updateSliceControls(const Image &image, bool orthogonal, const QList<ImageViewer *> &views,
                         const QList<SliceControls> &controls, std::array<std::size_t, 3> spatial,
                         std::size_t channel, std::size_t time, const QString &baseTitle)
{
    const QStringList names{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
    for (int i = 0; i < views.size(); ++i) {
        const auto axes = orthogonalAxes(orthogonal, i);
        views[i]->setSlice(spatial[axes[2]], channel, time);
        views[i]->setNavigationCrosshair(orthogonal
            ? std::optional<QPointF>{{double(spatial[axes[0]]) + .5, double(spatial[axes[1]]) + .5}} : std::nullopt);
        const QString label = orthogonal
            ? QCoreApplication::translate("DocumentWidget", "%1%2  ·  %3 %4/%5")
                  .arg(names[int(axes[0])], names[int(axes[1])], names[int(axes[2])])
                  .arg(spatial[axes[2]] + 1).arg(image.dimension(axes[2]))
            : QString{};
        views[i]->setViewLabel(orthogonal ? label : QString{});
        if (i > 0) {
            views[i]->window()->setProperty("viewLabel", label);
            views[i]->window()->setWindowTitle(baseTitle + " · " + label);
        }
        const auto &bar = controls[i];
        const auto length = image.dimension(axes[2]);
        const auto position = spatial[axes[2]];
        const auto axis = names[int(axes[2])];
        const QSignalBlocker blocker(bar.slice);
        const auto maximum = std::min<std::size_t>(length - 1, std::numeric_limits<int>::max());
        bar.slice->setRange(0, int(maximum));
        bar.slice->setValue(int(std::min(position, maximum)));
        bar.slice->setEnabled(length > 1);
        bar.slice->setAccessibleName(QCoreApplication::translate("DocumentWidget", "%1 slice").arg(axis));
        bar.axis->setText(axis);
        bar.position->setText(QCoreApplication::translate("DocumentWidget", "%1/%2").arg(position + 1).arg(length));
    }
}

void synchronizeOrthogonalTransform(const Image &image, bool orthogonal, const QList<ImageViewer *> &views,
                                    ImageViewer *source, int index, std::array<double, 3> &center)
{
    const auto axes = orthogonalAxes(orthogonal, index);
    center[axes[0]] = source->viewCenter().x();
    center[axes[1]] = source->viewCenter().y();
    if (source->fitsWindow()) {
        for (auto *view : views) if (view != source) view->fitToWindow();
        center = {double(image.width()) / 2, double(image.height()) / 2, double(image.depth()) / 2};
        return;
    }
    const auto spacing = [&image, orthogonal](std::size_t axis) {
        const auto &metadata = image.metadata().axes;
        if (!orthogonal || metadata.size() < 3 || metadata[axis].unit != metadata[0].unit) return 1.0;
        return metadata[axis].spacing / metadata[0].spacing;
    };
    // Clamp one physical scale for the group before updating any window.
    double minimum = ImageViewer::minimumZoom, maximum = ImageViewer::maximumZoom;
    for (int i = 0; i < views.size(); ++i) {
        const auto factor = spacing(orthogonalAxes(orthogonal, i)[0]);
        minimum = std::max(minimum, ImageViewer::minimumZoom / factor);
        maximum = std::min(maximum, ImageViewer::maximumZoom / factor);
    }
    const auto scale = std::max(minimum, std::min(source->zoom() / spacing(axes[0]), maximum));
    for (int i = 0; i < views.size(); ++i) {
        const auto target = orthogonalAxes(orthogonal, i);
        const auto zoom = scale * spacing(target[0]);
        if (views[i] != source || zoom != source->zoom())
            views[i]->setViewTransform(zoom, {center[target[0]], center[target[1]]});
    }
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

#ifdef Q_OS_WIN
bool ImageWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG" && message) {
        const auto *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_SIZE) {
            if (msg->wParam == SIZE_MINIMIZED) {
                lastNativeClientSize_ = {};
            } else if (msg->wParam == SIZE_RESTORED || msg->wParam == SIZE_MAXIMIZED) {
                const QSize clientSize(int(LOWORD(msg->lParam)), int(HIWORD(msg->lParam)));
                if (clientSize.isEmpty()) {
                    lastNativeClientSize_ = {};
                } else {
                    // Without CS_HREDRAW|CS_VREDRAW, Win32 skips WM_PAINT when shrinking, so Qt defers
                    // the resize flush behind a modal-loop timer instead of painting in lockstep like growth.
                    if (isVisible() && msg->hwnd && !lastNativeClientSize_.isEmpty()
                        && clientSize != lastNativeClientSize_
                        && clientSize.width() <= lastNativeClientSize_.width()
                        && clientSize.height() <= lastNativeClientSize_.height()) {
                        InvalidateRect(msg->hwnd, nullptr, FALSE);
                    }
                    lastNativeClientSize_ = clientSize;
                }
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}
#endif

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
