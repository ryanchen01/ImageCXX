#include "processing/processing.h"
#include "core/memory.h"
#include <itkMultiThreaderBase.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <type_traits>

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
        const auto *buffer = image.data();
        const auto &strides = image.strides();
        const double zExtent = double(image.depth() - 1) * options.sliceSpacing;
        const double radius = options.rotationAxis == 2 ? zExtent
            : std::hypot(double(image.dimension(options.rotationAxis == 0 ? 1 : 0) - 1), zExtent);
        const std::array<double, 3> center{double(image.width() - 1)/2, double(image.height() - 1)/2, double(image.depth() - 1)/2};
        const std::array<double, 3> halfSize{double(image.width())/2, double(image.height())/2, double(image.depth()) * options.sliceSpacing/2};
        const double outputCenterX = center[0] + std::ceil(double(output.width() - image.width())/2);
        const double outputCenterY = center[1] + std::ceil(double(output.height() - image.height())/2);
        auto *destination = output.mutableData();
        const auto pixelBytes = bytesPerPixel(image.pixelType());
        const auto rows = output.pixelCount() / output.width() * components;
        std::size_t completed = 0;
        std::mutex progressMutex;
        std::exception_ptr error;
        std::atomic<bool> failed = false;
        if (progress) progress(0);
        auto threader = itk::MultiThreaderBase::New();
        // Storage providers need not support concurrent reads without a shared buffer.
        if (!buffer) {
            threader->SetMaximumNumberOfThreads(1);
            threader->SetNumberOfWorkUnits(1);
        }
        threader->ParallelizeArray(0, rows, [&](std::size_t row) {
            try {
                if (failed.load()) return;
                if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
                auto remainder = row;
                const auto y = remainder % output.height(); remainder /= output.height();
                const auto k = remainder % components; remainder /= components;
                const auto angle = remainder % output.depth(); remainder /= output.depth();
                const auto c = remainder % image.channels();
                const auto t = remainder / image.channels();
                const auto degrees = options.initialAngle + angle * options.angleIncrement;
                const double radians = degrees * std::numbers::pi / 180.;
                double cosine = std::cos(radians), sine = std::sin(radians);
                // Avoid floating-point drift across pixel boundaries in cardinal views.
                if (degrees % 90 == 0) { cosine = std::round(cosine); sine = std::round(sine); }
                const auto volumeOffset = (strides.size() > 3 ? c * strides[3] : 0)
                    + (strides.size() > 4 ? t * strides[4] : 0) + (components > 1 ? k : 0);
                const auto read = [&](std::size_t x, std::size_t y, std::size_t z) -> double {
                    if (!buffer) return image.sample(x, y, z, c, t, k);
                    const auto *at = buffer + volumeOffset + x * strides[0] + y * strides[1] + z * strides[2];
                    // Views and mapped files can be unaligned; memcpy also avoids aliasing typed storage.
                    switch (image.pixelType()) {
                    case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8:
                        return std::to_integer<std::uint8_t>(*at);
                    case PixelType::UInt16: { std::uint16_t value; std::memcpy(&value, at, sizeof(value)); return value; }
                    case PixelType::Int16: { std::int16_t value; std::memcpy(&value, at, sizeof(value)); return value; }
                    case PixelType::Float32: { float value; std::memcpy(&value, at, sizeof(value)); return value; }
                    case PixelType::Float64: { double value; std::memcpy(&value, at, sizeof(value)); return value; }
                    }
                    throw std::invalid_argument("Unknown pixel type");
                };
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
                        if ((sample & 4095) == 0 && (stop.stop_requested() || failed.load())) throw std::runtime_error("3D projection cancelled");
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
                        if (!options.interpolate) value = read(low[0], low[1], low[2]);
                        else for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                            const double weight = (dx ? fraction[0] : 1-fraction[0]) * (dy ? fraction[1] : 1-fraction[1]) * (dz ? fraction[2] : 1-fraction[2]);
                            if (weight > 0) value += weight * read(dx ? high[0] : low[0], dy ? high[1] : low[1], dz ? high[2] : low[2]);
                        }
                        if (!std::isfinite(value) || value < options.lower || value > options.upper) continue;
                        if (!pixel.count) {
                            pixel.nearestDepth = std::clamp(.5 + depthPosition / radius, 0., 1.);
                            pixel.surface = value;
                        }
                        if (options.method == ProjectionMethod::BrightestPoint && value > pixel.brightest) {
                            pixel.brightest = value;
                            pixel.brightestDepth = std::clamp(.5 + depthPosition / radius, 0., 1.);
                        }
                        ++pixel.count;
                        if (options.method == ProjectionMethod::NearestPoint) break;
                        if (options.method == ProjectionMethod::MeanValue)
                            pixel.mean = std::lerp(pixel.mean, value, 1. / pixel.count);
                    }
                    if (!pixel.count) continue;
                    const double surface = pixel.surface * (1 - (k == 3 ? 0 : options.surfaceDepthCue) / 100. * pixel.nearestDepth);
                    double value = surface;
                    if (options.method != ProjectionMethod::NearestPoint) {
                        value = options.method == ProjectionMethod::MeanValue ? pixel.mean
                            : pixel.brightest * (1 - (k == 3 ? 0 : options.interiorDepthCue) / 100. * pixel.brightestDepth);
                        value = std::lerp(value, surface, options.opacity / 100.);
                    }
                    const auto offset = (((t * image.channels() + c) * output.depth() + angle) * output.height() + y)
                        * output.width() * pixelBytes + x * pixelBytes + (components > 1 ? k : 0);
                    const auto write = [&]<typename T>() {
                        double converted = value;
                        if constexpr (std::is_integral_v<T>)
                            converted = std::isnan(converted) ? 0 : std::round(std::clamp(converted,
                                double(std::numeric_limits<T>::lowest()), double(std::numeric_limits<T>::max())));
                        const T number = static_cast<T>(converted);
                        std::memcpy(destination + offset, &number, sizeof(T));
                    };
                    switch (image.pixelType()) {
                    case PixelType::UInt8: case PixelType::RGB8: case PixelType::RGBA8: write.template operator()<std::uint8_t>(); break;
                    case PixelType::UInt16: write.template operator()<std::uint16_t>(); break;
                    case PixelType::Int16: write.template operator()<std::int16_t>(); break;
                    case PixelType::Float32: write.template operator()<float>(); break;
                    case PixelType::Float64: write.template operator()<double>(); break;
                    }
                }
                const std::lock_guard guard(progressMutex);
                ++completed;
                if (progress && !failed.load() && !stop.stop_requested()) progress(double(completed) / double(rows));
            } catch (...) {
                const std::lock_guard guard(progressMutex);
                if (!error) error = std::current_exception();
                failed.store(true);
            }
        }, nullptr);
        if (error) std::rethrow_exception(error);
        if (stop.stop_requested()) throw std::runtime_error("3D projection cancelled");
        if (progress) progress(1);
        return output;
    } catch (const std::bad_alloc &) {
        throw std::runtime_error("Memory allocation failed during 3D projection. The source image is unchanged.");
    }
}
} // namespace imagecxx
