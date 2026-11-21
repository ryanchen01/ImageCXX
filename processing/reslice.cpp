#include "core/memory.h"
#include <QCoreApplication>
#include "processing/processing.h"

#include <itkMultiThreaderBase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace imagecxx {
namespace {

struct AxisSample {
    std::size_t low = 0;
    std::size_t high = 0;
    double weight = 0.0;
};

inline AxisSample sampleAxis(double start, double step, std::size_t index, std::size_t srcLen, bool avoidInterpolation) {
    const double pos = start + double(index) * step;
    if (avoidInterpolation) {
        const auto idx = static_cast<std::size_t>(
            std::clamp(std::llround(pos), 0LL, static_cast<long long>(srcLen - 1)));
        return {idx, idx, 0.0};
    }
    const double clamped = std::clamp(pos, 0.0, double(srcLen - 1));
    const auto low = static_cast<std::size_t>(clamped);
    const double frac = clamped - double(low);
    if (low >= srcLen - 1 || frac == 0.0)
        return {std::min(low, srcLen - 1), std::min(low, srcLen - 1), 0.0};
    return {low, low + 1, frac};
}

inline double interp1D(double v0, double v1, double w) {
    return v0 + (v1 - v0) * w;
}

inline double interp2D(double v00, double v01, double v10, double v11, double wFirst, double wSecond) {
    const double a = v00 + (v01 - v00) * wFirst;
    const double b = v10 + (v11 - v10) * wFirst;
    return a + (b - a) * wSecond;
}

template <typename T>
inline T readTyped(const std::byte *at) {
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}

template <typename T>
inline void writeTyped(std::byte *at, double value) {
    if constexpr (std::is_integral_v<T>)
        value = std::round(std::clamp(value, double(std::numeric_limits<T>::lowest()),
                                      double(std::numeric_limits<T>::max())));
    const T converted = static_cast<T>(value);
    std::memcpy(at, &converted, sizeof(T));
}

template <typename T>
Image sampleReslice(const Image &source, const ResliceGeometry &geometry, const ResliceOptions &options,
                    std::stop_token stop, const Progress &progress) {
    if (stop.stop_requested())
        throw std::runtime_error(QCoreApplication::translate("Reslice", "Reslice cancelled").toStdString());
    auto result = Image::allocate(source.pixelType(), geometry.shape, geometry.metadata);
    auto *destination = result.mutableData();
    const auto *buffer = source.data();
    const auto &srcStrides = source.strides();
    const auto components = componentCount(source.pixelType());
    const auto pixelBytes = bytesPerPixel(source.pixelType());
    const auto outW = geometry.shape[0], outH = geometry.shape[1], outD = geometry.shape[2];
    const auto inputVolumePixels = source.width() * source.height() * source.depth();
    const auto volumes = source.pixelCount() / inputVolumePixels;
    const auto totalRows = volumes * outD * outH;
    const auto progressStep = std::max<std::size_t>(1, totalRows / 100);

    const std::size_t sa0 = geometry.sourceAxes[0], sa1 = geometry.sourceAxes[1], sa2 = geometry.sourceAxes[2];
    const std::size_t srcLen0 = source.dimension(sa0), srcLen1 = source.dimension(sa1), srcLen2 = source.dimension(sa2);
    const std::size_t stride0 = srcStrides[sa0], stride1 = srcStrides[sa1], stride2 = srcStrides[sa2];
    const bool identityX = !options.rotate90;
    const bool contiguousCopyX = identityX && stride0 == pixelBytes;

    std::vector<std::size_t> volumeOffsets(volumes, 0);
    std::vector<std::vector<std::size_t>> volumeIndices;
    if (buffer) {
        for (std::size_t volume = 0; volume < volumes; ++volume) {
            auto remainder = volume;
            std::size_t offset = 0;
            for (std::size_t axis = 3; axis < source.shape().size(); ++axis) {
                offset += (remainder % source.dimension(axis)) * srcStrides[axis];
                remainder /= source.dimension(axis);
            }
            volumeOffsets[volume] = offset;
        }
    } else {
        volumeIndices.assign(volumes, std::vector<std::size_t>(source.shape().size(), 0));
        for (std::size_t volume = 0; volume < volumes; ++volume) {
            auto remainder = volume;
            for (std::size_t axis = 3; axis < source.shape().size(); ++axis) {
                volumeIndices[volume][axis] = remainder % source.dimension(axis);
                remainder /= source.dimension(axis);
            }
        }
    }

    std::atomic<std::size_t> completed{0};
    std::mutex progressMutex;
    double reported = 0.0;
    std::exception_ptr error;
    std::atomic<bool> failed{false};
    const auto report = [&](double fraction) {
        if (!progress) return;
        const std::lock_guard lock(progressMutex);
        if (failed.load() || stop.stop_requested()) return;
        reported = std::max(reported, std::clamp(fraction, 0.0, 1.0));
        progress(reported);
    };
    report(0.0);

    auto threader = itk::MultiThreaderBase::New();
    if (!buffer) {
        threader->SetMaximumNumberOfThreads(1);
        threader->SetNumberOfWorkUnits(1);
    }
    threader->ParallelizeArray(0, totalRows, [&](std::size_t row) {
        try {
            if (failed.load()) return;
            if (stop.stop_requested())
                throw std::runtime_error(QCoreApplication::translate("Reslice", "Reslice cancelled").toStdString());
            const std::size_t y = row % outH;
            const std::size_t z = (row / outH) % outD;
            const std::size_t volume = row / (outH * outD);
            auto *dstRow = destination + row * outW * pixelBytes;
            const AxisSample sY = options.rotate90
                ? AxisSample{y, y, 0.0}
                : sampleAxis(geometry.start[1], geometry.step[1], y, srcLen1, options.avoidInterpolation);
            const AxisSample sZ = sampleAxis(geometry.start[2], geometry.step[2], z, srcLen2, options.avoidInterpolation);

            if (buffer) {
                const auto *volBase = buffer + volumeOffsets[volume];
                if (options.avoidInterpolation) {
                    const auto *srcBase = volBase + sY.low * stride1 + sZ.low * stride2;
                    if (contiguousCopyX) {
                        std::memcpy(dstRow, srcBase, outW * pixelBytes);
                    } else if (identityX) {
                        for (std::size_t x = 0; x < outW; ++x)
                            std::memcpy(dstRow + x * pixelBytes, srcBase + x * stride0, pixelBytes);
                    } else {
                        for (std::size_t x = 0; x < outW; ++x) {
                            const auto sX = sampleAxis(geometry.start[0], geometry.step[0], x, srcLen0, true);
                            std::memcpy(dstRow + x * pixelBytes, srcBase + sX.low * stride0, pixelBytes);
                        }
                    }
                } else if (identityX) {
                    // Output axis 0 (X) is the uninterpolated profile axis: weights are row-constant.
                    // Interpolate along source X/Y (sZ) before source Z (sY) to match ITK's axis order.
                    const bool interpY = sY.weight > 0.0;
                    const bool interpZ = sZ.weight > 0.0;
                    const auto *p00 = volBase + sY.low * stride1 + sZ.low * stride2;
                    if (!interpY && !interpZ) {
                        if (contiguousCopyX) {
                            std::memcpy(dstRow, p00, outW * pixelBytes);
                        } else {
                            for (std::size_t x = 0; x < outW; ++x)
                                std::memcpy(dstRow + x * pixelBytes, p00 + x * stride0, pixelBytes);
                        }
                    } else if (!interpY || !interpZ) {
                        const double w = interpY ? sY.weight : sZ.weight;
                        const auto *p1 = interpY ? volBase + sY.high * stride1 + sZ.low * stride2
                                                 : volBase + sY.low * stride1 + sZ.high * stride2;
                        if (contiguousCopyX && components == 1) {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const std::size_t off = x * sizeof(T);
                                writeTyped<T>(dstRow + off, interp1D(double(readTyped<T>(p00 + off)), double(readTyped<T>(p1 + off)), w));
                            }
                        } else {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const std::size_t srcOff = x * stride0;
                                auto *dstPx = dstRow + x * pixelBytes;
                                for (std::size_t k = 0; k < components; ++k) {
                                    const std::size_t cOff = k * sizeof(T);
                                    writeTyped<T>(dstPx + cOff, interp1D(double(readTyped<T>(p00 + srcOff + cOff)),
                                                                         double(readTyped<T>(p1 + srcOff + cOff)), w));
                                }
                            }
                        }
                    } else {
                        const double wy = sY.weight, wz = sZ.weight;
                        const auto *p10 = volBase + sY.high * stride1 + sZ.low * stride2;
                        const auto *p01 = volBase + sY.low * stride1 + sZ.high * stride2;
                        const auto *p11 = volBase + sY.high * stride1 + sZ.high * stride2;
                        if (contiguousCopyX && components == 1) {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const std::size_t off = x * sizeof(T);
                                writeTyped<T>(dstRow + off, interp2D(double(readTyped<T>(p00 + off)), double(readTyped<T>(p01 + off)),
                                                                     double(readTyped<T>(p10 + off)), double(readTyped<T>(p11 + off)), wz, wy));
                            }
                        } else {
                            for (std::size_t x = 0; x < outW; ++x) {
                                const std::size_t srcOff = x * stride0;
                                auto *dstPx = dstRow + x * pixelBytes;
                                for (std::size_t k = 0; k < components; ++k) {
                                    const std::size_t cOff = k * sizeof(T);
                                    writeTyped<T>(dstPx + cOff, interp2D(double(readTyped<T>(p00 + srcOff + cOff)), double(readTyped<T>(p01 + srcOff + cOff)),
                                                                         double(readTyped<T>(p10 + srcOff + cOff)), double(readTyped<T>(p11 + srcOff + cOff)), wz, wy));
                                }
                            }
                        }
                    }
                } else {
                    // rotate90: output axis 1 (Y) is exact; interpolate source X/Y (sZ) before source Z (sX).
                    const bool interpZ = sZ.weight > 0.0;
                    const double wz = sZ.weight;
                    const auto *base0 = volBase + sY.low * stride1 + sZ.low * stride2;
                    const auto *base1 = interpZ ? volBase + sY.low * stride1 + sZ.high * stride2 : base0;
                    for (std::size_t x = 0; x < outW; ++x) {
                        const auto sX = sampleAxis(geometry.start[0], geometry.step[0], x, srcLen0, false);
                        const bool interpX = sX.weight > 0.0;
                        auto *dstPx = dstRow + x * pixelBytes;
                        const std::size_t offL = sX.low * stride0;
                        if (!interpX && !interpZ) {
                            std::memcpy(dstPx, base0 + offL, pixelBytes);
                        } else if (!interpX) {
                            for (std::size_t k = 0; k < components; ++k) {
                                const std::size_t cOff = k * sizeof(T);
                                writeTyped<T>(dstPx + cOff, interp1D(double(readTyped<T>(base0 + offL + cOff)),
                                                                     double(readTyped<T>(base1 + offL + cOff)), wz));
                            }
                        } else if (!interpZ) {
                            const double wx = sX.weight;
                            const std::size_t offH = sX.high * stride0;
                            for (std::size_t k = 0; k < components; ++k) {
                                const std::size_t cOff = k * sizeof(T);
                                writeTyped<T>(dstPx + cOff, interp1D(double(readTyped<T>(base0 + offL + cOff)),
                                                                     double(readTyped<T>(base0 + offH + cOff)), wx));
                            }
                        } else {
                            const double wx = sX.weight;
                            const std::size_t offH = sX.high * stride0;
                            for (std::size_t k = 0; k < components; ++k) {
                                const std::size_t cOff = k * sizeof(T);
                                writeTyped<T>(dstPx + cOff, interp2D(double(readTyped<T>(base0 + offL + cOff)), double(readTyped<T>(base1 + offL + cOff)),
                                                                     double(readTyped<T>(base0 + offH + cOff)), double(readTyped<T>(base1 + offH + cOff)), wz, wx));
                            }
                        }
                    }
                }
            } else {
                auto idx = volumeIndices[volume];
                for (std::size_t x = 0; x < outW; ++x) {
                    const AxisSample sX = identityX
                        ? AxisSample{x, x, 0.0}
                        : sampleAxis(geometry.start[0], geometry.step[0], x, srcLen0, options.avoidInterpolation);
                    auto *dstPx = dstRow + x * pixelBytes;
                    if (options.avoidInterpolation || (sX.weight == 0.0 && sY.weight == 0.0 && sZ.weight == 0.0)) {
                        idx[sa0] = sX.low; idx[sa1] = sY.low; idx[sa2] = sZ.low;
                        for (std::size_t k = 0; k < components; ++k)
                            writeTyped<T>(dstPx + k * sizeof(T), source.value(idx, k));
                    } else {
                        const AxisSample &sDepth = identityX ? sY : sX;
                        const std::size_t depthAxis = identityX ? sa1 : sa0;
                        idx[identityX ? sa0 : sa1] = identityX ? x : y;
                        const auto readSweep = [&](std::size_t depthIndex, std::size_t k) {
                            idx[depthAxis] = depthIndex;
                            idx[sa2] = sZ.low;
                            const double v0 = source.value(idx, k);
                            if (sZ.weight == 0.0) return v0;
                            idx[sa2] = sZ.high;
                            return interp1D(v0, source.value(idx, k), sZ.weight);
                        };
                        for (std::size_t k = 0; k < components; ++k) {
                            const double a = readSweep(sDepth.low, k);
                            const double v = sDepth.weight == 0.0 ? a : interp1D(a, readSweep(sDepth.high, k), sDepth.weight);
                            writeTyped<T>(dstPx + k * sizeof(T), v);
                        }
                    }
                }
            }

            const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress && (done % progressStep == 0 || done == totalRows))
                report(double(done) / double(totalRows));
        } catch (...) {
            const std::lock_guard lock(progressMutex);
            if (!error) error = std::current_exception();
            failed.store(true);
        }
    }, nullptr);

    if (error) std::rethrow_exception(error);
    if (stop.stop_requested())
        throw std::runtime_error(QCoreApplication::translate("Reslice", "Reslice cancelled").toStdString());
    report(1.0);
    return result;
}

} // namespace

