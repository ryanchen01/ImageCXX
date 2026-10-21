#include "processing/processing.h"
#include "core/memory.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>

namespace imagecxx {
namespace {
struct ProjectedPixel {
    double nearestDepth = std::numeric_limits<double>::infinity(), surface = 0;
    double brightest = -std::numeric_limits<double>::infinity(), brightestDepth = 0, mean = 0;
    std::size_t count = 0;
};

std::size_t projectionSize(double value) {
    if (!std::isfinite(value) || value < 1 || value > std::numeric_limits<int>::max())
        throw std::overflow_error("3D projection dimensions exceed the supported size");
    return static_cast<std::size_t>(std::ceil(value));
}
}

ProjectionGeometry projectionGeometry(const Image &image, const ProjectionOptions &options) {
    if (image.empty() || image.depth() < 2 || image.shape().size() > 5)
        throw std::invalid_argument("3D Project requires a Z stack with at least two slices and at most five axes");
    if (options.method != ProjectionMethod::NearestPoint && options.method != ProjectionMethod::BrightestPoint
        && options.method != ProjectionMethod::MeanValue)
        throw std::invalid_argument("Invalid projection method");
    if (options.rotationAxis < 0 || options.rotationAxis > 2 || !std::isfinite(options.sliceSpacing) || options.sliceSpacing <= 0
        || options.initialAngle < 0 || options.initialAngle > 359 || options.totalRotation < 0 || options.totalRotation > 360
        || options.angleIncrement < 1 || options.angleIncrement > 360)
        throw std::invalid_argument("Invalid rotation axis, spacing or angles");
    if (!std::isfinite(options.lower) || !std::isfinite(options.upper) || options.lower > options.upper
        || options.opacity < 0 || options.opacity > 100 || options.surfaceDepthCue < 0 || options.surfaceDepthCue > 100
        || options.interiorDepthCue < 0 || options.interiorDepthCue > 100)
        throw std::invalid_argument("Invalid transparency bounds, opacity or depth cueing");
    const double depth = double(image.depth()) * options.sliceSpacing;
    projectionSize(std::max(depth, 1.));
    projectionSize(double(image.width())); projectionSize(double(image.height()));
    ProjectionGeometry geometry;
    geometry.shape = image.shape();
    geometry.shape[0] = options.rotationAxis == 0 ? image.width() : projectionSize(std::hypot(double(image.width()),
        options.rotationAxis == 1 ? depth : double(image.height())));
    geometry.shape[1] = options.rotationAxis == 1 ? image.height() : projectionSize(std::hypot(double(image.height()),
        options.rotationAxis == 0 ? depth : double(image.width())));
    geometry.shape[2] = options.totalRotation == 360 ? (359 / options.angleIncrement + 1)
        : (options.totalRotation / options.angleIncrement + 1);
    geometry.byteCount = bytesPerPixel(image.pixelType());
    for (auto length : geometry.shape) {
        if (geometry.byteCount > std::numeric_limits<std::size_t>::max() / length)
            throw std::overflow_error("3D projection buffer exceeds the addressable size");
        geometry.byteCount *= length;
    }
    const auto plane = geometry.shape[0] * geometry.shape[1];
    if (plane > (std::numeric_limits<std::size_t>::max() - geometry.byteCount) / sizeof(ProjectedPixel))
        throw std::overflow_error("3D projection working memory exceeds the addressable size");
    geometry.requiredMemoryBytes = geometry.byteCount + plane * sizeof(ProjectedPixel);
    return geometry;
}

Image project3D(const Image &image, const ProjectionOptions &options, std::stop_token stop, Progress progress) {
    static std::mutex allocationMutex;
    const std::unique_lock lock(allocationMutex);
    if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
    const auto geometry = projectionGeometry(image, options);
    detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
    try {
        auto metadata = image.metadata();
        metadata.name += QStringLiteral(" · 3D Projection"); metadata.sourcePath.clear(); metadata.direction.clear();
        metadata.axes[0] = {"X", 1, 0, "px"}; metadata.axes[1] = {"Y", 1, 0, "px"};
        metadata.axes[2] = {"Angle", double(options.angleIncrement), double(options.initialAngle), "degrees"};
        metadata.attributes.insert("ImageCXX.LastOperation", "3D Project");
        metadata.attributes.remove("ImageCXX.EmbeddedPlaneOrigin"); metadata.attributes.remove("ImageCXX.EmbeddedPlaneZUnit");
        auto output = Image::allocate(image.pixelType(), geometry.shape, metadata);
        std::vector<ProjectedPixel> pixels(geometry.shape[0] * geometry.shape[1]);
        const auto components = componentCount(image.pixelType());
        const auto samples = options.interpolate && options.sliceSpacing > 1
            ? projectionSize(std::ceil(double(image.depth() - 1) * options.sliceSpacing) + 1) : image.depth();
        const double zExtent = double(image.depth() - 1) * options.sliceSpacing;
        const double radius = options.rotationAxis == 2 ? std::max(zExtent, 1.)
            : std::hypot(double(image.dimension(options.rotationAxis == 0 ? 1 : 0) - 1), zExtent);
        const double units = double(output.depth()) * image.channels() * image.frames() * components;
        double completed = 0;
        if (progress) progress(0);
        std::vector<std::size_t> index(output.shape().size(), 0);
        for (std::size_t t = 0; t < image.frames(); ++t) for (std::size_t c = 0; c < image.channels(); ++c)
            for (std::size_t angle = 0; angle < output.depth(); ++angle) for (std::size_t k = 0; k < components; ++k) {
                std::fill(pixels.begin(), pixels.end(), ProjectedPixel{});
                const auto degrees = options.initialAngle + angle * options.angleIncrement;
                const double radians = degrees * std::numbers::pi / 180.;
                double cosine = std::cos(radians), sine = std::sin(radians);
                // Avoid floating-point drift across pixel boundaries in cardinal views.
                if (degrees % 90 == 0) { cosine = std::round(cosine); sine = std::round(sine); }
                for (std::size_t z = 0; z < samples; ++z) {
                    const double sourceZ = samples == image.depth() ? double(z)
                        : double(z) * double(image.depth() - 1) / double(samples - 1);
                    const auto z0 = std::size_t(std::floor(sourceZ)), z1 = std::min(z0 + 1, image.depth() - 1);
                    const double fraction = sourceZ - z0;
                    for (std::size_t y = 0; y < image.height(); ++y) {
                        if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
                        if (progress && y == 0) progress((completed + double(z) / samples) / units);
                        for (std::size_t x = 0; x < image.width(); ++x) {
                            double value = image.sample(x, y, z0, c, t, k);
                            if (fraction > 0) value = (1 - fraction) * value + fraction * image.sample(x, y, z1, c, t, k);
                            if (!std::isfinite(value) || value < options.lower || value > options.upper) continue;
                            const double sx = double(x) - double(image.width() - 1) / 2;
                            const double sy = double(y) - double(image.height() - 1) / 2;
                            const double sz = sourceZ * options.sliceSpacing - zExtent / 2;
                            double px = sx, py = sy, pz = sz;
                            if (options.rotationAxis == 0) { py = cosine * sy - sine * sz; pz = sine * sy + cosine * sz; }
                            if (options.rotationAxis == 1) { px = cosine * sx + sine * sz; pz = -sine * sx + cosine * sz; }
                            if (options.rotationAxis == 2) { px = cosine * sx - sine * sy; py = sine * sx + cosine * sy; }
                            const auto ox = std::lround(px + double(output.width() - 1) / 2);
                            const auto oy = std::lround(py + double(output.height() - 1) / 2);
                            if (ox < 0 || oy < 0 || std::size_t(ox) >= output.width() || std::size_t(oy) >= output.height()) continue;
                            auto &pixel = pixels[std::size_t(oy) * output.width() + std::size_t(ox)];
                            const double depth = std::clamp(.5 + pz / radius, 0., 1.);
                            if (depth < pixel.nearestDepth) { pixel.nearestDepth = depth; pixel.surface = value; }
                            if (value > pixel.brightest || (value == pixel.brightest && depth < pixel.brightestDepth)) {
                                pixel.brightest = value; pixel.brightestDepth = depth;
                            }
                            ++pixel.count; pixel.mean = std::lerp(pixel.mean, value, 1. / pixel.count);
                        }
                    }
                }
                if (index.size() > 3) index[3] = c;
                if (index.size() > 4) index[4] = t;
                index[2] = angle;
                for (std::size_t y = 0; y < output.height(); ++y) {
                    if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
                    for (std::size_t x = 0; x < output.width(); ++x) {
                        const auto &pixel = pixels[y * output.width() + x];
                        if (!pixel.count) continue;
                        const double surface = pixel.surface * (1 - (k == 3 ? 0 : options.surfaceDepthCue) / 100. * pixel.nearestDepth);
                        double value = options.method == ProjectionMethod::MeanValue ? pixel.mean
                            : pixel.brightest * (1 - (k == 3 ? 0 : options.interiorDepthCue) / 100. * pixel.brightestDepth);
                        value = options.method == ProjectionMethod::NearestPoint ? surface : std::lerp(value, surface, options.opacity / 100.);
                        index[0] = x; index[1] = y; output.setValue(index, value, k);
                    }
                }
                ++completed;
            }
        if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
        if (progress) progress(1);
        return output;
    } catch (const std::bad_alloc &) {
        throw std::runtime_error("Memory allocation failed during 3D projection. The source image is unchanged.");
    }
}
} // namespace imagecxx
