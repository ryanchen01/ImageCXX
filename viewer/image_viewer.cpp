#include "viewer/image_viewer.h"
#include "viewer/viewer_internal.h"

#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <numbers>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace imagecxx {
using detail::CanvasLayer;
using detail::DecodedSlice;
using detail::SliceCanvas;
using detail::SliceKey;
using detail::decodeSlice;
using detail::renderSliceImage;

namespace {

std::vector<QPointF> roiHandles(const Roi &roi)
{
    if (roi.type == RoiType::Composite) return {};
    if (roi.type == RoiType::Line || roi.type == RoiType::Polygon) return roi.points;
    const auto bounds = roi.bounds();
    const auto center = bounds.center();
    return {bounds.topLeft(), {center.x(), bounds.top()}, bounds.topRight(), {bounds.right(), center.y()},
        bounds.bottomRight(), {center.x(), bounds.bottom()}, bounds.bottomLeft(), {bounds.left(), center.y()}};
}

bool validRoi(const Roi &roi)
{
    if (roi.type == RoiType::Composite) return !roi.region.isEmpty();
    if (roi.points.size() < 2) return false;
    if (roi.type == RoiType::Line) return QLineF(roi.points[0], roi.points[1]).length() >= 0.5;
    if (roi.type != RoiType::Polygon) return roi.bounds().width() >= 0.5 && roi.bounds().height() >= 0.5;
    double area = 0;
    for (std::size_t i = 0; i < roi.points.size(); ++i) {
        const auto &a = roi.points[i];
        const auto &b = roi.points[(i + 1) % roi.points.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return roi.points.size() >= 3 && std::abs(area) >= 0.5;
}

QPointF constrainedPoint(QPointF anchor, QPointF point, RoiType type, QSizeF imageSize, bool shift)
{
    point.setX(std::clamp(point.x(), 0.0, imageSize.width()));
    point.setY(std::clamp(point.y(), 0.0, imageSize.height()));
    if (!shift) return point;
    QPointF delta = point - anchor;
    if (type == RoiType::Rectangle || type == RoiType::Ellipse) {
        const double side = std::max(std::abs(delta.x()), std::abs(delta.y()));
        delta = {std::copysign(side, delta.x()), std::copysign(side, delta.y())};
    } else {
        constexpr double step = std::numbers::pi / 4;
        const double angle = std::round(std::atan2(delta.y(), delta.x()) / step) * step;
        const double length = std::hypot(delta.x(), delta.y());
        const double dx = std::cos(angle), dy = std::sin(angle);
        delta = {length * (std::abs(dx) < 1e-12 ? 0 : dx), length * (std::abs(dy) < 1e-12 ? 0 : dy)};
    }
    // Shorten the whole vector at image edges to preserve the constraint.
    double scale = 1;
    if (delta.x() > 0) scale = std::min(scale, (imageSize.width() - anchor.x()) / delta.x());
    else if (delta.x() < 0) scale = std::min(scale, -anchor.x() / delta.x());
    if (delta.y() > 0) scale = std::min(scale, (imageSize.height() - anchor.y()) / delta.y());
    else if (delta.y() < 0) scale = std::min(scale, -anchor.y() / delta.y());
    return anchor + delta * scale;
}

class ViewerOverlay final : public QWidget {
public:
    explicit ViewerOverlay(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }
    std::function<void(QPainter &)> paint;
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (paint) paint(painter);
    }
};

} // namespace

class ImageViewer::Private {
public:
    struct Request {
        SliceKey key;
        Image image;
        std::uint64_t serial = 0;
        int zDirection = 1;
        bool prefetchOnly = false;
    };
    QList<OverlayLayer> layers;
    QList<QMetaObject::Connection> layerConnections;
    QList<QPointer<ImageViewer>> connectedLayerViewers;
    SliceCanvas *canvas = nullptr;
    ViewerOverlay *overlay = nullptr;
    Image image, mask;
    std::size_t z = 0, channel = 0, time = 0;
    int zDirection = 1;
    std::uint64_t imageRevision = 0;
    double zoom = 1, pixelAspect = 1;
    QPointF pan;
    QPointF mouseImage{-1, -1}, lastMouse;
    std::optional<QPointF> navigationCrosshair;
    QString viewLabel;
    bool pickingPosition = false;
    Tool tool = Tool::Pointer;
    std::vector<Roi> rois;
    std::vector<Annotation> annotations;
    std::optional<Roi> draft, selection, dragOriginal;
    bool editing = false;
    int dragHandle = -1;
    QPointF dragStart;
    std::optional<Roi> combineOriginal;
    bool subtract = false;
    bool panning = false, drawing = false, fitted = true, spacePan = false;
    bool crosshair = false, scaleBar = false, composite = false, autoRangePending = true;
    bool loading = false;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    std::deque<std::shared_ptr<const DecodedSlice>> cache;
    bool cacheActive = true;
    std::size_t cacheBytes = 0, decodedBudget = 128 * 1024 * 1024;
    std::atomic<std::uint64_t> serial{0};
    std::jthread worker;

    std::shared_ptr<const DecodedSlice> enqueueSlice(ImageViewer *owner, std::uint64_t serialValue)
    {
        const SliceKey key{imageRevision, z, channel, time, composite};
        std::shared_ptr<const DecodedSlice> cached;
        bool notifyWorker = true;
        {
            std::lock_guard lock(mutex);
            const auto found = std::find_if(cache.begin(), cache.end(),
                [&key](const auto &entry) { return entry->key == key; });
            if (found != cache.end()) {
                cached = *found;
                cache.erase(found);
                cache.push_front(cached);
                if (cacheActive && image.depth() > 1) {
                    pending = Request{key, image, serialValue, zDirection, true};
                } else {
                    pending.reset();
                    notifyWorker = false;
                }
            } else {
                pending = Request{key, image, serialValue, zDirection, false};
            }
        }
        if (cached) {
            canvas->frame = cached;
            if (notifyWorker) condition.notify_one();
            return cached;
        }
        loading = true;
        condition.notify_one();
        owner->updateCanvas();
        Q_EMIT owner->loadingChanged(true);
        return {};
    }

