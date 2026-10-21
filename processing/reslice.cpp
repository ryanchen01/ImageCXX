#include "memory.h"
#include "processing/processing.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace imagecxx {
namespace {
template <typename T>
Image sampleReslice(const Image &source, const ResliceGeometry &geometry, std::stop_token stop,
                    const Progress &progress) {
    if (stop.stop_requested())
        throw std::runtime_error("Reslice cancelled");
    auto result = Image::allocate(source.pixelType(), geometry.shape, geometry.metadata);
    auto *destination = result.mutableData();
    const auto *buffer = source.data();
    const auto components = componentCount(source.pixelType());
    const auto pixelBytes = bytesPerPixel(source.pixelType());
    std::vector<std::size_t> outputIndex(geometry.shape.size(), 0);
    std::vector<std::size_t> inputIndex(source.shape().size(), 0);
    const auto count = result.pixelCount();
    for (std::size_t pixel = 0; pixel < count; ++pixel) {
        if ((pixel & 4095) == 0) {
            if (stop.stop_requested())
                throw std::runtime_error("Reslice cancelled");
            if (progress)
                progress(double(pixel) / double(count));
        }
        for (std::size_t axis = 3; axis < inputIndex.size(); ++axis)
            inputIndex[axis] = outputIndex[axis];
        std::array<double, 3> fraction{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto inputAxis = geometry.sourceAxes[axis];
            const double position = std::clamp(geometry.start[axis] + outputIndex[axis] * geometry.step[axis],
                                               0.0, double(source.dimension(inputAxis) - 1));
            inputIndex[inputAxis] = std::size_t(std::floor(position));
            fraction[inputAxis] = position - double(inputIndex[inputAxis]);
        }
        std::size_t offset = 0;
        for (std::size_t axis = 0; axis < inputIndex.size(); ++axis)
            offset += inputIndex[axis] * source.strides()[axis];
        // Preserve all bits at exact samples, including floating NaNs and RGBA alpha.
        if (buffer && fraction == std::array<double, 3>{}) {
            std::memcpy(destination + pixel * pixelBytes, buffer + offset, pixelBytes);
        } else {
            for (std::size_t component = 0; component < components; ++component) {
                long double value = 0;
                for (unsigned corner = 0; corner < 8; ++corner) {
                    long double weight = 1;
                    auto index = inputIndex;
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const bool upper = (corner & (1u << axis)) != 0;
                        weight *= upper ? fraction[axis] : 1 - fraction[axis];
                        if (upper)
                            ++index[axis];
                    }
                    if (weight == 0)
                        continue; // Do not contaminate exact samples with adjacent NaNs.
                    double sample;
                    if (buffer) {
                        std::size_t at = component * sizeof(T);
                        for (std::size_t axis = 0; axis < index.size(); ++axis)
                            at += index[axis] * source.strides()[axis];
                        T stored;
                        std::memcpy(&stored, buffer + at, sizeof(T));
                        sample = double(stored);
                    } else
                        sample = source.value(index, component);
                    value += weight * sample;
                }
                if constexpr (std::is_integral_v<T>)
                    value = std::round(std::clamp(value,
                                                  static_cast<long double>(std::numeric_limits<T>::lowest()),
                                                  static_cast<long double>(std::numeric_limits<T>::max())));
                const T converted = static_cast<T>(value);
                std::memcpy(destination + pixel * pixelBytes + component * sizeof(T), &converted, sizeof(T));
            }
        }
        for (std::size_t axis = 0; axis < outputIndex.size(); ++axis) {
            if (++outputIndex[axis] < geometry.shape[axis])
                break;
            outputIndex[axis] = 0;
        }
    }
    if (stop.stop_requested())
        throw std::runtime_error("Reslice cancelled");
    if (progress)
        progress(1);
    return result;
}
} // namespace

