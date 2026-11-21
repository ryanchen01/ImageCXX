#pragma once

#include "core/image.h"
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QRhiWidget>
#include <QSize>
#include <QString>
#include <QTransform>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace imagecxx {
namespace detail {

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

struct CanvasLayer {
    std::shared_ptr<const DecodedSlice> frame;
    QTransform transform;
    ImageMetadata metadata;
    double minimum = 0, maximum = 1, gamma = 1, opacity = 1;
    int lut = 0;
    bool inverted = false, linear = false;
    QByteArray mask;
    QSize maskSize;
};

// This is the only class that knows about the private Qt RHI API.
class SliceCanvas final : public QRhiWidget {
public:
    explicit SliceCanvas(QWidget *parent);
    ~SliceCanvas() override;
    std::shared_ptr<const DecodedSlice> frame;
    QRectF imageRect;
    ImageMetadata metadata;
    double minimum = 0, maximum = 1, gamma = 1;
    int lut = 0;
    bool inverted = false, linear = false, overlayMode = false;
    QByteArray mask;
    QSize maskSize;
    QList<CanvasLayer> layers;
    std::size_t gpuBudget = 128 * 1024 * 1024;
    std::size_t gpuBytes = 0;
    std::function<void(const QString &)> failed;

protected:
    void initialize(QRhiCommandBuffer *) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;

private:
    struct GpuState;
    void reportError(const QString &message);
    QString lastError;
    std::unique_ptr<GpuState> gpu;
};

std::shared_ptr<const DecodedSlice> decodeSlice(const Image &image, const SliceKey &key,
                                                std::size_t decodedBudget,
                                                const std::function<bool()> &cancelled);
QImage renderSliceImage(const Image &image, std::size_t z, std::size_t channel, std::size_t time,
                        double windowMinimum, double windowMaximum, double gamma, int lut,
                        bool inverted, bool composite, const Image &mask);

} // namespace detail
} // namespace imagecxx