    void finishDecoded(ImageViewer *owner, const std::shared_ptr<const DecodedSlice> &decoded)
    {
        const bool wasLoading = loading;
        canvas->frame = decoded;
        loading = false;
        if (autoRangePending) {
            autoRangePending = false;
            // RGB8 textures are normalized by the sampler.
            const double divisor = decoded->mode == 1 ? 255.0 : 1.0;
            canvas->minimum = decoded->minimum / divisor;
            canvas->maximum = decoded->maximum / divisor;
            Q_EMIT owner->displayRangeChanged(decoded->minimum, decoded->maximum);
            Q_EMIT owner->displaySettingsChanged();
        }
        owner->updateCanvas();
        if (wasLoading) Q_EMIT owner->loadingChanged(false);
        Q_EMIT owner->sliceReady();
    }
};

ImageViewer::ImageViewer(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>())
{
    setMinimumSize(160, 120);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(toolCursor());
    d->canvas = new SliceCanvas(this);
    d->overlay = new ViewerOverlay(this);
    d->canvas->failed = [this](const QString &message) { Q_EMIT loadFailed(message); };
    d->overlay->paint = [this](QPainter &painter) { paintOverlays(painter); };

    // One worker owns decode scheduling. A new serial interrupts decode between rows,
    // and the GUI also checks the serial before publishing a result.
    d->worker = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            Private::Request request;
            {
                std::unique_lock lock(d->mutex);
                d->condition.wait(lock, [this, &stop] { return stop.stop_requested() || d->pending.has_value(); });
                if (stop.stop_requested()) break;
                request = std::move(*d->pending);
                d->pending.reset();
            }
            const std::size_t depth = request.image.depth();
            const auto neighbor = [z = request.key.z, depth](int delta) {
                return delta > 0 ? (z + 1 < depth ? z + 1 : z) : (z > 0 ? z - 1 : z);
            };
            const std::array<std::size_t, 3> slices{request.key.z,
                neighbor(request.zDirection), neighbor(-request.zDirection)};
            for (std::size_t index = request.prefetchOnly ? 1 : 0; index < slices.size(); ++index) {
                if (stop.stop_requested() || request.serial != d->serial.load()) break;
                if (index) {
                    std::lock_guard lock(d->mutex);
                    if (!d->cacheActive) break;
                    if (index == 2 && slices[1] != request.key.z && !d->cache.empty()
                        && std::size_t(d->cache.front()->pixels.capacity()) * 2 > d->decodedBudget)
                        break;
                }
                SliceKey key = request.key;
                key.z = slices[index];
                if (index && key.z == request.key.z) continue;
                std::shared_ptr<const DecodedSlice> decoded;
                {
                    std::lock_guard lock(d->mutex);
                    const auto found = std::find_if(d->cache.begin(), d->cache.end(),
                        [&key](const auto &entry) { return entry->key == key; });
                    if (found != d->cache.end()) {
                        decoded = *found;
                        d->cache.erase(found);
                        d->cache.push_front(decoded);
                    }
                }
                try {
                    if (!decoded) {
                        std::size_t budget;
                        {
                            std::lock_guard lock(d->mutex);
                            budget = d->decodedBudget;
                        }
                        decoded = decodeSlice(request.image, key, budget, [&stop, &request, this] {
                            return stop.stop_requested() || request.serial != d->serial.load();
                        });
                        if (!decoded) break;
                        {
                            std::lock_guard lock(d->mutex);
                            const auto retainedBytes = std::size_t(decoded->pixels.capacity());
                            while (!d->cache.empty() && d->cacheBytes + retainedBytes > d->decodedBudget) {
                                d->cacheBytes -= std::size_t(d->cache.back()->pixels.capacity());
                                d->cache.pop_back();
                            }
                            if (d->cacheActive && retainedBytes <= d->decodedBudget) {
                                d->cache.push_front(decoded);
                                d->cacheBytes += retainedBytes;
                            }
                        }
                    }
                    if (index == 0) {
                        QMetaObject::invokeMethod(this, [this, decoded, serial = request.serial] {
                            if (serial != d->serial.load()) return;
                            d->finishDecoded(this, decoded);
                        }, Qt::QueuedConnection);
                    }
                } catch (const std::exception &error) {
                    if (index == 0) {
                        QMetaObject::invokeMethod(this, [this, message = QString::fromUtf8(error.what()), serial = request.serial] {
                            if (serial != d->serial.load()) return;
                            d->loading = false;
                            d->canvas->frame.reset();
                            updateCanvas();
                            Q_EMIT loadingChanged(false);
                            Q_EMIT loadFailed(message);
                        }, Qt::QueuedConnection);
                    }
                    break;
                }
            }
        }
    });
}

void ImageViewer::paintOverlays(QPainter &painter, bool printing)
{
    if (d->image.empty()) {
        painter.setPen(QColor("#545049"));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open an image to begin"));
        return;
    }
    const double zoom = printing ? 1 : d->zoom;
    const auto origin = printing ? QPointF{} : imageToWidget({0, 0});
    const QTransform transform(zoom, 0, 0, printing ? 1 : zoom * d->pixelAspect, origin.x(), origin.y());
    const auto mapPoint = [&transform](QPointF point) { return transform.map(point); };
    const auto drawRoi = [this, &painter, &transform, &mapPoint](const Roi &roi, bool draft) {
        if (!roi.visible || roi.time != d->time
            || (!d->composite && roi.channel != d->channel) || roi.points.size() < 2)
            return;
        painter.save();
        const QColor color = draft ? QColor(255, 213, 115) : QColor::fromRgba(roi.color);
        painter.setPen(QPen(color, 1.6, draft ? Qt::DashLine : Qt::SolidLine));
        painter.setBrush(Qt::NoBrush);
        const QRectF bounds(mapPoint(roi.bounds().topLeft()), mapPoint(roi.bounds().bottomRight()));
        if (draft && roi.type == RoiType::Polygon) {
            QPolygonF points;
            for (QPointF point : roi.points) points << mapPoint(point);
            painter.drawPolyline(points);
        } else painter.drawPath(transform.map(roi.path()));
        if (!roi.name.isEmpty()) {
            const QPointF label = bounds.topLeft() + QPointF(3, -5);
            painter.setPen(QPen(QColor(8, 12, 17), 3));
            QPainterPath path;
            path.addText(label, painter.font(), roi.name);
            painter.drawPath(path);
            painter.fillPath(path, color);
        }
        painter.restore();
    };
    for (const auto &annotation : d->annotations) {
        if (annotation.geometry.z != d->z) continue;
        auto outline = annotation.geometry; outline.name = annotation.text;
        drawRoi(outline, false);
    }
    for (const Roi &roi : d->rois)
        if (!d->selection || roi.id != d->selection->id) drawRoi(roi, false);
    if (d->selection) {
        drawRoi(*d->selection, false);
        if (!printing && d->editing && d->selection->visible && d->selection->time == d->time
            && (d->composite || d->selection->channel == d->channel)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 213, 115));
            for (const auto &handle : roiHandles(*d->selection))
                painter.drawRect(QRectF(mapPoint(handle) - QPointF(3, 3), QSizeF(6, 6)));
        }
    }
    if (!printing && d->draft) drawRoi(*d->draft, true);
    const QRectF viewport = printing ? QRectF(mapPoint({0, 0}), mapPoint(
        {double(d->image.width()), double(d->image.height())})) : QRectF(rect());
    if (d->crosshair && d->mouseImage.x() >= 0) {
        const QPointF point = mapPoint(d->mouseImage);
        painter.setPen(QPen(QColor(240, 245, 255, 150), 1, Qt::DashLine));
        painter.drawLine(QPointF(point.x(), viewport.top()), QPointF(point.x(), viewport.bottom()));
        painter.drawLine(QPointF(viewport.left(), point.y()), QPointF(viewport.right(), point.y()));
    }
    if (d->navigationCrosshair) {
        const auto point = mapPoint(*d->navigationCrosshair);
        const auto bounds = QRectF(mapPoint({0, 0}), mapPoint(
            {double(d->image.width()), double(d->image.height())}));
        painter.save();
        painter.setClipRect(bounds);
        painter.setPen(QPen(QColor(70, 220, 235, 210), 1));
        painter.drawLine(QPointF(point.x(), bounds.top()), QPointF(point.x(), bounds.bottom()));
        painter.setPen(QPen(QColor(255, 213, 115, 210), 1));
        painter.drawLine(QPointF(bounds.left(), point.y()), QPointF(bounds.right(), point.y()));
        painter.restore();
    }
    if (d->scaleBar && viewport.width() > 180 && viewport.height() > 100) {
        const auto &axes = d->image.metadata().axes;
        const double spacing = axes.empty() ? 1.0 : axes[0].spacing;
        const QString unit = axes.empty() ? QStringLiteral("px") : axes[0].unit;
        const double target = 110.0 * spacing / zoom;
        if (spacing > 0 && std::isfinite(target) && target > 0) {
            const double power = std::pow(10.0, std::floor(std::log10(target)));
            const double fraction = target / power;
            const double length = (fraction >= 5 ? 5 : fraction >= 2 ? 2 : 1) * power;
            const double pixels = length / spacing * zoom;
            const QPointF end(viewport.right() - 24.0, viewport.bottom() - 26.0);
            painter.setPen(QPen(QColor(0, 0, 0, 170), 6));
            painter.drawLine(end - QPointF(pixels, 0), end);
            painter.setPen(QPen(Qt::white, 2));
            painter.drawLine(end - QPointF(pixels, 0), end);
            const QRectF textRect(end.x() - pixels - 25, end.y() - 24, pixels + 50, 19);
            painter.fillRect(textRect.adjusted(10, 0, -10, 0), QColor(0, 0, 0, 160));
            QPainterPath label;
            label.addText(QPointF{}, painter.font(), QStringLiteral("%1 %2").arg(length, 0, 'g', 4).arg(unit));
            const auto offset = textRect.center() - label.boundingRect().center();
            painter.fillPath(label.translated(offset), Qt::white);
        }
    }
    if (!d->viewLabel.isEmpty()) {
        const QRectF label(10, 10, painter.fontMetrics().horizontalAdvance(d->viewLabel) + 20, 26);
        painter.save();
        painter.setPen(QPen(QColor(255, 255, 255, 38), 1));
        painter.setBrush(QColor(18, 22, 28, 215));
        painter.drawRoundedRect(label, 5, 5);
        painter.setPen(QColor(238, 242, 248));
        painter.drawText(label, Qt::AlignCenter, d->viewLabel);
        painter.restore();
    }
}