ResliceGeometry resliceGeometry(const Image &image, const ResliceOptions &options) {
    if (image.empty() || image.shape().size() < 3 || image.depth() < 2)
        throw std::invalid_argument("Reslice requires a Z stack with at least two slices");
    if (!std::isfinite(options.outputSpacing) || options.outputSpacing <= 0)
        throw std::invalid_argument("Output spacing must be positive and finite");
    if (options.start != ResliceStart::Top && options.start != ResliceStart::Left &&
        options.start != ResliceStart::Bottom && options.start != ResliceStart::Right)
        throw std::invalid_argument("Invalid reslice start direction");
    const auto &axes = image.metadata().axes;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(axes[axis].spacing) || axes[axis].spacing <= 0 ||
            !std::isfinite(axes[axis].origin))
            throw std::invalid_argument("Spatial calibration must be finite with positive spacing");
        if (axes[axis].unit != axes[0].unit)
            throw std::invalid_argument(
                "Reslice requires matching spatial axis units. Convert the image's spatial units first.");
    }
    auto direction = image.metadata().direction;
    if (direction.empty())
        direction = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (direction.size() != 9 ||
        !std::all_of(direction.begin(), direction.end(), [](double v) { return std::isfinite(v); }))
        throw std::invalid_argument("Reslice requires a finite 3D spatial direction matrix");
    const double determinant = direction[0] * (direction[4] * direction[8] - direction[5] * direction[7]) -
                               direction[1] * (direction[3] * direction[8] - direction[5] * direction[6]) +
                               direction[2] * (direction[3] * direction[7] - direction[4] * direction[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        throw std::invalid_argument("Spatial image direction is singular");

    const bool horizontal = options.start == ResliceStart::Top || options.start == ResliceStart::Bottom;
    const std::size_t profile = horizontal ? 0 : 1, sweep = horizontal ? 1 : 0;
    ResliceGeometry geometry;
    geometry.sourceAxes = {profile, 2, sweep};
    geometry.start = {0, options.flipVertically ? double(image.depth() - 1) : 0, 0};
    geometry.step = {1, options.avoidInterpolation ? 1 : axes[profile].spacing / axes[2].spacing,
                     options.avoidInterpolation ? 1 : options.outputSpacing / axes[sweep].spacing};
    if (options.flipVertically)
        geometry.step[1] *= -1;
    if (options.start == ResliceStart::Bottom || options.start == ResliceStart::Right) {
        geometry.start[2] = double(image.dimension(sweep) - 1);
        geometry.step[2] *= -1;
    }
    if (options.rotate90) {
        std::swap(geometry.sourceAxes[0], geometry.sourceAxes[1]);
        std::swap(geometry.start[0], geometry.start[1]);
        std::swap(geometry.step[0], geometry.step[1]);
    }
    geometry.shape = image.shape();
    geometry.metadata = image.metadata();
    geometry.metadata.direction.resize(9);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto sourceAxis = geometry.sourceAxes[axis];
        const double step = std::abs(geometry.step[axis]);
        const double intervals = std::floor(double(image.dimension(sourceAxis) - 1) / step + 1e-9);
        if (!std::isfinite(step) || step <= 0 || !std::isfinite(intervals) ||
            intervals >= double(std::numeric_limits<std::size_t>::max() - 1))
            throw std::overflow_error("Resliced axis exceeds the addressable size");
        geometry.shape[axis] = std::size_t(intervals) + 1;
        geometry.metadata.axes[axis] = axes[sourceAxis];
        geometry.metadata.axes[axis].spacing *= step;
        if (!std::isfinite(geometry.metadata.axes[axis].spacing) || geometry.metadata.axes[axis].spacing <= 0)
            throw std::overflow_error("Resliced spacing is not finite and positive");
        for (std::size_t row = 0; row < 3; ++row)
            geometry.metadata.direction[row * 3 + axis] =
                direction[row * 3 + sourceAxis] * (geometry.step[axis] < 0 ? -1 : 1);
    }
    // Origins are world-coordinate components, not permuted voxel-axis offsets.
    for (std::size_t row = 0; row < 3; ++row) {
        double origin = axes[row].origin;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto sourceAxis = geometry.sourceAxes[axis];
            origin += direction[row * 3 + sourceAxis] * geometry.start[axis] * axes[sourceAxis].spacing;
        }
        if (!std::isfinite(origin))
            throw std::overflow_error("Resliced spatial origin is not finite");
        geometry.metadata.axes[row].origin = origin;
    }
    geometry.byteCount = bytesPerPixel(image.pixelType());
    for (const auto size : geometry.shape) {
        if (size > std::numeric_limits<std::size_t>::max() / geometry.byteCount)
            throw std::overflow_error("Resliced image exceeds the addressable buffer size");
        geometry.byteCount *= size;
    }
    // The sampler allocates no full-volume intermediate. Reserve the result
    // window's decoded cache (its current 128 MiB default), plus four plane
    // buffers for decoding/encoding, upload and texture staging. Non-RAM inputs
    // may also fault their entire source into memory while being read.
    const auto planePixels = geometry.shape[0] * geometry.shape[1];
    const std::size_t decodedBytes =
        componentCount(image.pixelType()) > 1
            ? 4
            : (image.pixelType() == PixelType::Float64 ? 8 : 4) * std::min<std::size_t>(4, image.channels());
    if (planePixels > std::numeric_limits<std::size_t>::max() / decodedBytes / 4)
        throw std::overflow_error("Reslice working buffers exceed the addressable size");
    const auto planeBytes = planePixels * decodedBytes;
    const auto cachePixels = geometry.byteCount / bytesPerPixel(image.pixelType());
    const auto cacheBytes =
        std::min(cachePixels, std::size_t{128} * 1024 * 1024 / decodedBytes) * decodedBytes;
    geometry.requiredMemoryBytes = geometry.byteCount;
    for (const auto extra :
         {planeBytes * 4, cacheBytes, image.storageKind() == "RAM" ? std::size_t{0} : image.byteCount()}) {
        if (extra > std::numeric_limits<std::size_t>::max() - geometry.requiredMemoryBytes)
            throw std::overflow_error("Reslice memory requirement exceeds the addressable size");
        geometry.requiredMemoryBytes += extra;
    }
    geometry.metadata.sourcePath.clear();
    geometry.metadata.name += QStringLiteral(" · Reslice");
    geometry.metadata.attributes.insert(QStringLiteral("ImageCXX.LastOperation"), QStringLiteral("Reslice"));
    return geometry;
}

