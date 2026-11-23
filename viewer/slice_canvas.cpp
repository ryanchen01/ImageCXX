#include "viewer/viewer_internal.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QMatrix4x4>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace imagecxx {
namespace detail {
namespace {

QShader loadShader(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QShader::fromSerialized(file.readAll()) : QShader{};
}

} // namespace

struct SliceCanvas::GpuState {
    struct Resources {
        std::unique_ptr<QRhiBuffer> uniforms;
        std::unique_ptr<QRhiTexture> texture, maskTexture;
        std::unique_ptr<QRhiShaderResourceBindings> bindings;
        std::weak_ptr<const DecodedSlice> uploaded;
        QByteArray uploadedMask;
        bool boundLinear = false;
    };
    QRhi *rhi = nullptr;
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDesc;
    std::vector<std::unique_ptr<Resources>> resources;
    std::unique_ptr<QRhiSampler> nearest, smooth;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline;
};

SliceCanvas::SliceCanvas(QWidget *parent) : QRhiWidget(parent), gpu(std::make_unique<GpuState>())
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(this, &QRhiWidget::renderFailed, this, [this] {
        reportError(QCoreApplication::translate("ImageViewer", "The graphics backend could not initialize the image viewer."));
    });
}

SliceCanvas::~SliceCanvas()
{
    releaseResources();
}

