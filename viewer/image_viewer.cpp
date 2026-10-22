#include "viewer/image_viewer.h"

#include <QFile>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <numbers>
#include <QRhiWidget>
#include <QResizeEvent>
#include <QWheelEvent>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace imagecxx {
namespace {

QPainterPath roiPath(const Roi &roi)
{
    QPainterPath path;
    if (roi.points.size() < 2) return path;
    if (roi.type == RoiType::Rectangle) path.addRect(roi.bounds());
    else if (roi.type == RoiType::Ellipse) path.addEllipse(roi.bounds());
    else {
        path.moveTo(roi.points.front());
        for (std::size_t i = 1; i < roi.points.size(); ++i) path.lineTo(roi.points[i]);
        if (roi.type == RoiType::Polygon) path.closeSubpath();
    }
    return path;
}

std::vector<QPointF> roiHandles(const Roi &roi)
{
    if (roi.type == RoiType::Line || roi.type == RoiType::Polygon) return roi.points;
    const auto bounds = roi.bounds();
    const auto center = bounds.center();
    return {bounds.topLeft(), {center.x(), bounds.top()}, bounds.topRight(), {bounds.right(), center.y()},
        bounds.bottomRight(), {center.x(), bounds.bottom()}, bounds.bottomLeft(), {bounds.left(), center.y()}};
}

bool validRoi(const Roi &roi)
{
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

struct SliceKey {
    std::uint64_t image = 0;
    std::size_t z = 0, channel = 0, time = 0;
    bool composite = false;
    bool operator==(const SliceKey &) const = default;
};

struct DecodedSlice {
    SliceKey key;
    QSize size;
    QByteArray pixels;
    int mode = 0;
    int channels = 1;
    double minimum = 0, maximum = 1;
    // Float64 values use an affine encoding before upload to the float texture.
    double offset = 0, scale = 1;
};

QShader loadShader(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}

// This is the only class that knows about the private Qt RHI API.
class SliceCanvas final : public QRhiWidget {
public:
    explicit SliceCanvas(QWidget *parent) : QRhiWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(this, &QRhiWidget::renderFailed, this, [this] {
            reportError(QStringLiteral("The graphics backend could not initialize the image viewer."));
        });
    }
    ~SliceCanvas() override { releaseResources(); }
    std::shared_ptr<const DecodedSlice> frame;
    QRectF imageRect;
    ImageMetadata metadata;
    double minimum = 0, maximum = 1, gamma = 1;
    int lut = 0;
    bool inverted = false, linear = false;
    QByteArray mask;
    QSize maskSize;
    std::size_t gpuBudget = 128 * 1024 * 1024;
    std::size_t gpuBytes = 0;
    std::function<void(const QString &)> failed;

protected:
    void initialize(QRhiCommandBuffer *) override
    {
        releaseResources();
        uniforms.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 160));
        nearest.reset(rhi()->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        smooth.reset(rhi()->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                     QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!uniforms->create() || !nearest->create() || !smooth->create()) {
            reportError(QStringLiteral("Could not create GPU display resources."));
            releaseResources();
        }
    }

