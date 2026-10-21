#include "core/memory.h"
#include "processing/processing.h"
#include "processing_detail.h"

#include <itkAffineTransform.h>
#include <itkLinearInterpolateImageFunction.h>
#include <itkNearestNeighborInterpolateImageFunction.h>
#include <itkResampleImageFilter.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace imagecxx {
namespace {
using ResliceTransform = itk::AffineTransform<double, 3>;

ResliceTransform::Pointer indexTransform(const ResliceGeometry &geometry) {
    auto transform = ResliceTransform::New();
    ResliceTransform::MatrixType matrix;
    ResliceTransform::OutputVectorType offset;
    matrix.Fill(0);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        matrix[geometry.sourceAxes[axis]][axis] = geometry.step[axis];
        offset[geometry.sourceAxes[axis]] = geometry.start[axis];
    }
    transform->SetMatrix(matrix);
    transform->SetOffset(offset);
    return transform;
}

template <typename T>
Image sampleReslice(const Image &source, const ResliceGeometry &geometry, const ResliceOptions &options,
                    std::stop_token stop, const Progress &progress) {
    using Input = itk::Image<T, 3>;
    using Output = itk::Image<double, 3>;
    using Filter = itk::ResampleImageFilter<Input, Output>;
    double reported = 0;
    std::mutex progressMutex;
    const auto report = [&](double fraction) {
        const std::lock_guard lock(progressMutex);
        reported = std::max(reported, std::clamp(fraction, 0.0, 1.0));
        if (progress) progress(reported);
    };
    report(0);
    if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
    auto result = Image::allocate(source.pixelType(), geometry.shape, geometry.metadata);
    auto *destination = result.mutableData();
    const auto *buffer = source.data();
    const auto components = componentCount(source.pixelType());
    const auto pixelBytes = bytesPerPixel(source.pixelType());
    const auto inputPixels = source.width() * source.height() * source.depth();
    const auto outputPixels = geometry.shape[0] * geometry.shape[1] * geometry.shape[2];
    const auto volumes = source.pixelCount() / inputPixels;
    const double units = double(volumes) * double(components);
    typename Input::SizeType inputSize;
    for (std::size_t axis = 0; axis < 3; ++axis) inputSize[axis] = source.dimension(axis);
    typename Output::SizeType outputSize;
    for (std::size_t axis = 0; axis < 3; ++axis) outputSize[axis] = geometry.shape[axis];
    const auto transform = indexTransform(geometry);
    std::vector<std::size_t> index(source.shape().size(), 0);
    for (std::size_t volume = 0; volume < volumes; ++volume) {
        auto remainder = volume;
        for (std::size_t axis = 3; axis < index.size(); ++axis) {
            index[axis] = remainder % source.dimension(axis);
            remainder /= source.dimension(axis);
        }
        for (std::size_t component = 0; component < components; ++component) {
            if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
            const double unit = double(volume * components + component);
            const T *inputBuffer = buffer && source.contiguous() && components == 1
                && reinterpret_cast<std::uintptr_t>(buffer) % alignof(T) == 0
                ? reinterpret_cast<const T *>(buffer) + volume * inputPixels : nullptr;
            std::vector<T> packed;
            if (!inputBuffer) {
                packed.resize(inputPixels);
                for (std::size_t pixel = 0; pixel < inputPixels; ++pixel) {
                    if ((pixel & 4095) == 0) {
                        if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
                        report((unit + .1 * double(pixel) / double(inputPixels)) / units);
                    }
                    index[0] = pixel % source.width(); index[1] = pixel / source.width() % source.height();
                    index[2] = pixel / (source.width() * source.height());
                    if (buffer) {
                        std::size_t offset = component * sizeof(T);
                        for (std::size_t axis = 0; axis < index.size(); ++axis) offset += index[axis] * source.strides()[axis];
                        std::memcpy(&packed[pixel], buffer + offset, sizeof(T));
                    } else packed[pixel] = static_cast<T>(source.value(index, component));
                }
                inputBuffer = packed.data();
            }
            auto input = Input::New();
            input->SetRegions(inputSize);
            input->GetPixelContainer()->SetImportPointer(const_cast<T *>(inputBuffer), inputPixels, false);
            auto filter = Filter::New();
            // A unit voxel basis avoids cancellation from large world origins or
            // oblique direction inverses. ITK owns the output-to-input affine map;
            // the corresponding calibrated world transform is retained in metadata.
            filter->SetInput(input);
            filter->SetTransform(transform); filter->SetSize(outputSize);
            if (options.avoidInterpolation)
                filter->SetInterpolator(itk::NearestNeighborInterpolateImageFunction<Input, double>::New());
            else filter->SetInterpolator(itk::LinearInterpolateImageFunction<Input, double>::New());
            auto observer = detail::ProgressObserver::New(); observer->stop = stop;
            filter->AddObserver(itk::ProgressEvent(), observer);
            observer->progress = [&](double fraction) {
                report((unit + .1 + .8 * fraction) / units);
            };
            filter->UpdateLargestPossibleRegion();
            if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
            const auto *resampled = filter->GetOutput()->GetBufferPointer();
            for (std::size_t pixel = 0; pixel < outputPixels; ++pixel) {
                if ((pixel & 4095) == 0) {
                    if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
                    report((unit + .9 + .1 * double(pixel) / double(outputPixels)) / units);
                }
                double value = resampled[pixel];
                if constexpr (std::is_integral_v<T>)
                    value = std::round(std::clamp(value, double(std::numeric_limits<T>::lowest()),
                                                  double(std::numeric_limits<T>::max())));
                const T converted = static_cast<T>(value);
                const auto at = (volume * outputPixels + pixel) * pixelBytes + component * sizeof(T);
                std::memcpy(destination + at, &converted, sizeof(T));
            }
            report((unit + 1) / units);
        }
    }
    if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
    report(1);
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
    // Transform the output origin through the same ITK affine map used by the
    // resampler, then through the source's calibrated voxel-to-world transform.
    auto world = ResliceTransform::New();
    ResliceTransform::MatrixType worldMatrix;
    ResliceTransform::OutputVectorType worldOffset;
    for (std::size_t row = 0; row < 3; ++row) {
        worldOffset[row] = axes[row].origin;
        for (std::size_t col = 0; col < 3; ++col)
            worldMatrix[row][col] = direction[row * 3 + col] * axes[col].spacing;
    }
    world->SetMatrix(worldMatrix); world->SetOffset(worldOffset);
    ResliceTransform::InputPointType zero; zero.Fill(0);
    const auto origin = world->TransformPoint(indexTransform(geometry)->TransformPoint(zero));
    for (std::size_t row = 0; row < 3; ++row) {
        if (!std::isfinite(origin[row]))
            throw std::overflow_error("Resliced spatial origin is not finite");
        geometry.metadata.axes[row].origin = origin[row];
    }
    geometry.byteCount = bytesPerPixel(image.pixelType());
    for (const auto size : geometry.shape) {
        if (size > std::numeric_limits<std::size_t>::max() / geometry.byteCount)
            throw std::overflow_error("Resliced image exceeds the addressable buffer size");
        geometry.byteCount *= size;
    }
    // ITK imports contiguous scalar volumes without copying. Other storage and
    // color components need one packed input component volume. Its resampler
    // produces a complete double-precision spatial volume before typed export.
    const auto volumePixels = image.width() * image.height() * image.depth();
    const auto componentBytes = bytesPerPixel(image.pixelType()) / componentCount(image.pixelType());
    const auto *buffer = image.data();
    const bool packInput = componentCount(image.pixelType()) > 1 || !image.contiguous() || !buffer ||
                           reinterpret_cast<std::uintptr_t>(buffer) % componentBytes != 0;
    const auto packedBytes = packInput ? volumePixels * componentBytes : 0;
    // Reserve the result window's decoded cache (its current 128 MiB default), plus four plane
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
    const auto outputVolumePixels = planePixels * geometry.shape[2];
    if (outputVolumePixels > std::numeric_limits<std::size_t>::max() / sizeof(double))
        throw std::overflow_error("ITK output volume exceeds the addressable size");
    for (const auto extra :
         {planeBytes * 4, cacheBytes, packedBytes, outputVolumePixels * sizeof(double),
          image.storageKind() == "RAM" ? std::size_t{0} : image.byteCount()}) {
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
        throw std::invalid_argument("Unsupported reslice pixel type");
    } catch (const std::bad_alloc &) {
        throw std::runtime_error("Memory allocation failed while reslicing. The source image is unchanged.");
    } catch (const itk::MemoryAllocationError &) {
        throw std::runtime_error("ITK could not allocate reslice buffers. The source image is unchanged.");
    } catch (const itk::ExceptionObject &) {
        if (stop.stop_requested()) throw std::runtime_error("Reslice cancelled");
        throw;
    }
}
} // namespace imagecxx