ImageViewer::~ImageViewer()
{
    d->worker.request_stop();
    d->condition.notify_all();
    d->worker.join();
    // Child callbacks reference Private, so destroy them before Private.
    delete d->overlay;
    delete d->canvas;
}

void ImageViewer::setImage(const Image &image, bool preserveInversion)
{
    if (!preserveInversion || d->image.empty()) {
        const auto photometric = image.metadata().attributes.value(QStringLiteral("DICOM.0028,0004"));
        d->canvas->inverted = photometric.trimmed().compare(QStringLiteral("MONOCHROME1"), Qt::CaseInsensitive) == 0;
    }
    d->image = image;
    ++d->imageRevision;
    d->z = d->channel = d->time = 0;
    d->zDirection = 1;
    d->autoRangePending = true;
    d->mask = {};
    d->canvas->mask.clear();
    d->canvas->frame.reset();
    d->canvas->metadata = image.metadata();
    d->draft.reset(); d->combineOriginal.reset();
    d->selection.reset(); d->dragOriginal.reset(); d->editing = d->drawing = false;
    {
        std::lock_guard lock(d->mutex);
        d->cache.clear();
        d->cacheBytes = 0;
    }
    fitToWindow();
    requestSlice();
    Q_EMIT sliceChanged(0, 0, 0);
}

const Image &ImageViewer::image() const { return d->image; }
std::size_t ImageViewer::slice() const { return d->z; }
std::size_t ImageViewer::channel() const { return d->channel; }
std::size_t ImageViewer::time() const { return d->time; }

void ImageViewer::setSlice(std::size_t z, std::size_t channel, std::size_t time)
{
    if (d->image.empty()) return;
    z = std::min(z, d->image.depth() - 1);
    channel = std::min(channel, d->image.channels() - 1);
    time = std::min(time, d->image.frames() - 1);
    if (d->z == z && d->channel == channel && d->time == time) return;
    const bool planeGroupChanged = d->channel != channel || d->time != time;
    if (z != d->z) d->zDirection = z > d->z ? 1 : -1;
    d->z = z; d->channel = channel; d->time = time;
    // A mask created on one plane must not appear on unrelated slices.
    d->mask = {};
    d->canvas->mask.clear();
    d->draft.reset(); d->combineOriginal.reset();
    if (d->dragOriginal) d->selection = d->dragOriginal;
    d->dragOriginal.reset(); d->drawing = false;
    if (planeGroupChanged) d->editing = false;
    const auto serial = ++d->serial;
    const auto cached = d->enqueueSlice(this, serial);
    Q_EMIT sliceChanged(z, channel, time);
    if (cached && serial == d->serial.load())
        d->finishDecoded(this, cached);
}

void ImageViewer::requestSlice()
{
    const auto serial = ++d->serial;
    if (d->image.empty()) {
        d->loading = false;
        {
            std::lock_guard lock(d->mutex);
            d->pending.reset();
        }
        updateCanvas();
        Q_EMIT loadingChanged(false);
        return;
    }
    if (const auto cached = d->enqueueSlice(this, serial))
        d->finishDecoded(this, cached);
}