ResliceGeometry resliceGeometry(const Image &image, const ResliceOptions &options) {
    if (image.empty() || image.shape().size() < 3 || image.depth() < 2)
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Reslice requires a Z stack with at least two slices").toStdString());
    if (!std::isfinite(options.outputSpacing) || options.outputSpacing <= 0)
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Output spacing must be positive and finite").toStdString());
    if (options.start != ResliceStart::Top && options.start != ResliceStart::Left &&
        options.start != ResliceStart::Bottom && options.start != ResliceStart::Right)
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Invalid reslice start direction").toStdString());
    const auto &axes = image.metadata().axes;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(axes[axis].spacing) || axes[axis].spacing <= 0 ||
            !std::isfinite(axes[axis].origin))
            throw std::invalid_argument(QCoreApplication::translate("Reslice", "Spatial calibration must be finite with positive spacing").toStdString());
        if (axes[axis].unit != axes[0].unit)
            throw std::invalid_argument(
                QCoreApplication::translate("Reslice", "Reslice requires matching spatial axis units. Convert the image's spatial units first.").toStdString());
    }
    auto direction = image.metadata().direction;
    if (direction.empty())
        direction = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (direction.size() != 9 ||
        !std::all_of(direction.begin(), direction.end(), [](double v) { return std::isfinite(v); }))
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Reslice requires a finite 3D spatial direction matrix").toStdString());
    const double determinant = direction[0] * (direction[4] * direction[8] - direction[5] * direction[7]) -
                               direction[1] * (direction[3] * direction[8] - direction[5] * direction[6]) +
                               direction[2] * (direction[3] * direction[7] - direction[4] * direction[6]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Spatial image direction is singular").toStdString());

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
            throw std::overflow_error(QCoreApplication::translate("Reslice", "Resliced axis exceeds the addressable size").toStdString());
        geometry.shape[axis] = std::size_t(intervals) + 1;
        geometry.metadata.axes[axis] = axes[sourceAxis];
        geometry.metadata.axes[axis].spacing *= step;
        if (!std::isfinite(geometry.metadata.axes[axis].spacing) || geometry.metadata.axes[axis].spacing <= 0)
            throw std::overflow_error(QCoreApplication::translate("Reslice", "Resliced spacing is not finite and positive").toStdString());
        for (std::size_t row = 0; row < 3; ++row)
            geometry.metadata.direction[row * 3 + axis] =
                direction[row * 3 + sourceAxis] * (geometry.step[axis] < 0 ? -1 : 1);
    }
    std::array<double, 3> startVoxel{};
    for (std::size_t axis = 0; axis < 3; ++axis)
        startVoxel[geometry.sourceAxes[axis]] = geometry.start[axis];
    for (std::size_t row = 0; row < 3; ++row) {
        double shift = 0.0;
        for (std::size_t col = 0; col < 3; ++col)
            shift += direction[row * 3 + col] * axes[col].spacing * startVoxel[col];
        const double origin = axes[row].origin + shift;
        if (!std::isfinite(origin))
            throw std::overflow_error(QCoreApplication::translate("Reslice", "Resliced spatial origin is not finite").toStdString());
        geometry.metadata.axes[row].origin = origin;
    }
    geometry.byteCount = bytesPerPixel(image.pixelType());
    for (const auto size : geometry.shape) {
        if (size > std::numeric_limits<std::size_t>::max() / geometry.byteCount)
            throw std::overflow_error(QCoreApplication::translate("Reslice", "Resliced image exceeds the addressable buffer size").toStdString());
        geometry.byteCount *= size;
    }
    // Reserve the result window's decoded cache (its current 128 MiB default), plus four plane
    // buffers for decoding/encoding, upload and texture staging. Non-RAM inputs
    // may also fault their entire source into memory while being read.
    const auto planePixels = geometry.shape[0] * geometry.shape[1];
    const std::size_t decodedBytes =
        componentCount(image.pixelType()) > 1
            ? 4
            : (image.pixelType() == PixelType::Float64 ? 8 : 4) * std::min<std::size_t>(4, image.channels());
    if (planePixels > std::numeric_limits<std::size_t>::max() / decodedBytes / 4)
        throw std::overflow_error(QCoreApplication::translate("Reslice", "Reslice working buffers exceed the addressable size").toStdString());
    const auto planeBytes = planePixels * decodedBytes;
    const auto cachePixels = geometry.byteCount / bytesPerPixel(image.pixelType());
    const auto cacheBytes =
        std::min(cachePixels, std::size_t{128} * 1024 * 1024 / decodedBytes) * decodedBytes;
    geometry.requiredMemoryBytes = geometry.byteCount;
    for (const auto extra :
         {planeBytes * 4, cacheBytes, image.storageKind() == "RAM" ? std::size_t{0} : image.byteCount()}) {
        if (extra > std::numeric_limits<std::size_t>::max() - geometry.requiredMemoryBytes)
            throw std::overflow_error(QCoreApplication::translate("Reslice", "Reslice memory requirement exceeds the addressable size").toStdString());
        geometry.requiredMemoryBytes += extra;
    }
    geometry.metadata.sourcePath.clear();
    geometry.metadata.name += QCoreApplication::translate("Reslice", " · Reslice");
    geometry.metadata.attributes.insert(QStringLiteral("ImageCXX.LastOperation"), QStringLiteral("Reslice"));
    return geometry;
}