    void render(QRhiCommandBuffer *cb) override
    {
        if (!uniforms || !frame) {
            cb->beginPass(renderTarget(), QColor("#FAF7F2"), {1.0f, 0});
            cb->endPass();
            return;
        }
        const auto bytes = static_cast<std::size_t>(frame->pixels.size());
        const auto maskBytes = mask.isEmpty() ? std::size_t(1) : static_cast<std::size_t>(mask.size());
        const auto sizeLimit = rhi()->resourceLimit(QRhi::TextureSizeMax);
        if (bytes > gpuBudget || maskBytes > gpuBudget - bytes || frame->size.width() > sizeLimit
            || frame->size.height() > sizeLimit) {
            pipeline.reset();
            bindings.reset();
            texture.reset();
            maskTexture.reset();
            gpuBytes = 0;
            reportError(QStringLiteral("This slice exceeds the GPU texture or memory limit."));
            cb->beginPass(renderTarget(), QColor("#FAF7F2"), {1.0f, 0});
            cb->endPass();
            return;
        }

        QRhiResourceUpdateBatch *updates = rhi()->nextResourceUpdateBatch();
        const QRhiTexture::Format format = frame->mode == 1 ? QRhiTexture::RGBA8
            : frame->mode == 2 ? QRhiTexture::RGBA32F : QRhiTexture::R32F;
        bool bindingsChanged = !bindings || boundLinear != linear;
        if (!texture || texture->pixelSize() != frame->size || texture->format() != format) {
            pipeline.reset();
            bindings.reset();
            texture.reset(rhi()->newTexture(format, frame->size));
            if (!rhi()->isTextureFormatSupported(format) || !texture->create()) {
                updates->release();
                reportError(QStringLiteral("The graphics backend does not support this pixel texture format."));
                return;
            }
            uploaded.reset();
            bindingsChanged = true;
        }
        const QSize requestedMaskSize = mask.isEmpty() ? QSize(1, 1) : maskSize;
        if (!maskTexture || maskTexture->pixelSize() != requestedMaskSize) {
            pipeline.reset();
            bindings.reset();
            maskTexture.reset(rhi()->newTexture(QRhiTexture::R8, requestedMaskSize));
            if (!maskTexture->create()) {
                updates->release();
                reportError(QStringLiteral("Could not create the mask texture."));
                return;
            }
            uploadedMask = QByteArray();
            bindingsChanged = true;
        }
        if (uploaded.lock() != frame) {
            QRhiTextureSubresourceUploadDescription data(frame->pixels);
            data.setSourceSize(frame->size);
            updates->uploadTexture(texture.get(), QRhiTextureUploadDescription({{0, 0, data}}));
            uploaded = frame;
        }
        const QByteArray maskData = mask.isEmpty() ? QByteArray(1, '\0') : mask;
        if (uploadedMask != maskData) {
            QRhiTextureSubresourceUploadDescription data(maskData);
            data.setSourceSize(requestedMaskSize);
            updates->uploadTexture(maskTexture.get(), QRhiTextureUploadDescription({{0, 0, data}}));
            uploadedMask = maskData;
        }
        gpuBytes = bytes + maskBytes;
        if (bindingsChanged) {
            pipeline.reset();
            bindings.reset(rhi()->newShaderResourceBindings());
            bindings->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage
                                                           | QRhiShaderResourceBinding::FragmentStage, uniforms.get()),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                         texture.get(), linear ? smooth.get() : nearest.get()),
                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                         maskTexture.get(), nearest.get())});
            if (!bindings->create()) {
                updates->release();
                reportError(QStringLiteral("Could not bind the image texture."));
                return;
            }
            boundLinear = linear;
        }
        if (!pipeline) {
            pipeline.reset(rhi()->newGraphicsPipeline());
            pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
            pipeline->setShaderStages({
                {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/imagecxx/slice.vert.qsb"))},
                {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/imagecxx/slice.frag.qsb"))}});
            pipeline->setShaderResourceBindings(bindings.get());
            pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            pipeline->setSampleCount(sampleCount());
            if (!pipeline->create()) {
                pipeline.reset();
                updates->release();
                reportError(QStringLiteral("Could not create the image shader pipeline."));
                return;
            }
        }
        std::array<float, 40> parameters{};
        QMatrix4x4 matrix = rhi()->clipSpaceCorrMatrix();
        matrix.ortho(0.0f, float(width()), float(height()), 0.0f, -1.0f, 1.0f);
        matrix.translate(float(imageRect.x()), float(imageRect.y()));
        matrix.scale(float(imageRect.width()), float(imageRect.height()));
        std::copy_n(matrix.constData(), 16, parameters.begin());
        parameters[16] = float((minimum - frame->offset) / frame->scale);
        parameters[17] = float(frame->scale / std::max(maximum - minimum, 1e-300));
        parameters[18] = float(1.0 / gamma);
        parameters[19] = inverted ? 1.0f : 0.0f;
        parameters[20] = float(frame->mode);
        parameters[21] = float(lut);
        parameters[22] = float(frame->channels);
        parameters[23] = mask.isEmpty() ? 0.0f : 1.0f;
        constexpr std::array<QRgb, 4> defaults{0xffff4545, 0xff55e887, 0xff529bff, 0xffeeeeee};
        for (std::size_t c = 0; c < 4; ++c) {
            const QColor color = QColor::fromRgba(c < metadata.channels.size() ? metadata.channels[c].color : defaults[c]);
            parameters[24 + c * 4] = float(color.redF());
            parameters[25 + c * 4] = float(color.greenF());
            parameters[26 + c * 4] = float(color.blueF());
        }
        updates->updateDynamicBuffer(uniforms.get(), 0, sizeof(parameters), parameters.data());
        cb->beginPass(renderTarget(), QColor("#FAF7F2"), {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline.get());
        const QSize targetSize = renderTarget()->pixelSize();
        cb->setViewport({0, 0, float(targetSize.width()), float(targetSize.height())});
        cb->setShaderResources(bindings.get());
        cb->draw(4);
        cb->endPass();
    }

    void releaseResources() override
    {
        pipeline.reset();
        bindings.reset();
        texture.reset();
        maskTexture.reset();
        uniforms.reset();
        nearest.reset();
        smooth.reset();
        uploaded.reset();
        uploadedMask.clear();
        gpuBytes = 0;
    }

private:
    void reportError(const QString &message)
    {
        if (lastError != message && failed) {
            lastError = message;
            failed(message);
        }
    }
    QString lastError;
    std::unique_ptr<QRhiBuffer> uniforms;
    std::unique_ptr<QRhiTexture> texture, maskTexture;
    std::unique_ptr<QRhiSampler> nearest, smooth;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
    std::weak_ptr<const DecodedSlice> uploaded;
    QByteArray uploadedMask;
    bool boundLinear = false;
};

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
    };
    SliceCanvas *canvas = nullptr;
    ViewerOverlay *overlay = nullptr;
    Image image, mask;
    std::size_t z = 0, channel = 0, time = 0;
    std::uint64_t imageRevision = 0;
    double zoom = 1;
    QPointF pan;
    QPointF mouseImage{-1, -1}, lastMouse;
    Tool tool = Tool::Rectangle;
    std::vector<Roi> rois;
    std::optional<Roi> draft, selection, dragOriginal;
    bool editing = false;
    int dragHandle = -1;
    QPointF dragStart;
    bool panning = false, drawing = false, fitted = true, spacePan = false;
    bool crosshair = false, scaleBar = false, composite = false, autoRangePending = true;
    bool loading = false;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    std::deque<std::shared_ptr<const DecodedSlice>> cache;
    std::size_t cacheBytes = 0, decodedBudget = 128 * 1024 * 1024;
    std::atomic<std::uint64_t> serial{0};
    std::jthread worker;
};

ImageViewer::ImageViewer(QWidget *parent) : QWidget(parent), d(std::make_unique<Private>())
{
    setMinimumSize(160, 120);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    d->canvas = new SliceCanvas(this);
    d->overlay = new ViewerOverlay(this);
    d->canvas->failed = [this](const QString &message) { Q_EMIT loadFailed(message); };
    d->overlay->paint = [this](QPainter &painter) {
        if (d->image.empty()) {
            painter.setPen(QColor("#1A1C1F"));
            painter.drawText(rect(), Qt::AlignCenter, tr("Open an image to begin"));
            return;
        }
        const auto drawRoi = [this, &painter](const Roi &roi, bool draft) {
            if (!roi.visible || roi.time != d->time
                || (!d->composite && roi.channel != d->channel) || roi.points.size() < 2)
                return;
            painter.save();
            const QColor color = draft ? QColor(255, 213, 115) : QColor::fromRgba(roi.color);
            painter.setPen(QPen(color, 1.6, draft ? Qt::DashLine : Qt::SolidLine));
            painter.setBrush(Qt::NoBrush);
            const QRectF bounds(imageToWidget(roi.bounds().topLeft()), imageToWidget(roi.bounds().bottomRight()));
            const auto origin = imageToWidget({0, 0});
            const QTransform transform(d->zoom, 0, 0, d->zoom, origin.x(), origin.y());
            if (draft && roi.type == RoiType::Polygon) {
                QPolygonF points;
                for (QPointF point : roi.points) points << imageToWidget(point);
                painter.drawPolyline(points);
            } else painter.drawPath(transform.map(roiPath(roi)));
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
        for (const Roi &roi : d->rois)
            if (!d->selection || roi.id != d->selection->id) drawRoi(roi, false);
        if (d->selection) {
            drawRoi(*d->selection, false);
            if (d->editing && d->selection->visible && d->selection->time == d->time
                && (d->composite || d->selection->channel == d->channel)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 213, 115));
                for (const auto &handle : roiHandles(*d->selection))
                    painter.drawRect(QRectF(imageToWidget(handle) - QPointF(3, 3), QSizeF(6, 6)));
            }
        }
        if (d->draft) drawRoi(*d->draft, true);
        if (d->crosshair && d->mouseImage.x() >= 0) {
            const QPointF point = imageToWidget(d->mouseImage);
            painter.setPen(QPen(QColor(240, 245, 255, 150), 1, Qt::DashLine));
            painter.drawLine(QPointF(point.x(), 0), QPointF(point.x(), height()));
            painter.drawLine(QPointF(0, point.y()), QPointF(width(), point.y()));
        }
        if (d->scaleBar && width() > 180 && height() > 100) {
            const auto &axes = d->image.metadata().axes;
            const double spacing = axes.empty() ? 1.0 : axes[0].spacing;
            const QString unit = axes.empty() ? QStringLiteral("px") : axes[0].unit;
            const double target = 110.0 * spacing / d->zoom;
            if (spacing > 0 && std::isfinite(target) && target > 0) {
                const double power = std::pow(10.0, std::floor(std::log10(target)));
                const double fraction = target / power;
                const double length = (fraction >= 5 ? 5 : fraction >= 2 ? 2 : 1) * power;
                const double pixels = length / spacing * d->zoom;
                const QPointF end(width() - 24.0, height() - 26.0);
                painter.setPen(QPen(QColor(0, 0, 0, 170), 6));
                painter.drawLine(end - QPointF(pixels, 0), end);
                painter.setPen(QPen(Qt::white, 2));
                painter.drawLine(end - QPointF(pixels, 0), end);
                painter.drawText(QRectF(end.x() - pixels - 25, end.y() - 24, pixels + 50, 19),
                                 Qt::AlignCenter, QStringLiteral("%1 %2").arg(length, 0, 'g', 4).arg(unit));
            }
        }
    };

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
            std::array<std::size_t, 3> slices{request.key.z,
                request.key.z + 1 < request.image.depth() ? request.key.z + 1 : request.key.z,
                request.key.z > 0 ? request.key.z - 1 : request.key.z};
            for (std::size_t index = 0; index < slices.size(); ++index) {
                if (stop.stop_requested() || request.serial != d->serial.load()) break;
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
                        auto result = std::make_shared<DecodedSlice>();
                        result->key = key;
                        const Image &image = request.image;
                        const bool color = componentCount(image.pixelType()) > 1;
                        result->mode = color ? 1 : key.composite ? 2 : 0;
                        result->channels = result->mode == 2 ? int(std::min<std::size_t>(4, image.channels())) : 1;
                        const std::size_t components = result->mode == 0 ? 1 : 4;
                        const bool doublePrecision = image.pixelType() == PixelType::Float64;
                        const std::size_t bytesPerSample = color ? 1 : doublePrecision ? sizeof(double) : sizeof(float);
                        const std::size_t perPixel = components * bytesPerSample;
                        std::size_t budget;
                        {
                            std::lock_guard lock(d->mutex);
                            budget = d->decodedBudget;
                        }
                        if (image.width() > std::size_t(std::numeric_limits<int>::max())
                            || image.height() > std::size_t(std::numeric_limits<int>::max())
                            || image.width() > budget / perPixel / image.height())
                            throw std::runtime_error("The decoded slice exceeds the viewer cache memory limit.");
                        result->size = QSize(int(image.width()), int(image.height()));
                        const auto byteCount = image.width() * image.height() * perPixel;
                        result->pixels = QByteArray(qsizetype(byteCount), Qt::Uninitialized);
                        result->minimum = std::numeric_limits<double>::infinity();
                        result->maximum = -std::numeric_limits<double>::infinity();
                        std::vector<Image> planes;
                        for (int c = 0; c < result->channels; ++c)
                            planes.push_back(image.slice(key.z, result->mode == 2 ? std::size_t(c) : key.channel, key.time));
                        for (std::size_t y = 0; y < image.height(); ++y) {
                            if (stop.stop_requested() || request.serial != d->serial.load()) break;
                            for (std::size_t x = 0; x < image.width(); ++x) {
                                const auto offset = (y * image.width() + x) * components;
                                for (std::size_t c = 0; c < components; ++c) {
                                    double value = color && c == 3 && componentCount(image.pixelType()) == 3 ? 255.0
                                        : color ? planes[0].sample(x, y, 0, 0, 0, c)
                                        : c < std::size_t(result->channels) ? planes[c].sample(x, y) : 0.0;
                                    if (c < std::size_t(color ? 3 : result->channels) && std::isfinite(value)) {
                                        result->minimum = std::min(result->minimum, value);
                                        result->maximum = std::max(result->maximum, value);
                                    }
                                    if (!std::isfinite(value)) value = 0;
                                    if (color) {
                                        const auto displayValue = std::uint8_t(std::clamp(value, 0.0, 255.0));
                                        std::memcpy(result->pixels.data() + offset + c, &displayValue, sizeof(displayValue));
                                    } else if (doublePrecision) {
                                        std::memcpy(result->pixels.data() + (offset + c) * sizeof(double), &value, sizeof(double));
                                    } else {
                                        const float displayValue = float(std::clamp(value,
                                            -double(std::numeric_limits<float>::max()), double(std::numeric_limits<float>::max())));
                                        std::memcpy(result->pixels.data() + (offset + c) * sizeof(float), &displayValue, sizeof(float));
                                    }
                                }
                            }
                        }
                        if (stop.stop_requested() || request.serial != d->serial.load()) break;
                        if (!std::isfinite(result->minimum)) { result->minimum = 0; result->maximum = 1; }
                        if (result->maximum <= result->minimum) {
                            const double expanded = result->minimum + std::max(1.0, std::abs(result->minimum) * 1e-6);
                            result->maximum = expanded;
                            if (!std::isfinite(expanded)) {
                                result->minimum = std::nextafter(result->minimum, -std::numeric_limits<double>::infinity());
                                result->maximum = std::numeric_limits<double>::max();
                            }
                        }
                        if (color) { result->minimum = 0; result->maximum = 255; }
                        if (doublePrecision) {
                            result->offset = result->minimum;
                            result->scale = result->maximum - result->minimum;
                            if (!std::isfinite(result->scale)) {
                                result->offset = 0;
                                result->scale = std::max(std::abs(result->minimum), std::abs(result->maximum));
                            }
                            const auto values = image.width() * image.height() * components;
                            for (std::size_t pixel = 0; pixel < values; ++pixel) {
                                double value;
                                std::memcpy(&value, result->pixels.constData() + pixel * sizeof(double), sizeof(double));
                                const float encoded = float((value - result->offset) / result->scale);
                                std::memcpy(result->pixels.data() + pixel * sizeof(float), &encoded, sizeof(float));
                            }
                            result->pixels.resize(qsizetype(values * sizeof(float)));
                        }
                        result->pixels.squeeze();
                        decoded = result;
                        {
                            std::lock_guard lock(d->mutex);
                            const auto retainedBytes = std::size_t(decoded->pixels.capacity());
                            while (!d->cache.empty() && d->cacheBytes + retainedBytes > d->decodedBudget) {
                                d->cacheBytes -= std::size_t(d->cache.back()->pixels.capacity());
                                d->cache.pop_back();
                            }
                            if (retainedBytes <= d->decodedBudget) {
                                d->cache.push_front(decoded);
                                d->cacheBytes += retainedBytes;
                            }
                        }
                    }
                    if (index == 0) {
                        QMetaObject::invokeMethod(this, [this, decoded, serial = request.serial] {
                            if (serial != d->serial.load()) return;
                            d->canvas->frame = decoded;
                            d->loading = false;
                            if (d->autoRangePending) {
                                d->autoRangePending = false;
                                // RGB8 textures are normalized by the sampler.
                                const double divisor = decoded->mode == 1 ? 255.0 : 1.0;
                                d->canvas->minimum = decoded->minimum / divisor;
                                d->canvas->maximum = decoded->maximum / divisor;
                                Q_EMIT displayRangeChanged(decoded->minimum, decoded->maximum);
                            }
                            updateCanvas();
                            Q_EMIT loadingChanged(false);
                            Q_EMIT sliceReady();
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
    d->autoRangePending = true;
    d->mask = {};
    d->canvas->mask.clear();
    d->canvas->frame.reset();
    d->canvas->metadata = image.metadata();
    d->draft.reset();
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
    d->z = z; d->channel = channel; d->time = time;
    // A mask created on one plane must not appear on unrelated slices.
    d->mask = {};
    d->canvas->mask.clear();
    d->draft.reset();
    if (d->dragOriginal) d->selection = d->dragOriginal;
    d->dragOriginal.reset(); d->drawing = false;
    if (planeGroupChanged) d->editing = false;
    requestSlice();
    Q_EMIT sliceChanged(z, channel, time);
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
    {
        std::lock_guard lock(d->mutex);
        d->pending = Private::Request{{d->imageRevision, d->z, d->channel, d->time, d->composite}, d->image, serial};
    }
    d->loading = true;
    d->condition.notify_one();
    updateCanvas();
    Q_EMIT loadingChanged(true);
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
        updateCanvas();
    }
}
void ImageViewer::setGamma(double gamma)
{
    if (!std::isfinite(gamma) || gamma < 0.01 || gamma > 100) return;
    if (d->canvas->gamma == gamma) return;
    d->canvas->gamma = gamma;
    updateCanvas();
}
double ImageViewer::gamma() const { return d->canvas->gamma; }
void ImageViewer::setInverted(bool inverted)
{
    if (d->canvas->inverted == inverted) return;
    d->canvas->inverted = inverted;
    updateCanvas();
}
bool ImageViewer::isInverted() const { return d->canvas->inverted; }
void ImageViewer::setLut(const QString &name)
{
    const int lut = name.compare(QStringLiteral("hot"), Qt::CaseInsensitive) == 0 ? 1
        : name.compare(QStringLiteral("viridis"), Qt::CaseInsensitive) == 0 ? 2 : 0;
    if (d->canvas->lut == lut) return;
    d->canvas->lut = lut;
    updateCanvas();
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
}
bool ImageViewer::linearInterpolation() const { return d->canvas->linear; }
void ImageViewer::setComposite(bool enabled)
{
    if (d->composite == enabled) return;
    d->composite = enabled;
    requestSlice();
}
bool ImageViewer::composite() const { return d->composite; }

void ImageViewer::fitToWindow()
{
    d->fitted = true;
    d->pan = {};
    if (!d->image.empty())
        d->zoom = std::max(1e-6, std::min(double(width()) / double(d->image.width()),
                                        double(height()) / double(d->image.height())));
    updateCanvas();
    Q_EMIT zoomChanged(d->zoom);
}
void ImageViewer::setZoom(double scale)
{
    if (!std::isfinite(scale) || scale <= 0) return;
    d->fitted = false;
    d->zoom = std::clamp(scale, 1e-6, 256.0);
    updateCanvas();
    Q_EMIT zoomChanged(d->zoom);
}
double ImageViewer::zoom() const { return d->zoom; }
void ImageViewer::setTool(Tool tool)
{
    d->tool = tool;
    d->draft.reset();
    d->drawing = false; d->editing = false; d->dragOriginal.reset();
    setCursor(tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    updateCanvas();
}
ImageViewer::Tool ImageViewer::tool() const { return d->tool; }
void ImageViewer::setRois(const std::vector<Roi> &rois)
{
    d->rois = rois;
    if (d->selection && !d->dragOriginal) {
        const auto saved = std::find_if(rois.begin(), rois.end(), [this](const Roi &roi) { return roi.id == d->selection->id; });
        if (saved != rois.end()) d->selection = *saved;
    }
    d->overlay->update();
}
void ImageViewer::setSelection(std::optional<Roi> roi)
{
    if (!roi || !d->selection || roi->id != d->selection->id) {
        d->editing = false;
        d->dragOriginal.reset();
    }
    d->selection = std::move(roi);
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
void ImageViewer::setCrosshairVisible(bool visible) { d->crosshair = visible; d->overlay->update(); }
void ImageViewer::setScaleBarVisible(bool visible) { d->scaleBar = visible; d->overlay->update(); }
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
std::size_t ImageViewer::gpuCacheBytes() const { return d->canvas->gpuBytes; }

QPointF ImageViewer::imageToWidget(QPointF point) const
{
    const QPointF origin = QPointF(width(), height()) / 2.0 + d->pan
        - QPointF(double(d->image.width()), double(d->image.height())) * (d->zoom / 2.0);
    return origin + point * d->zoom;
}
QPointF ImageViewer::widgetToImage(QPointF point) const
{
    return (point - imageToWidget({0, 0})) / d->zoom;
}
void ImageViewer::updateCanvas()
{
    if (!d->image.empty())
        d->canvas->imageRect = QRectF(imageToWidget({0, 0}), QSizeF(double(d->image.width()) * d->zoom, double(d->image.height()) * d->zoom));
    d->canvas->update();
    d->overlay->update();
}
void ImageViewer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    d->canvas->setGeometry(rect());
    d->overlay->setGeometry(rect());
    d->overlay->raise();
    if (d->fitted) fitToWindow();
    else updateCanvas();
}
void ImageViewer::wheelEvent(QWheelEvent *event)
{
    if (d->image.empty()) return;
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
        const QPointF anchor = widgetToImage(event->position());
        const double factor = std::pow(1.2, double(event->angleDelta().y()) / 120.0);
        setZoom(d->zoom * factor);
        d->pan += event->position() - imageToWidget(anchor);
        updateCanvas();
    } else if (d->image.depth() > 1) {
        const auto step = event->angleDelta().y() > 0 ? -1 : 1;
        const auto z = std::clamp<qint64>(qint64(d->z) + step, 0, qint64(d->image.depth()) - 1);
        setSlice(std::size_t(z), d->channel, d->time);
    } else {
        const QPointF anchor = widgetToImage(event->position());
        setZoom(d->zoom * std::pow(1.2, double(event->angleDelta().y()) / 120.0));
        d->pan += event->position() - imageToWidget(anchor);
        updateCanvas();
    }
    event->accept();
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
    if (event->button() == Qt::RightButton && d->tool == Tool::Polygon) { completeRoi(); return; }
    if (event->button() != Qt::LeftButton) return;
    const QPointF point = widgetToImage(event->position());
    const bool onImage = point.x() >= 0 && point.y() >= 0
        && point.x() < double(d->image.width()) && point.y() < double(d->image.height());
    if (!d->draft) {
        if (d->editing && d->selection) {
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
        const auto hit = [this, event, point](const Roi &roi) {
            if (!roi.visible || roi.time != d->time
                || (!d->composite && roi.channel != d->channel)) return false;
            const auto origin = imageToWidget({0, 0});
            const QTransform transform(d->zoom, 0, 0, d->zoom, origin.x(), origin.y());
            QPainterPathStroker stroker;
            stroker.setWidth(12);
            return roi.contains(point) || stroker.createStroke(transform.map(roiPath(roi))).contains(event->position());
        };
        std::optional<Roi> picked;
        if (d->selection && hit(*d->selection)) picked = d->selection;
        else for (auto it = d->rois.rbegin(); it != d->rois.rend(); ++it)
            if (hit(*it)) { picked = *it; break; }
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
        if (onImage && d->editing) {
            d->editing = false; d->dragOriginal.reset(); updateCanvas();
            return;
        }
        if (onImage && d->selection) {
            d->selection.reset();
            Q_EMIT selectionCleared();
            updateCanvas();
            // A drag can start a replacement ROI; a click simply clears it.
            if (d->tool == Tool::Polygon) return;
        }
    }
    if (d->tool == Tool::Pan) {
        d->panning = true; d->fitted = false; setCursor(Qt::ClosedHandCursor);
        return;
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
        roi.points = d->tool == Tool::Polygon ? std::vector<QPointF>{point} : std::vector<QPointF>{point, point};
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
        d->draft->points[1] = constrainedPoint(d->draft->points[0], point, d->draft->type, size, shift);
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
    } else if (d->drawing || d->dragOriginal) updateRoiDrag(point, event->modifiers());
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
        setCursor((d->tool == Tool::Pan || d->spacePan) ? Qt::OpenHandCursor : Qt::CrossCursor);
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
    else if (d->tool == Tool::Pan) fitToWindow();
}
void ImageViewer::completeRoi()
{
    d->drawing = false;
    if (!d->draft) return;
    Roi roi = *d->draft;
    d->draft.reset();
    roi.points.erase(std::unique(roi.points.begin(), roi.points.end()), roi.points.end());
    if (validRoi(roi)) { d->selection = roi; d->editing = false; Q_EMIT roiCreated(roi); }
    updateCanvas();
}
void ImageViewer::keyPressEvent(QKeyEvent *event)
{
    if (d->image.empty()) { QWidget::keyPressEvent(event); return; }
    if (event->key() == Qt::Key_Escape) {
        d->draft.reset(); d->drawing = false;
        if (d->dragOriginal) { d->selection = d->dragOriginal; d->dragOriginal.reset(); }
        if (d->editing) d->editing = false;
        else if (d->selection) { d->selection.reset(); Q_EMIT selectionCleared(); }
        updateCanvas();
    } else if (event->key() == Qt::Key_Shift && !event->isAutoRepeat()) {
        updateRoiDrag(widgetToImage(d->lastMouse), event->modifiers() | Qt::ShiftModifier);
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
        setCursor(d->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
        event->accept();
    } else if (event->key() == Qt::Key_Shift && !event->isAutoRepeat()) {
        updateRoiDrag(widgetToImage(d->lastMouse), event->modifiers() & ~Qt::ShiftModifier);
        event->accept();
    } else QWidget::keyReleaseEvent(event);
}

void ImageViewer::focusOutEvent(QFocusEvent *event)
{
    d->spacePan = false; d->panning = false;
    if (d->dragOriginal) {
        d->selection = d->dragOriginal;
        d->dragOriginal.reset();
        updateCanvas();
    }
    setCursor(d->tool == Tool::Pan ? Qt::OpenHandCursor : Qt::CrossCursor);
    QWidget::focusOutEvent(event);
}

} // namespace imagecxx