void ImageViewer::setWindow(double minimum, double maximum)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) return;
    d->autoRangePending = false;
    const double divisor = componentCount(d->image.pixelType()) > 1 ? 255.0 : 1.0;
    if (d->canvas->minimum == minimum / divisor && d->canvas->maximum == maximum / divisor) return;
    d->canvas->minimum = minimum / divisor;
    d->canvas->maximum = maximum / divisor;
    updateCanvas();
    Q_EMIT displayRangeChanged(minimum, maximum);
    Q_EMIT displaySettingsChanged();
}
double ImageViewer::windowMinimum() const
{
    return d->canvas->minimum * (componentCount(d->image.pixelType()) > 1 ? 255.0 : 1.0);
}
double ImageViewer::windowMaximum() const
{
    return d->canvas->maximum * (componentCount(d->image.pixelType()) > 1 ? 255.0 : 1.0);
}
std::pair<double, double> ImageViewer::sliceRange() const
{
    if (d->canvas->frame) return {d->canvas->frame->minimum, d->canvas->frame->maximum};
    return {windowMinimum(), windowMaximum()};
}
void ImageViewer::autoWindow()
{
    d->autoRangePending = true;
    if (d->canvas->frame && !d->loading) {
        d->autoRangePending = false;
        const auto [minimum, maximum] = sliceRange();
        const double divisor = d->canvas->frame->mode == 1 ? 255.0 : 1.0;
        d->canvas->minimum = minimum / divisor;
        d->canvas->maximum = maximum / divisor;
        Q_EMIT displayRangeChanged(minimum, maximum);
        Q_EMIT displaySettingsChanged();
        updateCanvas();
    }
}
void ImageViewer::setGamma(double gamma)
{
    if (!std::isfinite(gamma) || gamma < 0.01 || gamma > 100) return;
    if (d->canvas->gamma == gamma) return;
    d->canvas->gamma = gamma;
    updateCanvas();
    Q_EMIT displaySettingsChanged();
}
double ImageViewer::gamma() const { return d->canvas->gamma; }
void ImageViewer::setInverted(bool inverted)
{
    if (d->canvas->inverted == inverted) return;
    d->canvas->inverted = inverted;
    updateCanvas();
    Q_EMIT displaySettingsChanged();
}
bool ImageViewer::isInverted() const { return d->canvas->inverted; }
void ImageViewer::setLut(const QString &name)
{
    const int lut = name.compare(QStringLiteral("hot"), Qt::CaseInsensitive) == 0 ? 1
        : name.compare(QStringLiteral("viridis"), Qt::CaseInsensitive) == 0 ? 2 : 0;
    if (d->canvas->lut == lut) return;
    d->canvas->lut = lut;
    updateCanvas();
    Q_EMIT displaySettingsChanged();
}
QString ImageViewer::lut() const
{
    return d->canvas->lut == 1 ? QStringLiteral("hot") : d->canvas->lut == 2 ? QStringLiteral("viridis") : QStringLiteral("grayscale");
}
void ImageViewer::setLinearInterpolation(bool enabled)
{
    if (d->canvas->linear == enabled) return;
    d->canvas->linear = enabled;
    updateCanvas();
    Q_EMIT displaySettingsChanged();
}
bool ImageViewer::linearInterpolation() const { return d->canvas->linear; }
void ImageViewer::setComposite(bool enabled)
{
    if (d->composite == enabled) return;
    d->composite = enabled;
    requestSlice();
    Q_EMIT displaySettingsChanged();
}
bool ImageViewer::composite() const { return d->composite; }

QImage ImageViewer::renderedImage()
{
    QImage result = renderSliceImage(d->image, d->z, d->channel, d->time, windowMinimum(), windowMaximum(),
        gamma(), d->canvas->lut, isInverted(), d->composite, d->mask);
    if (result.isNull()) return {};
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(font());
    paintOverlays(painter, true);
    return result;
}

void ImageViewer::setOverlayLayers(const QList<OverlayLayer> &layers)
{
    d->layers = layers;
    d->canvas->overlayMode = true;
    QList<QPointer<ImageViewer>> viewers;
    for (const auto &layer : layers)
        if (layer.viewer && layer.viewer != this && !viewers.contains(layer.viewer))
            viewers.append(layer.viewer);
    if (viewers != d->connectedLayerViewers) {
        for (const auto &connection : d->layerConnections) disconnect(connection);
        d->layerConnections.clear();
        d->connectedLayerViewers = viewers;
        for (const auto &viewer : viewers) {
            d->layerConnections.append(connect(viewer, &ImageViewer::sliceReady, this, &ImageViewer::updateCanvas));
            d->layerConnections.append(connect(viewer, &ImageViewer::displaySettingsChanged, this, &ImageViewer::updateCanvas));
            d->layerConnections.append(connect(viewer, &ImageViewer::loadFailed, this, [this] { updateCanvas(); }));
            d->layerConnections.append(connect(viewer, &QObject::destroyed, this, [this, dying = viewer.data()] {
                for (auto &entry : d->layers) if (entry.viewer == dying) entry.viewer.clear();
                updateCanvas();
            }));
        }
    }
    updateCanvas();
}

void ImageViewer::applyFitTransform()
{
    d->pan = {};
    if (!d->image.empty())
        d->zoom = std::max(minimumZoom, std::min(double(width()) / double(d->image.width()),
                                        double(height()) / (double(d->image.height()) * d->pixelAspect)));
}

void ImageViewer::fitToWindow()
{
    d->fitted = true;
    applyFitTransform();
    updateCanvas();
    Q_EMIT zoomChanged(d->zoom);
    Q_EMIT viewTransformChanged();
}
void ImageViewer::setZoom(double scale)
{
    if (!std::isfinite(scale) || scale <= 0) return;
    d->fitted = false;
    d->zoom = std::clamp(scale, minimumZoom, maximumZoom);
    updateCanvas();
    Q_EMIT zoomChanged(d->zoom);
    Q_EMIT viewTransformChanged();
}
double ImageViewer::zoom() const { return d->zoom; }
bool ImageViewer::fitsWindow() const { return d->fitted; }
QPointF ImageViewer::viewCenter() const { return widgetToImage(QPointF(width(), height()) / 2); }
void ImageViewer::setViewTransform(double scale, QPointF center)
{
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(center.x()) || !std::isfinite(center.y())) return;
    d->fitted = false;
    d->zoom = std::clamp(scale, minimumZoom, maximumZoom);
    d->pan = QPointF((double(d->image.width()) / 2 - center.x()) * d->zoom,
        (double(d->image.height()) / 2 - center.y()) * d->zoom * d->pixelAspect);
    updateCanvas();
    Q_EMIT zoomChanged(d->zoom);
    Q_EMIT viewTransformChanged();
}
void ImageViewer::setPixelAspectRatio(double heightOverWidth)
{
    if (!std::isfinite(heightOverWidth) || heightOverWidth <= 0 || d->pixelAspect == heightOverWidth) return;
    d->pixelAspect = heightOverWidth;
    fitToWindow();
}
void ImageViewer::setTool(Tool tool)
{
    d->tool = tool;
    d->draft.reset(); d->combineOriginal.reset();
    d->drawing = false; d->editing = false; d->dragOriginal.reset();
    d->panning = false; d->pickingPosition = false;
    setCursor(toolCursor());
    updateCanvas();
    Q_EMIT toolChanged();
}
Qt::CursorShape ImageViewer::toolCursor() const
{
    return d->spacePan ? Qt::OpenHandCursor : Qt::CrossCursor;
}
ImageViewer::Tool ImageViewer::tool() const { return d->tool; }
void ImageViewer::setAnnotations(const std::vector<Annotation> &annotations)
{
    d->annotations = annotations;
    d->overlay->update();
}