Image reslice(const Image &image, const ResliceOptions &options, std::stop_token stop, Progress progress) {
    // Serialize reslice allocations: queued jobs must recheck memory after the
    // preceding output exists, rather than both trusting the same free-memory snapshot.
    static std::mutex allocationMutex;
    const std::unique_lock lock(allocationMutex);
    if (stop.stop_requested())
        throw std::runtime_error("Reslice cancelled");
    const auto geometry = resliceGeometry(image, options);
    detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
    try {
        switch (image.pixelType()) {
        case PixelType::UInt8:
        case PixelType::RGB8:
        case PixelType::RGBA8:
            return sampleReslice<std::uint8_t>(image, geometry, stop, progress);
        case PixelType::UInt16:
            return sampleReslice<std::uint16_t>(image, geometry, stop, progress);
        case PixelType::Int16:
            return sampleReslice<std::int16_t>(image, geometry, stop, progress);
        case PixelType::Float32:
            return sampleReslice<float>(image, geometry, stop, progress);
        case PixelType::Float64:
            return sampleReslice<double>(image, geometry, stop, progress);
        }
        throw std::invalid_argument("Unsupported reslice pixel type");
    } catch (const std::bad_alloc &) {
        throw std::runtime_error("Memory allocation failed while reslicing. The source image is unchanged.");
    }
}
} // namespace imagecxx
