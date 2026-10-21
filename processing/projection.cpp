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
    geometry.requiredMemoryBytes = geometry.byteCount;
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
        const auto components = componentCount(image.pixelType());
        const double zExtent = double(image.depth() - 1) * options.sliceSpacing;
        const double radius = options.rotationAxis == 2 ? zExtent
            : std::hypot(double(image.dimension(options.rotationAxis == 0 ? 1 : 0) - 1), zExtent);
        const std::array<double, 3> center{double(image.width() - 1)/2, double(image.height() - 1)/2, double(image.depth() - 1)/2};
        const std::array<double, 3> halfSize{double(image.width())/2, double(image.height())/2, double(image.depth()) * options.sliceSpacing/2};
        const double outputCenterX = center[0] + std::ceil(double(output.width() - image.width())/2);
        const double outputCenterY = center[1] + std::ceil(double(output.height() - image.height())/2);
        const double units = double(output.depth()) * image.channels() * image.frames() * components;
        double completed = 0;
        if (progress) progress(0);
        std::vector<std::size_t> index(output.shape().size(), 0);
        for (std::size_t t = 0; t < image.frames(); ++t) for (std::size_t c = 0; c < image.channels(); ++c)
            for (std::size_t angle = 0; angle < output.depth(); ++angle) for (std::size_t k = 0; k < components; ++k) {
                const auto degrees = options.initialAngle + angle * options.angleIncrement;
                const double radians = degrees * std::numbers::pi / 180.;
                double cosine = std::cos(radians), sine = std::sin(radians);
                // Avoid floating-point drift across pixel boundaries in cardinal views.
                if (degrees % 90 == 0) { cosine = std::round(cosine); sine = std::round(sine); }
                if (index.size() > 3) index[3] = c;
                if (index.size() > 4) index[4] = t;
                index[2] = angle;
                for (std::size_t y = 0; y < output.height(); ++y) {
                    if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
                    if (progress) progress((completed + double(y) / output.height()) / units);
                    for (std::size_t x = 0; x < output.width(); ++x) {
                        const double px = double(x) - outputCenterX, py = double(y) - outputCenterY;
                        std::array<double, 3> origin, direction;
                        if (options.rotationAxis == 0) { origin = {px, cosine*py, -sine*py}; direction = {0, sine, cosine}; }
                        else if (options.rotationAxis == 1) { origin = {cosine*px, py, sine*px}; direction = {-sine, 0, cosine}; }
                        else { origin = {cosine*px + sine*py, -sine*px + cosine*py, 0}; direction = {0, 0, 1}; }
                        double entry = -std::numeric_limits<double>::infinity(), exit = std::numeric_limits<double>::infinity();
                        bool intersects = true;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            if (direction[axis] == 0) {
                                if (origin[axis] < -halfSize[axis] || origin[axis] >= halfSize[axis]) intersects = false;
                            } else {
                                const double a = (-halfSize[axis] - origin[axis]) / direction[axis];
                                const double b = (halfSize[axis] - origin[axis]) / direction[axis];
                                entry = std::max(entry, std::min(a, b)); exit = std::min(exit, std::max(a, b));
                            }
                        }
                        if (!intersects || exit <= entry) continue;
                        // Sample each output ray uniformly; rounding forward-mapped voxels leaves holes and uneven density.
                        const double length = exit - entry;
                        const auto samples = projectionSize(std::max({1., length * std::abs(direction[0]), length * std::abs(direction[1]),
                            length * std::abs(direction[2]) / options.sliceSpacing}));
                        ProjectedPixel pixel;
                        for (std::size_t sample = 0; sample < samples; ++sample) {
                            if ((sample & 4095) == 0 && stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
                            const double depthPosition = std::lerp(entry, exit, (double(sample) + .5) / samples);
                            std::array<std::size_t, 3> low, high;
                            std::array<double, 3> fraction;
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                double coordinate = (origin[axis] + depthPosition * direction[axis]) / (axis == 2 ? options.sliceSpacing : 1) + center[axis];
                                coordinate = std::clamp(coordinate, 0., double(image.dimension(axis) - 1));
                                if (!options.interpolate) coordinate = std::round(coordinate);
                                low[axis] = std::size_t(std::floor(coordinate)); high[axis] = std::min(low[axis] + 1, image.dimension(axis) - 1);
                                fraction[axis] = coordinate - low[axis];
                            }
                            double value = 0;
                            for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                                const double weight = (dx ? fraction[0] : 1-fraction[0]) * (dy ? fraction[1] : 1-fraction[1]) * (dz ? fraction[2] : 1-fraction[2]);
                                if (weight > 0) value += weight * image.sample(dx ? high[0] : low[0], dy ? high[1] : low[1], dz ? high[2] : low[2], c, t, k);
                            }
                            if (!std::isfinite(value) || value < options.lower || value > options.upper) continue;
                            const double depth = std::clamp(.5 + depthPosition / radius, 0., 1.);
                            if (depth < pixel.nearestDepth) { pixel.nearestDepth = depth; pixel.surface = value; }
                            if (value > pixel.brightest) { pixel.brightest = value; pixel.brightestDepth = depth; }
                            ++pixel.count; pixel.mean = std::lerp(pixel.mean, value, 1. / pixel.count);
                        }
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