void ImageViewer::setRois(const std::vector<Roi> &rois)
{
    d->rois = rois;
    if (d->selection && !d->dragOriginal) {
        const auto saved = std::find_if(rois.begin(), rois.end(), [this](const Roi &roi) { return roi.id == d->selection->id; });
        if (saved != rois.end()) d->selection = *saved;
    }
    Q_EMIT toolDimensionsChanged(toolDimensions());
    d->overlay->update();
}
void ImageViewer::setSelection(std::optional<Roi> roi)
{
    if (!roi || !d->selection || roi->id != d->selection->id) {
        d->editing = false;
        d->dragOriginal.reset();
    }
    d->selection = std::move(roi);
    Q_EMIT toolDimensionsChanged(toolDimensions());
    d->overlay->update();
}
void ImageViewer::setMask(const Image &mask)
{
    d->mask = mask;
    d->canvas->mask.clear();
    if (!mask.empty() && !d->image.empty() && mask.width() == d->image.width() && mask.height() == d->image.height()) {
        const auto count = mask.width() * mask.height();
        if (count <= d->canvas->gpuBudget) {
            d->canvas->mask.resize(qsizetype(count));
            for (std::size_t y = 0; y < mask.height(); ++y)
                for (std::size_t x = 0; x < mask.width(); ++x)
                    d->canvas->mask[qsizetype(y * mask.width() + x)] = mask.sample(x, y) == 0 ? 0 : char(255);
            d->canvas->maskSize = QSize(int(mask.width()), int(mask.height()));
        }
    }
    updateCanvas();
}
void ImageViewer::setCrosshairVisible(bool visible)
{
    if (d->crosshair == visible) return;
    d->crosshair = visible; d->overlay->update();
    Q_EMIT overlaysChanged(d->crosshair, d->scaleBar);
}
void ImageViewer::setScaleBarVisible(bool visible)
{
    if (d->scaleBar == visible) return;
    d->scaleBar = visible; d->overlay->update();
    Q_EMIT overlaysChanged(d->crosshair, d->scaleBar);
}
void ImageViewer::setNavigationCrosshair(std::optional<QPointF> position)
{
    d->navigationCrosshair = position;
    if (!position) d->pickingPosition = false;
    d->overlay->update();
}
bool ImageViewer::crosshairVisible() const { return d->crosshair; }
bool ImageViewer::scaleBarVisible() const { return d->scaleBar; }
void ImageViewer::setViewLabel(const QString &label) { d->viewLabel = label; d->overlay->update(); }
void ImageViewer::setCacheActive(bool active)
{
    std::lock_guard lock(d->mutex);
    if (d->cacheActive == active) return;
    d->cacheActive = active;
    if (!active) {
        d->cache.clear();
        d->cacheBytes = 0;
    }
}

void ImageViewer::setCacheBudgets(std::size_t decodedBytes, std::size_t gpuBytes)
{
    {
        std::lock_guard lock(d->mutex);
        d->decodedBudget = decodedBytes;
        while (!d->cache.empty() && d->cacheBytes > decodedBytes) {
            d->cacheBytes -= std::size_t(d->cache.back()->pixels.capacity());
            d->cache.pop_back();
        }
    }
    d->canvas->gpuBudget = gpuBytes;
    requestSlice();
}
std::size_t ImageViewer::decodedCacheBytes() const
{
    std::lock_guard lock(d->mutex);
    return d->cacheBytes;
}
bool ImageViewer::hasCachedSlice(std::size_t z, std::size_t channel, std::size_t time) const
{
    const SliceKey key{d->imageRevision, z, channel, time, d->composite};
    std::lock_guard lock(d->mutex);
    return std::any_of(d->cache.begin(), d->cache.end(),
        [&key](const auto &entry) { return entry->key == key; });
}
std::size_t ImageViewer::gpuCacheBytes() const { return d->canvas->gpuBytes; }

QPointF ImageViewer::imageToWidget(QPointF point) const
{
    const QPointF origin = QPointF(width(), height()) / 2.0 + d->pan
        - QPointF(double(d->image.width()), double(d->image.height()) * d->pixelAspect) * (d->zoom / 2.0);
    return origin + QPointF(point.x() * d->zoom, point.y() * d->zoom * d->pixelAspect);
}
QPointF ImageViewer::widgetToImage(QPointF point) const
{
    const auto offset = (point - imageToWidget({0, 0})) / d->zoom;
    return {offset.x(), offset.y() / d->pixelAspect};
}
QString ImageViewer::toolDimensions() const
{
    const auto &roi = d->draft ? d->draft : d->selection;
    if (!roi || !roi->visible || roi->time != d->time
        || (!d->composite && roi->channel != d->channel)) return {};
    const auto &axes = d->image.metadata().axes;
    const auto dimension = [this](double pixels, double calibrated, const QString &unit) {
        auto text = QString::number(pixels, 'g', 6);
        if (!unit.isEmpty() && unit != "px")
            text += tr(" (%1)").arg(calibrated, 0, 'g', 6);
        return text;
    };
    if (roi->type == RoiType::Line && roi->points.size() >= 2) {
        const auto delta = roi->points[1] - roi->points[0];
        const auto length = std::hypot(delta.x(), delta.y());
        double calibrated = length;
        QString unit;
        if (axes.size() >= 2) {
            if (axes[0].unit == axes[1].unit) {
                calibrated = std::hypot(delta.x() * axes[0].spacing, delta.y() * axes[1].spacing);
                unit = axes[0].unit;
            } else if (delta.y() == 0) {
                calibrated = length * axes[0].spacing;
                unit = axes[0].unit;
            } else if (delta.x() == 0) {
                calibrated = length * axes[1].spacing;
                unit = axes[1].unit;
            }
        }
        return tr("L %1").arg(dimension(length, calibrated, unit));
    }
    const auto bounds = roi->bounds();
    return tr("W %1   H %2")
        .arg(dimension(bounds.width(), bounds.width() * (axes.empty() ? 1 : axes[0].spacing),
            axes.empty() ? QString{} : axes[0].unit))
        .arg(dimension(bounds.height(), bounds.height() * (axes.size() < 2 ? 1 : axes[1].spacing),
            axes.size() < 2 ? QString{} : axes[1].unit));
}

void ImageViewer::syncCanvas()
{
    Q_EMIT toolDimensionsChanged(toolDimensions());
    if (!d->image.empty())
        d->canvas->imageRect = QRectF(imageToWidget({0, 0}), QSizeF(double(d->image.width()) * d->zoom,
            double(d->image.height()) * d->zoom * d->pixelAspect));
    if (d->canvas->overlayMode) {
        d->canvas->layers.clear();
        const auto origin = imageToWidget({0, 0});
        const QTransform viewport(d->zoom, 0, 0, d->zoom * d->pixelAspect, origin.x(), origin.y());
        for (const auto &layer : d->layers) {
            if (!layer.viewer || layer.viewer == this || layer.opacity <= 0) continue;
            const auto &source = *layer.viewer->d;
            const auto *canvas = source.canvas;
            if (!canvas->frame) continue;
            CanvasLayer rendered;
            rendered.frame = canvas->frame;
            rendered.transform = QTransform::fromScale(double(source.image.width()), double(source.image.height()))
                * layer.transform * viewport;
            rendered.metadata = source.image.metadata();
            rendered.minimum = canvas->minimum;
            rendered.maximum = canvas->maximum;
            rendered.gamma = canvas->gamma;
            rendered.opacity = std::clamp(layer.opacity, 0.0, 1.0);
            rendered.lut = layer.lut == "hot" ? 1 : layer.lut == "viridis" ? 2 : 0;
            rendered.inverted = canvas->inverted;
            rendered.linear = canvas->linear;
            d->canvas->layers.append(std::move(rendered));
        }
    }
}
void ImageViewer::updateCanvas()
{
    syncCanvas();
    d->canvas->update();
    d->overlay->update();
}
void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (d->fitted) applyFitTransform();
    syncCanvas();
    const QRect bounds = rect();
    d->canvas->setGeometry(bounds);
    d->overlay->setGeometry(bounds);
    d->overlay->raise();
    d->canvas->update();
    d->overlay->update();
    if (d->fitted) {
        Q_EMIT zoomChanged(d->zoom);
        Q_EMIT viewTransformChanged();
    }
}
void ImageViewer::zoomAt(double scale, QPointF position)
{
    const QPointF anchor = widgetToImage(position);
    setZoom(scale);
    d->pan += position - imageToWidget(anchor);
    updateCanvas();
    Q_EMIT viewTransformChanged();
}