Image reslice(const Image &image, const ResliceOptions &options, std::stop_token stop, Progress progress) {
    // Serialize reslice allocations: queued jobs must recheck memory after the
    // preceding output exists, rather than both trusting the same free-memory snapshot.
    static std::mutex allocationMutex;
    const std::unique_lock lock(allocationMutex);
    if (stop.stop_requested())
        throw std::runtime_error(QCoreApplication::translate("Reslice", "Reslice cancelled").toStdString());
    const auto geometry = resliceGeometry(image, options);
    detail::checkMemoryBudget(geometry.requiredMemoryBytes, detail::availableMemoryBytes());
    try {
        switch (image.pixelType()) {
        case PixelType::UInt8:
        case PixelType::RGB8:
        case PixelType::RGBA8:
            return sampleReslice<std::uint8_t>(image, geometry, options, stop, progress);
        case PixelType::UInt16:
            return sampleReslice<std::uint16_t>(image, geometry, options, stop, progress);
        case PixelType::Int16:
            return sampleReslice<std::int16_t>(image, geometry, options, stop, progress);
        case PixelType::Float32:
            return sampleReslice<float>(image, geometry, options, stop, progress);
        case PixelType::Float64:
            return sampleReslice<double>(image, geometry, options, stop, progress);
        }
        throw std::invalid_argument(QCoreApplication::translate("Reslice", "Unsupported reslice pixel type").toStdString());
    } catch (const std::bad_alloc &) {
        throw std::runtime_error(QCoreApplication::translate("Reslice", "Memory allocation failed while reslicing. The source image is unchanged.").toStdString());
    }
}
} // namespace imagecxx