void SliceCanvas::initialize(QRhiCommandBuffer *)
{
    if (gpu->rhi != rhi()) {
        releaseResources();
        gpu->rhi = rhi();
    }
    if (gpu->pipeline
        && (gpu->pipeline->sampleCount() != sampleCount()
            || !gpu->renderPassDesc->isCompatible(renderTarget()->renderPassDescriptor()))) {
        gpu->pipeline.reset();
        gpu->renderPassDesc.reset();
    }
    if (!gpu->nearest || !gpu->smooth) {
        gpu->nearest.reset(rhi()->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                             QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        gpu->smooth.reset(rhi()->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                            QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!gpu->nearest->create() || !gpu->smooth->create()) {
            reportError(QCoreApplication::translate("ImageViewer", "Could not create GPU display resources."));
            releaseResources();
        }
    }
}

void SliceCanvas::render(QRhiCommandBuffer *cb)
{
    QList<CanvasLayer> active = layers;
    if (!overlayMode && frame) {
        active = {{frame, QTransform(imageRect.width(), 0, 0, imageRect.height(), imageRect.x(), imageRect.y()),
            metadata, minimum, maximum, gamma, 1, lut, inverted, linear, mask, maskSize}};
    }
    const QColor background = overlayMode ? QColor(Qt::black) : QColor("#F7F4EE");
    std::size_t bytes = 0;
    const auto sizeLimit = rhi()->resourceLimit(QRhi::TextureSizeMax);
    for (const auto &layer : active) {
        const auto textureBytes = std::size_t(layer.frame->pixels.size());
        const auto maskBytes = layer.mask.isEmpty() ? std::size_t(1) : std::size_t(layer.mask.size());
        if (textureBytes > gpuBudget - bytes || maskBytes > gpuBudget - bytes - textureBytes
            || layer.frame->size.width() > sizeLimit || layer.frame->size.height() > sizeLimit) {
            gpu->pipeline.reset();
            gpu->resources.clear();
            gpuBytes = 0;
            reportError(QCoreApplication::translate("ImageViewer", "This view exceeds the GPU texture or memory limit."));
            cb->beginPass(renderTarget(), background, {1.0f, 0});
            cb->endPass();
            return;
        }
        bytes += textureBytes + maskBytes;
    }
    gpuBytes = bytes;
    if (!gpu->nearest || !gpu->smooth || active.isEmpty()) {
        gpu->pipeline.reset();
        gpu->resources.clear();
        cb->beginPass(renderTarget(), background, {1.0f, 0});
        cb->endPass();
        return;
    }
    if (gpu->resources.size() != std::size_t(active.size())) {
        gpu->pipeline.reset();
        gpu->resources.resize(std::size_t(active.size()));
    }
    auto *updates = rhi()->nextResourceUpdateBatch();
    for (qsizetype index = 0; index < active.size(); ++index) {
        const auto &layer = active[index];
        const auto &decoded = layer.frame;
        auto &resource = gpu->resources[std::size_t(index)];
        if (!resource) resource = std::make_unique<GpuState::Resources>();
        auto &slot = *resource;
        if (!slot.uniforms) {
            slot.uniforms.reset(rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 176));
            if (!slot.uniforms->create()) {
                updates->release();
                reportError(QCoreApplication::translate("ImageViewer", "Could not create GPU display resources."));
                return;
            }
        }
        const auto format = decoded->mode == 1 ? QRhiTexture::RGBA8
            : decoded->mode == 2 ? QRhiTexture::RGBA32F : QRhiTexture::R32F;
        bool bindingsChanged = !slot.bindings || slot.boundLinear != layer.linear;
        if (!slot.texture || slot.texture->pixelSize() != decoded->size || slot.texture->format() != format) {
            gpu->pipeline.reset();
            slot.bindings.reset();
            slot.texture.reset(rhi()->newTexture(format, decoded->size));
            if (!rhi()->isTextureFormatSupported(format) || !slot.texture->create()) {
                updates->release();
                reportError(QCoreApplication::translate("ImageViewer", "The graphics backend does not support this pixel texture format."));
                return;
            }
            slot.uploaded.reset();
            bindingsChanged = true;
        }
        const QSize requestedMaskSize = layer.mask.isEmpty() ? QSize(1, 1) : layer.maskSize;
        if (!slot.maskTexture || slot.maskTexture->pixelSize() != requestedMaskSize) {
            gpu->pipeline.reset();
            slot.bindings.reset();
            slot.maskTexture.reset(rhi()->newTexture(QRhiTexture::R8, requestedMaskSize));
            if (!slot.maskTexture->create()) {
                updates->release();
                reportError(QCoreApplication::translate("ImageViewer", "Could not create the mask texture."));
                return;
            }
            slot.uploadedMask.clear();
            bindingsChanged = true;
        }
        if (slot.uploaded.lock() != decoded) {
            QRhiTextureSubresourceUploadDescription data(decoded->pixels);
            data.setSourceSize(decoded->size);
            updates->uploadTexture(slot.texture.get(), QRhiTextureUploadDescription({{0, 0, data}}));
            slot.uploaded = decoded;
        }
        static const QByteArray emptyMask(1, '\0');
        const QByteArray &maskData = layer.mask.isEmpty() ? emptyMask : layer.mask;
        if (slot.uploadedMask != maskData) {
            QRhiTextureSubresourceUploadDescription data(maskData);
            data.setSourceSize(requestedMaskSize);
            updates->uploadTexture(slot.maskTexture.get(), QRhiTextureUploadDescription({{0, 0, data}}));
            slot.uploadedMask = maskData;
        }
        if (bindingsChanged) {
            gpu->pipeline.reset();
            slot.bindings.reset(rhi()->newShaderResourceBindings());
            slot.bindings->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage
                                                           | QRhiShaderResourceBinding::FragmentStage, slot.uniforms.get()),
                QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                    slot.texture.get(), layer.linear ? gpu->smooth.get() : gpu->nearest.get()),
                QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                    slot.maskTexture.get(), gpu->nearest.get())});
            if (!slot.bindings->create()) {
                updates->release();
                reportError(QCoreApplication::translate("ImageViewer", "Could not bind the image texture."));
                return;
            }
            slot.boundLinear = layer.linear;
        }
        std::array<float, 44> parameters{};
        QMatrix4x4 matrix = rhi()->clipSpaceCorrMatrix();
        matrix.ortho(0.0f, float(width()), float(height()), 0.0f, -1.0f, 1.0f);
        matrix *= QMatrix4x4(layer.transform);
        std::copy_n(matrix.constData(), 16, parameters.begin());
        const double relativeMinimum = layer.minimum - decoded->offset;
        parameters[16] = float(std::isfinite(relativeMinimum) ? relativeMinimum / decoded->scale
            : layer.minimum / decoded->scale - decoded->offset / decoded->scale);
        const double range = layer.maximum - layer.minimum;
        parameters[17] = float(std::isfinite(range)
            ? decoded->scale / std::max(range, std::numeric_limits<double>::denorm_min())
            : (decoded->scale / 2) / (layer.maximum / 2 - layer.minimum / 2));
        parameters[18] = float(1.0 / layer.gamma);
        parameters[19] = layer.inverted ? 1.0f : 0.0f;
        parameters[20] = float(decoded->mode);
        parameters[21] = float(layer.lut);
        parameters[22] = float(decoded->channels);
        parameters[23] = layer.mask.isEmpty() ? 0.0f : 1.0f;
        constexpr std::array<QRgb, 4> defaults{0xffff4545, 0xff55e887, 0xff529bff, 0xffeeeeee};
        for (std::size_t c = 0; c < 4; ++c) {
            const QColor color = QColor::fromRgba(c < layer.metadata.channels.size() ? layer.metadata.channels[c].color : defaults[c]);
            parameters[24 + c * 4] = float(color.redF());
            parameters[25 + c * 4] = float(color.greenF());
            parameters[26 + c * 4] = float(color.blueF());
        }
        parameters[40] = float(layer.opacity);
        updates->updateDynamicBuffer(slot.uniforms.get(), 0, sizeof(parameters), parameters.data());
    }
    if (!gpu->pipeline) {
        gpu->renderPassDesc.reset(renderTarget()->renderPassDescriptor()->newCompatibleRenderPassDescriptor());
        gpu->pipeline.reset(rhi()->newGraphicsPipeline());
        gpu->pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        gpu->pipeline->setShaderStages({
            {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/imagecxx/slice.vert.qsb"))},
            {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/imagecxx/slice.frag.qsb"))}});
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        gpu->pipeline->setTargetBlends({blend});
        gpu->pipeline->setShaderResourceBindings(gpu->resources.front()->bindings.get());
        gpu->pipeline->setRenderPassDescriptor(gpu->renderPassDesc.get());
        gpu->pipeline->setSampleCount(sampleCount());
        if (!gpu->renderPassDesc || !gpu->pipeline->create()) {
            gpu->pipeline.reset();
            gpu->renderPassDesc.reset();
            updates->release();
            reportError(QCoreApplication::translate("ImageViewer", "Could not create the image shader pipeline."));
            return;
        }
    }
    cb->beginPass(renderTarget(), background, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(gpu->pipeline.get());
    const QSize targetSize = renderTarget()->pixelSize();
    cb->setViewport({0, 0, float(targetSize.width()), float(targetSize.height())});
    for (const auto &resource : gpu->resources) {
        cb->setShaderResources(resource->bindings.get());
        cb->draw(4);
    }
    cb->endPass();
}

void SliceCanvas::releaseResources()
{
    gpu->pipeline.reset();
    gpu->renderPassDesc.reset();
    gpu->resources.clear();
    gpu->nearest.reset();
    gpu->smooth.reset();
    gpu->rhi = nullptr;
    gpuBytes = 0;
}

void SliceCanvas::reportError(const QString &message)
{
    if (lastError != message && failed) {
        lastError = message;
        failed(message);
    }
}

std::shared_ptr<const DecodedSlice> decodeSlice(const Image &image, const SliceKey &key,
                                                std::size_t decodedBudget,
                                                const std::function<bool()> &cancelled)
{
    auto result = std::make_shared<DecodedSlice>();
    result->key = key;
    const auto sourceComponents = componentCount(image.pixelType());
    const bool color = sourceComponents > 1;
    result->mode = color ? 1 : key.composite ? 2 : 0;
    result->channels = result->mode == 2 ? int(std::min<std::size_t>(4, image.channels())) : 1;
    const std::size_t components = result->mode == 0 ? 1 : 4;
    const bool doublePrecision = image.pixelType() == PixelType::Float64;
    const auto *base = image.data();
    const bool lazyDouble = doublePrecision && !base;
    const std::size_t bytesPerSample = color ? 1 : lazyDouble ? sizeof(double) : sizeof(float);
    const std::size_t perPixel = components * bytesPerSample;
    const auto width = image.width(), height = image.height();
    if (width > std::size_t(std::numeric_limits<int>::max())
        || height > std::size_t(std::numeric_limits<int>::max())
        || width > decodedBudget / perPixel / height)
        throw std::runtime_error(QCoreApplication::translate("ImageViewer", "The decoded slice exceeds the viewer cache memory limit.").toStdString());
    if (key.z >= image.depth() || key.channel >= image.channels() || key.time >= image.frames())
        throw std::out_of_range(QCoreApplication::translate("Image", "Slice is outside the image").toStdString());
    result->size = QSize(int(width), int(height));
    result->pixels = QByteArray(qsizetype(width * height * perPixel), Qt::Uninitialized);
    result->minimum = std::numeric_limits<double>::infinity();
    result->maximum = -std::numeric_limits<double>::infinity();
    auto *destination = result->pixels.data();

    const auto finalizeRange = [&] {
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
    };

    if (base) {
        const auto &shape = image.shape();
        const auto &strides = image.strides();
        const std::size_t strideX = strides[0], strideY = strides[1];
        const std::size_t basePlaneOffset = (shape.size() > 2 ? key.z * strides[2] : 0)
            + (shape.size() > 4 ? key.time * strides[4] : 0);
        std::array<const std::byte *, 4> planes{};
        for (int c = 0; c < result->channels; ++c) {
            const std::size_t ch = result->mode == 2 ? std::size_t(c) : key.channel;
            planes[std::size_t(c)] = base + basePlaneOffset + (shape.size() > 3 ? ch * strides[3] : 0);
        }
        if (color) {
            const auto *plane = planes[0];
            auto *dst = reinterpret_cast<std::uint8_t *>(destination);
            for (std::size_t y = 0; y < height; ++y) {
                if (cancelled()) return {};
                const auto *srcRow = plane + y * strideY;
                auto *dstRow = dst + y * width * 4;
                if (sourceComponents == 4 && strideX == 4) {
                    std::memcpy(dstRow, srcRow, width * 4);
                } else if (sourceComponents == 4) {
                    for (std::size_t x = 0; x < width; ++x)
                        std::memcpy(dstRow + x * 4, srcRow + x * strideX, 4);
                } else {
                    for (std::size_t x = 0; x < width; ++x) {
                        const auto *px = srcRow + x * strideX;
                        dstRow[x * 4 + 0] = std::to_integer<std::uint8_t>(px[0]);
                        dstRow[x * 4 + 1] = std::to_integer<std::uint8_t>(px[1]);
                        dstRow[x * 4 + 2] = std::to_integer<std::uint8_t>(px[2]);
                        dstRow[x * 4 + 3] = 255;
                    }
                }
            }
            result->minimum = 0;
            result->maximum = 255;
            return result;
        }
        const auto convertTyped = [&]<typename T>() -> bool {
            if constexpr (std::is_integral_v<T>) {
                T lo = std::numeric_limits<T>::max(), hi = std::numeric_limits<T>::lowest();
                if (result->mode == 0) {
                    const bool contiguousRow = strideX == sizeof(T);
                    for (std::size_t y = 0; y < height; ++y) {
                        if (cancelled()) return false;
                        const auto *srcRow = planes[0] + y * strideY;
                        auto *dstRow = destination + y * width * sizeof(float);
                        if (contiguousRow) {
                            for (std::size_t x = 0; x < width; ++x) {
                                T value;
                                std::memcpy(&value, srcRow + x * sizeof(T), sizeof(T));
                                if (value < lo) lo = value;
                                if (value > hi) hi = value;
                                const float f = static_cast<float>(value);
                                std::memcpy(dstRow + x * sizeof(float), &f, sizeof(float));
                            }
                        } else {
                            for (std::size_t x = 0; x < width; ++x) {
                                T value;
                                std::memcpy(&value, srcRow + x * strideX, sizeof(T));
                                if (value < lo) lo = value;
                                if (value > hi) hi = value;
                                const float f = static_cast<float>(value);
                                std::memcpy(dstRow + x * sizeof(float), &f, sizeof(float));
                            }
                        }
                    }
                } else {
                    for (std::size_t y = 0; y < height; ++y) {
                        if (cancelled()) return false;
                        auto *dstRow = destination + y * width * 4 * sizeof(float);
                        for (std::size_t x = 0; x < width; ++x) {
                            float pixel[4]{0, 0, 0, 0};
                            for (int c = 0; c < result->channels; ++c) {
                                T value;
                                std::memcpy(&value, planes[std::size_t(c)] + y * strideY + x * strideX, sizeof(T));
                                if (value < lo) lo = value;
                                if (value > hi) hi = value;
                                pixel[c] = static_cast<float>(value);
                            }
                            std::memcpy(dstRow + x * 4 * sizeof(float), pixel, sizeof(pixel));
                        }
                    }
                }
                result->minimum = double(lo);
                result->maximum = double(hi);
            } else if constexpr (std::is_same_v<T, float>) {
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                if (result->mode == 0) {
                    const bool contiguousRow = strideX == sizeof(float);
                    for (std::size_t y = 0; y < height; ++y) {
                        if (cancelled()) return false;
                        const auto *srcRow = planes[0] + y * strideY;
                        auto *dstRow = destination + y * width * sizeof(float);
                        for (std::size_t x = 0; x < width; ++x) {
                            float value;
                            std::memcpy(&value, srcRow + (contiguousRow ? x * sizeof(float) : x * strideX), sizeof(float));
                            if (std::isfinite(value)) {
                                if (double(value) < lo) lo = double(value);
                                if (double(value) > hi) hi = double(value);
                            } else value = 0.0f;
                            std::memcpy(dstRow + x * sizeof(float), &value, sizeof(float));
                        }
                    }
                } else {
                    for (std::size_t y = 0; y < height; ++y) {
                        if (cancelled()) return false;
                        auto *dstRow = destination + y * width * 4 * sizeof(float);
                        for (std::size_t x = 0; x < width; ++x) {
                            float pixel[4]{0, 0, 0, 0};
                            for (int c = 0; c < result->channels; ++c) {
                                float value;
                                std::memcpy(&value, planes[std::size_t(c)] + y * strideY + x * strideX, sizeof(float));
                                if (std::isfinite(value)) {
                                    if (double(value) < lo) lo = double(value);
                                    if (double(value) > hi) hi = double(value);
                                    pixel[c] = value;
                                }
                            }
                            std::memcpy(dstRow + x * 4 * sizeof(float), pixel, sizeof(pixel));
                        }
                    }
                }
                result->minimum = lo;
                result->maximum = hi;
            } else {
                double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                for (std::size_t y = 0; y < height; ++y) {
                    if (cancelled()) return false;
                    for (int c = 0; c < result->channels; ++c) {
                        const auto *srcRow = planes[std::size_t(c)] + y * strideY;
                        for (std::size_t x = 0; x < width; ++x) {
                            double value;
                            std::memcpy(&value, srcRow + x * strideX, sizeof(double));
                            if (std::isfinite(value)) {
                                if (value < lo) lo = value;
                                if (value > hi) hi = value;
                            }
                        }
                    }
                }
                result->minimum = lo;
                result->maximum = hi;
                finalizeRange();
                result->offset = result->minimum;
                result->scale = result->maximum - result->minimum;
                if (!std::isfinite(result->scale)) {
                    result->offset = 0;
                    result->scale = std::max(std::abs(result->minimum), std::abs(result->maximum));
                }
                const double offset = result->offset, scale = result->scale;
                const float zeroEncoded = float((0.0 - offset) / scale);
                for (std::size_t y = 0; y < height; ++y) {
                    if (cancelled()) return false;
                    if (result->mode == 0) {
                        const auto *srcRow = planes[0] + y * strideY;
                        auto *dstRow = destination + y * width * sizeof(float);
                        for (std::size_t x = 0; x < width; ++x) {
                            double value;
                            std::memcpy(&value, srcRow + x * strideX, sizeof(double));
                            const float encoded = std::isfinite(value) ? float((value - offset) / scale) : zeroEncoded;
                            std::memcpy(dstRow + x * sizeof(float), &encoded, sizeof(float));
                        }
                    } else {
                        auto *dstRow = destination + y * width * 4 * sizeof(float);
                        for (std::size_t x = 0; x < width; ++x) {
                            float pixel[4]{zeroEncoded, zeroEncoded, zeroEncoded, zeroEncoded};
                            for (int c = 0; c < result->channels; ++c) {
                                double value;
                                std::memcpy(&value, planes[std::size_t(c)] + y * strideY + x * strideX, sizeof(double));
                                if (std::isfinite(value)) pixel[c] = float((value - offset) / scale);
                            }
                            std::memcpy(dstRow + x * 4 * sizeof(float), pixel, sizeof(pixel));
                        }
                    }
                }
                return true;
            }
            finalizeRange();
            return true;
        };
        bool finished = false;
        switch (image.pixelType()) {
        case PixelType::UInt8: finished = convertTyped.template operator()<std::uint8_t>(); break;
        case PixelType::UInt16: finished = convertTyped.template operator()<std::uint16_t>(); break;
        case PixelType::Int16: finished = convertTyped.template operator()<std::int16_t>(); break;
        case PixelType::Float32: finished = convertTyped.template operator()<float>(); break;
        case PixelType::Float64: finished = convertTyped.template operator()<double>(); break;
        default: break;
        }
        return finished ? result : nullptr;
    }

    for (std::size_t y = 0; y < height; ++y) {
        if (cancelled()) return {};
        for (std::size_t x = 0; x < width; ++x) {
            const auto offset = (y * width + x) * components;
            for (std::size_t c = 0; c < components; ++c) {
                double value = color && c == 3 && sourceComponents == 3 ? 255.0
                    : color ? image.sample(x, y, key.z, key.channel, key.time, c)
                    : c < std::size_t(result->channels) ? image.sample(x, y, key.z, result->mode == 2 ? c : key.channel, key.time) : 0.0;
                if (c < std::size_t(color ? 3 : result->channels) && std::isfinite(value)) {
                    result->minimum = std::min(result->minimum, value);
                    result->maximum = std::max(result->maximum, value);
                }
                if (!std::isfinite(value)) value = 0;
                if (color) {
                    const auto displayValue = std::uint8_t(std::clamp(value, 0.0, 255.0));
                    std::memcpy(destination + offset + c, &displayValue, sizeof(displayValue));
                } else if (lazyDouble) {
                    std::memcpy(destination + (offset + c) * sizeof(double), &value, sizeof(double));
                } else {
                    const float displayValue = float(std::clamp(value,
                        -double(std::numeric_limits<float>::max()), double(std::numeric_limits<float>::max())));
                    std::memcpy(destination + (offset + c) * sizeof(float), &displayValue, sizeof(float));
                }
            }
        }
    }
    if (cancelled()) return {};
    finalizeRange();
    if (lazyDouble) {
        result->offset = result->minimum;
        result->scale = result->maximum - result->minimum;
        if (!std::isfinite(result->scale)) {
            result->offset = 0;
            result->scale = std::max(std::abs(result->minimum), std::abs(result->maximum));
        }
        const auto values = width * height * components;
        for (std::size_t pixel = 0; pixel < values; ++pixel) {
            double value;
            std::memcpy(&value, result->pixels.constData() + pixel * sizeof(double), sizeof(double));
            const float encoded = float((value - result->offset) / result->scale);
            std::memcpy(result->pixels.data() + pixel * sizeof(float), &encoded, sizeof(float));
        }
        result->pixels.resize(qsizetype(values * sizeof(float)));
        result->pixels.squeeze();
    }
    return result;
}

QImage renderSliceImage(const Image &image, std::size_t z, std::size_t channel, std::size_t time,
                        double windowMinimum, double windowMaximum, double gamma, int lut,
                        bool inverted, bool composite, const Image &mask)
{
    if (image.empty()) return {};
    constexpr std::size_t budget = 512 * 1024 * 1024;
    if (image.width() > std::size_t(std::numeric_limits<int>::max())
        || image.height() > std::size_t(std::numeric_limits<int>::max())
        || image.width() > budget / 4 / image.height())
        throw std::length_error(QCoreApplication::translate("imagecxx::ImageViewer", "The print image exceeds the 512 MiB display budget.").toStdString());
    QImage result(int(image.width()), int(image.height()), QImage::Format_ARGB32);
    if (result.isNull())
        throw std::runtime_error(QCoreApplication::translate("imagecxx::ImageViewer", "Cannot allocate print image.").toStdString());
    const auto components = componentCount(image.pixelType());
    const auto normalize = [&](double raw) {
        const double range = windowMaximum - windowMinimum;
        double value = std::isfinite(raw) ? (std::isfinite(range) ? (raw - windowMinimum) / range
            : (raw / 2 - windowMinimum / 2) / (windowMaximum / 2 - windowMinimum / 2)) : 0;
        value = std::pow(std::clamp(std::isfinite(value) ? value : 0., 0., 1.), 1. / gamma);
        return inverted ? 1. - value : value;
    };
    constexpr std::array<QRgb, 4> defaults{0xffff4545, 0xff55e887, 0xff529bff, 0xffeeeeee};
    constexpr std::array<std::array<double, 3>, 6> palette{{{.267, .005, .329}, {.254, .265, .530},
        {.164, .471, .558}, {.135, .659, .518}, {.478, .821, .318}, {.993, .906, .144}}};
    for (int y = 0; y < result.height(); ++y) for (int x = 0; x < result.width(); ++x) {
        std::array<double, 3> color{};
        double alpha = 1;
        if (components >= 3) {
            for (std::size_t c = 0; c < 3; ++c) color[c] = normalize(image.sample(x, y, z, channel, time, c));
            if (components == 4) alpha = std::clamp(image.sample(x, y, z, channel, time, 3) / 255., 0., 1.);
        } else if (composite && image.channels() > 1) {
            for (std::size_t c = 0; c < std::min<std::size_t>(4, image.channels()); ++c) {
                const double value = normalize(image.sample(x, y, z, c, time));
                const auto tint = QColor::fromRgba(c < image.metadata().channels.size() ? image.metadata().channels[c].color : defaults[c]);
                color[0] += value * tint.redF();
                color[1] += value * tint.greenF();
                color[2] += value * tint.blueF();
            }
        } else {
            const double value = normalize(image.sample(x, y, z, channel, time));
            if (lut == 2) {
                const double position = value * 5;
                const auto index = std::min<std::size_t>(std::size_t(position), 4);
                for (std::size_t c = 0; c < 3; ++c) color[c] = std::lerp(palette[index][c], palette[index + 1][c], position - index);
            } else if (lut == 1) color = {3 * value, 3 * value - 1, 3 * value - 2};
            else color.fill(value);
        }
        for (auto &value : color) value = std::clamp(value, 0., 1.);
        if (!mask.empty() && mask.sample(x, y) != 0) {
            constexpr std::array<double, 3> tint{.95, .25, .34};
            for (std::size_t c = 0; c < 3; ++c) color[c] = std::lerp(color[c], tint[c], .36);
        }
        result.setPixelColor(x, y, QColor::fromRgbF(color[0], color[1], color[2], alpha));
    }
    return result;
}

} // namespace detail
} // namespace imagecxx