bool ImageViewer::event(QEvent *event)
{
    if (event->type() == QEvent::NativeGesture && !d->image.empty()) {
        auto *gesture = static_cast<QNativeGestureEvent *>(event);
        switch (gesture->gestureType()) {
        case Qt::ZoomNativeGesture:
            zoomAt(d->zoom * (1 + gesture->value()), gesture->position());
            event->accept();
            return true;
        case Qt::BeginNativeGesture:
        case Qt::EndNativeGesture:
            event->accept();
            return true;
        default:
            break;
        }
    }
    return QWidget::event(event);
}

void ImageViewer::wheelEvent(QWheelEvent *event)
{
    if (d->image.empty()) return;
    // Scroll begin/end events can have no delta; they must not change slices.
    if (event->pixelDelta().isNull() && event->angleDelta().isNull()) {
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
        const double factor = std::pow(1.2, double(event->angleDelta().y()) / 120.0);
        zoomAt(d->zoom * factor, event->position());
    } else if (d->image.depth() > 1) {
        if (!event->angleDelta().y()) { event->accept(); return; }
        const auto step = event->angleDelta().y() > 0 ? -1 : 1;
        const auto z = std::clamp<qint64>(qint64(d->z) + step, 0, qint64(d->image.depth()) - 1);
        setSlice(std::size_t(z), d->channel, d->time);
    } else {
        zoomAt(d->zoom * std::pow(1.2, double(event->angleDelta().y()) / 120.0), event->position());
    }
    event->accept();
}
std::optional<Roi> ImageViewer::roiAt(QPointF position) const
{
    const auto point = widgetToImage(position);
    const auto origin = imageToWidget({0, 0});
    const QTransform transform(d->zoom, 0, 0, d->zoom * d->pixelAspect, origin.x(), origin.y());
    QPainterPathStroker stroker;
    stroker.setWidth(12);
    const auto hit = [&](const Roi &roi) {
        return roi.visible && roi.time == d->time && (d->composite || roi.channel == d->channel)
            && (roi.contains(point) || stroker.createStroke(transform.map(roi.path())).contains(position));
    };
    if (d->selection && hit(*d->selection)) return d->selection;
    for (auto it = d->rois.rbegin(); it != d->rois.rend(); ++it)
        if (hit(*it)) return *it;
    return {};
}

void ImageViewer::contextMenuEvent(QContextMenuEvent *event)
{
    if (d->image.empty()) { event->ignore(); return; }
    if (d->draft) {
        if (d->tool == Tool::Polygon) completeRoi();
        event->accept();
        return;
    }
    const auto roi = roiAt(event->pos());
    if (!roi) { event->ignore(); return; }
    setSelection(roi);
    Q_EMIT roiSelected(*roi);
    event->accept();
    Q_EMIT roiContextMenuRequested(event->globalPos());
}

void ImageViewer::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (d->image.empty()) return;
    d->lastMouse = event->position();
    if (event->button() == Qt::MiddleButton || (event->button() == Qt::LeftButton && d->spacePan)) {
        d->panning = true;
        d->fitted = false;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    const QPointF point = widgetToImage(event->position());
    const bool onImage = point.x() >= 0 && point.y() >= 0
        && point.x() < double(d->image.width()) && point.y() < double(d->image.height());
    if (d->tool == Tool::Pointer) {
        if (onImage && d->navigationCrosshair) {
            d->pickingPosition = true;
            Q_EMIT positionPicked(point);
        }
        return;
    }
    const auto selectable = [this](const Roi &roi) {
        return roi.visible && roi.time == d->time && (d->composite || roi.channel == d->channel);
    };
    if (!d->draft) {
        if (d->selection && selectable(*d->selection) && d->selection->type == RoiType::Polygon
            && (event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))) {
            auto roi = *d->selection;
            for (std::size_t i = 0; i < roi.points.size(); ++i) {
                if (QLineF(event->position(), imageToWidget(roi.points[i])).length() > 6) continue;
                if (event->modifiers().testFlag(Qt::AltModifier)) {
                    if (roi.points.size() <= 3) return;
                    roi.points.erase(roi.points.begin() + qptrdiff(i));
                } else {
                    // Insert a duplicate and drag it to place the new vertex.
                    roi.points.insert(roi.points.begin() + qptrdiff(i), roi.points[i]);
                }
                if (validRoi(roi)) { d->selection = roi; Q_EMIT roiEdited(roi); }
                if (!event->modifiers().testFlag(Qt::AltModifier)) {
                    d->dragOriginal = roi; d->dragHandle = int(i); d->dragStart = point;
                }
                updateCanvas(); return;
            }
        }
        if (d->editing && d->selection && selectable(*d->selection)) {
            const auto handles = roiHandles(*d->selection);
            for (std::size_t i = 0; i < handles.size(); ++i) {
                const auto offset = event->position() - imageToWidget(handles[i]);
                if (std::abs(offset.x()) <= 6 && std::abs(offset.y()) <= 6) {
                    d->dragHandle = int(i);
                    d->dragOriginal = d->selection;
                    d->dragStart = point;
                    return;
                }
            }
        }
        const bool combine = onImage && d->selection && selectable(*d->selection) && d->selection->type != RoiType::Line
            && d->tool != Tool::Line && (event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier));
        if (combine) {
            d->combineOriginal = d->selection;
            d->subtract = event->modifiers().testFlag(Qt::AltModifier);
        }
        const auto picked = combine ? std::optional<Roi>{} : roiAt(event->position());
        if (picked) {
            d->selection = picked;
            d->editing = true;
            d->dragOriginal = picked;
            d->dragHandle = -1;
            d->dragStart = point;
            Q_EMIT roiSelected(*picked);
            updateCanvas();
            return;
        }
        if (!combine && onImage && d->editing) {
            d->editing = false; d->dragOriginal.reset(); updateCanvas();
            return;
        }
        if (!combine && onImage && d->selection) {
            d->selection.reset();
            Q_EMIT selectionCleared();
            updateCanvas();
            // A drag can start a replacement ROI; a click simply clears it.
            if (d->tool == Tool::Polygon) return;
        }
    }
    if (!onImage) return;
    if (d->tool == Tool::Polygon && d->draft)
        d->draft->points.push_back(constrainedPoint(d->draft->points.back(), point, RoiType::Line,
            QSizeF(double(d->image.width()), double(d->image.height())), event->modifiers().testFlag(Qt::ShiftModifier)));
    else {
        Roi roi;
        roi.z = d->z; roi.channel = d->channel; roi.time = d->time;
        roi.type = d->tool == Tool::Rectangle ? RoiType::Rectangle : d->tool == Tool::Ellipse ? RoiType::Ellipse
            : d->tool == Tool::Line ? RoiType::Line : RoiType::Polygon;
        const QPointF start = roi.type == RoiType::Line && event->modifiers().testFlag(Qt::AltModifier)
            ? QPointF(std::round(point.x()), std::round(point.y())) : point;
        roi.points = d->tool == Tool::Polygon ? std::vector<QPointF>{point} : std::vector<QPointF>{start, start};
        d->draft = std::move(roi);
    }
    d->drawing = d->tool != Tool::Polygon;
    updateCanvas();
}

void ImageViewer::updateRoiDrag(QPointF point, Qt::KeyboardModifiers modifiers)
{
    const QSizeF size(double(d->image.width()), double(d->image.height()));
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    if (d->drawing && d->draft) {
        if (d->draft->type == RoiType::Line && modifiers.testFlag(Qt::AltModifier)) point = {std::round(point.x()), std::round(point.y())};
        d->draft->points[1] = constrainedPoint(d->draft->points[0], point, d->draft->type, size, shift && !d->combineOriginal);
    } else if (d->dragOriginal && d->selection) {
        const auto &original = *d->dragOriginal;
        Roi roi = original;
        if (d->dragHandle < 0) {
            auto delta = point - d->dragStart;
            if (shift) delta = constrainedPoint(d->dragStart, point, RoiType::Line, size, true) - d->dragStart;
            const auto bounds = original.bounds();
            delta.setX(std::clamp(delta.x(), -bounds.left(), size.width() - bounds.right()));
            delta.setY(std::clamp(delta.y(), -bounds.top(), size.height() - bounds.bottom()));
            for (auto &vertex : roi.points) vertex += delta;
            if (roi.type == RoiType::Composite) roi.region.translate(delta);
        } else if (roi.type == RoiType::Line && (modifiers & (Qt::AltModifier | Qt::ControlModifier))) {
            const auto index = std::size_t(d->dragHandle);
            const auto anchor = modifiers.testFlag(Qt::ControlModifier) ? original.bounds().center() : original.points[1 - index];
            auto end = constrainedPoint(anchor, point, RoiType::Line, size, shift);
            if (modifiers.testFlag(Qt::AltModifier)) {
                QLineF direction(anchor, end);
                if (direction.length() > 0) direction.setLength(QLineF(anchor, original.points[index]).length());
                end = direction.p2();
            }
            if (modifiers.testFlag(Qt::ControlModifier)) {
                const auto center = anchor; auto delta = end - center;
                double scale = 1;
                if (delta.x() != 0) scale = std::min(scale, std::min(center.x(), size.width() - center.x()) / std::abs(delta.x()));
                if (delta.y() != 0) scale = std::min(scale, std::min(center.y(), size.height() - center.y()) / std::abs(delta.y()));
                roi.points[index] = center + delta * scale; roi.points[1 - index] = center - delta * scale;
            } else if (end.x() >= 0 && end.y() >= 0 && end.x() <= size.width() && end.y() <= size.height()) roi.points[index] = end;
        } else if ((roi.type == RoiType::Rectangle || roi.type == RoiType::Ellipse)
                   && (modifiers & (Qt::AltModifier | Qt::ControlModifier))) {
            const auto bounds = original.bounds();
            const bool centered = modifiers.testFlag(Qt::ControlModifier);
            const bool aspect = shift || modifiers.testFlag(Qt::AltModifier);
            const auto handles = roiHandles(original);
            const auto anchor = centered ? bounds.center() : handles[std::size_t((d->dragHandle + 4) % 8)];
            auto delta = point - anchor;
            const bool horizontal = d->dragHandle == 3 || d->dragHandle == 7;
            const bool vertical = d->dragHandle == 1 || d->dragHandle == 5;
            const double ratio = shift ? 1 : bounds.width() / bounds.height();
            double w = std::abs(delta.x()), h = std::abs(delta.y());
            if (horizontal) h = aspect ? w / ratio : bounds.height() / (centered ? 2 : 1);
            else if (vertical) w = aspect ? h * ratio : bounds.width() / (centered ? 2 : 1);
            else if (aspect) { w = std::max(w, h * ratio); h = w / ratio; }
            const auto center = bounds.center();
            const double availableWidth = centered ? std::min(center.x(), size.width() - center.x())
                : vertical ? 2 * std::min(center.x(), size.width() - center.x())
                : delta.x() < 0 ? anchor.x() : size.width() - anchor.x();
            const double availableHeight = centered ? std::min(center.y(), size.height() - center.y())
                : horizontal ? 2 * std::min(center.y(), size.height() - center.y())
                : delta.y() < 0 ? anchor.y() : size.height() - anchor.y();
            if (aspect) {
                const double scale = std::min({1., w > 0 ? availableWidth / w : 1., h > 0 ? availableHeight / h : 1.});
                w *= scale; h *= scale;
            } else { w = std::min(w, availableWidth); h = std::min(h, availableHeight); }
            QPointF first = centered ? center - QPointF(w, h) : anchor;
            QPointF last = centered ? center + QPointF(w, h) : anchor + QPointF(std::copysign(w, delta.x()), std::copysign(h, delta.y()));
            if (!centered && horizontal) { first.setY(center.y() - h / 2); last.setY(center.y() + h / 2); }
            if (!centered && vertical) { first.setX(center.x() - w / 2); last.setX(center.x() + w / 2); }
            roi.points = {first, last};
        } else if (roi.type == RoiType::Line || roi.type == RoiType::Polygon) {
            const auto index = std::size_t(d->dragHandle);
            const auto anchor = roi.points[(index + roi.points.size() - 1) % roi.points.size()];
            roi.points[index] = constrainedPoint(anchor, point, roi.type, size, shift);
        } else {
            const auto handles = roiHandles(original);
            const auto anchor = handles[std::size_t((d->dragHandle + 4) % 8)];
            if (d->dragHandle % 2 == 0) {
                roi.points = {anchor, constrainedPoint(anchor, point, roi.type, size, shift)};
            } else {
                point = constrainedPoint(anchor, point, roi.type, size, false);
                const auto bounds = original.bounds();
                const bool horizontal = d->dragHandle == 3 || d->dragHandle == 7;
                if (horizontal) {
                    double side = std::abs(point.x() - anchor.x());
                    if (shift) side = std::min(side, 2 * std::min(bounds.center().y(), size.height() - bounds.center().y()));
                    const double x = anchor.x() + std::copysign(side, point.x() - anchor.x());
                    const double halfHeight = shift ? side / 2 : bounds.height() / 2;
                    roi.points = {{anchor.x(), bounds.center().y() - halfHeight}, {x, bounds.center().y() + halfHeight}};
                } else {
                    double side = std::abs(point.y() - anchor.y());
                    if (shift) side = std::min(side, 2 * std::min(bounds.center().x(), size.width() - bounds.center().x()));
                    const double y = anchor.y() + std::copysign(side, point.y() - anchor.y());
                    const double halfWidth = shift ? side / 2 : bounds.width() / 2;
                    roi.points = {{bounds.center().x() - halfWidth, anchor.y()}, {bounds.center().x() + halfWidth, y}};
                }
            }
        }
        d->selection = std::move(roi);
    }
    updateCanvas();
}

void ImageViewer::mouseMoveEvent(QMouseEvent *event)
{
    if (d->image.empty()) return;
    QPointF point = widgetToImage(event->position());
    d->mouseImage = point;
    if (d->panning) {
        d->pan += event->position() - d->lastMouse;
        updateCanvas();
        Q_EMIT viewTransformChanged();
    } else if (d->pickingPosition) Q_EMIT positionPicked(point);
    else if (d->drawing || d->dragOriginal) updateRoiDrag(point, event->modifiers());
    d->lastMouse = event->position();
    const int x = int(std::floor(point.x())), y = int(std::floor(point.y()));
    if (x >= 0 && y >= 0 && std::size_t(x) < d->image.width() && std::size_t(y) < d->image.height()) {
        try { Q_EMIT pixelHovered(x, y, d->image.sample(std::size_t(x), std::size_t(y), d->z, d->channel, d->time)); }
        catch (const std::exception &) { /* A lazy provider can fail independently of the current texture. */ }
    }
    if (d->crosshair) d->overlay->update();
}
void ImageViewer::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton) {
        d->panning = false;
        d->pickingPosition = false;
        setCursor(toolCursor());
        if (d->drawing && d->draft && event->button() == Qt::LeftButton) {
            updateRoiDrag(widgetToImage(event->position()), event->modifiers());
            completeRoi();
        } else if (d->dragOriginal && d->selection && event->button() == Qt::LeftButton) {
            updateRoiDrag(widgetToImage(event->position()), event->modifiers());
            if (!validRoi(*d->selection)) d->selection = d->dragOriginal;
            const bool changed = d->selection->points != d->dragOriginal->points;
            const Roi edited = *d->selection;
            d->dragOriginal.reset();
            if (changed) Q_EMIT roiEdited(edited);
            updateCanvas();
        }
    }
}
void ImageViewer::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (d->tool == Tool::Polygon && event->button() == Qt::LeftButton) completeRoi();
}
void ImageViewer::completeRoi()
{
    d->drawing = false;
    if (!d->draft) return;
    Roi roi = *d->draft;
    d->draft.reset();
    roi.points.erase(std::unique(roi.points.begin(), roi.points.end()), roi.points.end());
    if (d->combineOriginal && validRoi(roi)) {
        roi.region = d->subtract ? d->combineOriginal->path().subtracted(roi.path()) : d->combineOriginal->path().united(roi.path());
        roi.type = RoiType::Composite;
        const auto bounds = roi.region.boundingRect(); roi.points = {bounds.topLeft(), bounds.bottomRight()};
    }
    d->combineOriginal.reset();
    if (roi.type == RoiType::Composite && roi.region.isEmpty()) { d->selection.reset(); Q_EMIT selectionCleared(); }
    else if (validRoi(roi)) { d->selection = roi; d->editing = false; Q_EMIT roiCreated(roi); }
    updateCanvas();
}
void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    if (d->image.empty()) { QWidget::keyPressEvent(event); return; }
    if (event->key() == Qt::Key_Escape) {
        d->draft.reset(); d->combineOriginal.reset(); d->drawing = false;
        if (d->dragOriginal) { d->selection = d->dragOriginal; d->dragOriginal.reset(); }
        if (d->editing) d->editing = false;
        else if (d->selection) { d->selection.reset(); Q_EMIT selectionCleared(); }
        updateCanvas();
    } else if ((event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control) && !event->isAutoRepeat()) {
        updateRoiDrag(widgetToImage(d->lastMouse), event->modifiers() | (event->key() == Qt::Key_Shift ? Qt::ShiftModifier : event->key() == Qt::Key_Alt ? Qt::AltModifier : Qt::ControlModifier));
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) completeRoi();
    else if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        d->spacePan = true; setCursor(Qt::OpenHandCursor);
    } else if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal || event->key() == Qt::Key_Up)
        setZoom(d->zoom * 1.25);
    else if (event->key() == Qt::Key_Minus || event->key() == Qt::Key_Down) setZoom(d->zoom / 1.25);
    else if (event->key() == Qt::Key_4) fitToWindow();
    else if (event->key() == Qt::Key_5) setZoom(1);
    else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left
        || event->key() == Qt::Key_Greater || event->key() == Qt::Key_Less
        || event->key() == Qt::Key_Period || event->key() == Qt::Key_Comma) {
        const bool forward = event->key() == Qt::Key_Right || event->key() == Qt::Key_Greater || event->key() == Qt::Key_Period;
        const bool hyperstack = d->image.channels() > 1 || d->image.frames() > 1;
        const bool control = event->modifiers().testFlag(Qt::ControlModifier);
        const bool alt = event->modifiers().testFlag(Qt::AltModifier);
        auto z = d->z, c = d->channel, t = d->time;
        auto &position = hyperstack && alt ? t : hyperstack && !control ? c : z;
        const auto count = hyperstack && alt ? d->image.frames() : hyperstack && !control ? d->image.channels() : d->image.depth();
        const qint64 step = !hyperstack && alt ? 10 : 1;
        position = std::size_t(std::clamp<qint64>(qint64(position) + (forward ? step : -step), 0, qint64(count) - 1));
        setSlice(z, c, t);
    } else QWidget::keyPressEvent(event);
}

void ImageViewer::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        d->spacePan = false;
        setCursor(toolCursor());
        event->accept();
    } else if ((event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Control) && !event->isAutoRepeat()) {
        updateRoiDrag(widgetToImage(d->lastMouse), event->modifiers() & ~(event->key() == Qt::Key_Shift ? Qt::ShiftModifier : event->key() == Qt::Key_Alt ? Qt::AltModifier : Qt::ControlModifier));
        event->accept();
    } else QWidget::keyReleaseEvent(event);
}

void ImageViewer::focusOutEvent(QFocusEvent *event)
{
    d->spacePan = false; d->panning = false; d->pickingPosition = false;
    if (d->dragOriginal) {
        d->selection = d->dragOriginal;
        d->dragOriginal.reset();
        updateCanvas();
    }
    setCursor(toolCursor());
    QWidget::focusOutEvent(event);
}

} // namespace imagecxx
