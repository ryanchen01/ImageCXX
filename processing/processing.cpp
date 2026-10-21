#include "processing/processing.h"
#include "processing_detail.h"

#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryDilateImageFilter.h>
#include <itkBinaryErodeImageFilter.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkCommand.h>
#include <itkDiscreteGaussianImageFilter.h>
#include <itkIdentityTransform.h>
#include <itkImage.h>
#include <itkImportImageFilter.h>
#include <itkLinearInterpolateImageFunction.h>
#include <itkMedianImageFilter.h>
#include <itkNearestNeighborInterpolateImageFunction.h>
#include <itkResampleImageFilter.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace imagecxx {
namespace {

template <typename T, unsigned int Dimension>
Image runVolume(const Image &source, std::size_t volume, const ProcessingOptions &options,
                std::stop_token stop, const Progress &progress) {
    using ItkImage = itk::Image<T, Dimension>;
    using MaskImage = itk::Image<std::uint8_t, Dimension>;
    using Import = itk::ImportImageFilter<T, Dimension>;
    auto importer = Import::New();
    typename ItkImage::SizeType size;
    typename ItkImage::IndexType start;
    typename ItkImage::SpacingType spacing;
    typename ItkImage::PointType origin;
    typename ItkImage::DirectionType direction;
    direction.SetIdentity();
    start.Fill(0);
    std::size_t count = 1;
    for (unsigned int axis = 0; axis < Dimension; ++axis) {
        size[axis] = source.dimension(axis);
        spacing[axis] = source.metadata().axes[axis].spacing;
        origin[axis] = source.metadata().axes[axis].origin;
        count *= source.dimension(axis);
    }
    const auto &orientation = source.metadata().direction;
    const auto orientationDimension = static_cast<std::size_t>(std::sqrt(orientation.size()));
    // A 3D plane's XY projection can be singular. Filters on a 2D slice work in
    // intrinsic plane coordinates while its full orientation stays in metadata.
    if (orientationDimension == Dimension && orientationDimension * orientationDimension == orientation.size()) {
        for (unsigned int row = 0; row < Dimension; ++row)
            for (unsigned int col = 0; col < Dimension; ++col)
                direction[row][col] = orientation[row * orientationDimension + col];
        if (std::abs(vnl_determinant(direction.GetVnlMatrix())) < 1e-12)
            throw std::invalid_argument("Spatial image direction is singular");
    }
    typename ItkImage::RegionType region;
    region.SetIndex(start); region.SetSize(size);
    importer->SetRegion(region);
    importer->SetSpacing(spacing); importer->SetOrigin(origin); importer->SetDirection(direction);
    auto *inputBuffer = reinterpret_cast<T *>(const_cast<std::byte *>(source.data())) + volume * count;
    importer->SetImportPointer(inputBuffer, count, false);
    importer->Update();
    const auto input = importer->GetOutput();
    auto metadata = source.metadata();
    metadata.axes.resize(Dimension);

    auto collect = [&]<typename Filter>(typename Filter::Pointer filter, PixelType type) {
        auto observer = detail::ProgressObserver::New();
        observer->stop = stop; observer->progress = progress;
        filter->AddObserver(itk::ProgressEvent(), observer);
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        filter->Update();
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        const auto output = filter->GetOutput();
        const auto outputSize = output->GetLargestPossibleRegion().GetSize();
        std::vector<std::size_t> shape;
        for (unsigned int axis = 0; axis < Dimension; ++axis) {
            shape.push_back(outputSize[axis]);
            metadata.axes[axis].spacing = output->GetSpacing()[axis];
            metadata.axes[axis].origin = output->GetOrigin()[axis];
        }
        if (type == PixelType::UInt8 && (options.operation == Operation::Threshold
            || options.operation == Operation::Dilate || options.operation == Operation::Erode)) {
            detail::resetMaskMetadata(metadata);
        }
        auto result = Image::allocate(type, std::move(shape), metadata);
        std::memcpy(result.mutableData(), output->GetBufferPointer(), result.byteCount());
        return result;
    };

    switch (options.operation) {
    case Operation::Gaussian: {
        using Filter = itk::DiscreteGaussianImageFilter<ItkImage, ItkImage>;
        auto filter = Filter::New();
        filter->SetInput(input);
        filter->SetUseImageSpacing(true);
        filter->SetVariance(options.sigma * options.sigma);
        return collect.template operator()<Filter>(filter, source.pixelType());
    }
    case Operation::Median: {
        using Filter = itk::MedianImageFilter<ItkImage, ItkImage>;
        auto filter = Filter::New();
        typename Filter::InputSizeType radius;
        radius.Fill(options.radius);
        filter->SetInput(input); filter->SetRadius(radius);
        return collect.template operator()<Filter>(filter, source.pixelType());
    }
    case Operation::Threshold: {
        using Filter = itk::BinaryThresholdImageFilter<ItkImage, MaskImage>;
        auto filter = Filter::New();
        double lower = options.lower, upper = options.upper;
        if constexpr (std::is_integral_v<T>) { lower = std::ceil(lower); upper = std::floor(upper); }
        if (lower > static_cast<double>(std::numeric_limits<T>::max())
            || upper < static_cast<double>(std::numeric_limits<T>::lowest()) || lower > upper) {
            auto result = Image::allocate(PixelType::UInt8,
                std::vector<std::size_t>(source.shape().begin(), source.shape().begin() + Dimension), metadata);
            detail::resetMaskMetadata(result.metadata());
            if (progress) progress(1.0);
            return result;
        }
        lower = std::max(lower, static_cast<double>(std::numeric_limits<T>::lowest()));
        upper = std::min(upper, static_cast<double>(std::numeric_limits<T>::max()));
        filter->SetInput(input);
        filter->SetLowerThreshold(static_cast<T>(lower)); filter->SetUpperThreshold(static_cast<T>(upper));
        filter->SetInsideValue(255); filter->SetOutsideValue(0);
        return collect.template operator()<Filter>(filter, PixelType::UInt8);
    }
    case Operation::Dilate:
    case Operation::Erode: {
        // The adapter defines finite nonzero values as foreground. ITK owns morphology.
        auto binary = MaskImage::New();
        binary->CopyInformation(input); binary->SetRegions(region); binary->Allocate();
        for (std::size_t pixel = 0; pixel < count; ++pixel) {
            if ((pixel & 65535) == 0 && stop.stop_requested()) throw std::runtime_error("Processing cancelled");
            binary->GetBufferPointer()[pixel] = std::isfinite(static_cast<double>(inputBuffer[pixel]))
                                               && inputBuffer[pixel] != T{} ? 255 : 0;
        }
        using Kernel = itk::BinaryBallStructuringElement<std::uint8_t, Dimension>;
        Kernel kernel;
        kernel.SetRadius(options.radius); kernel.CreateStructuringElement();
        if (options.operation == Operation::Dilate) {
            using Filter = itk::BinaryDilateImageFilter<MaskImage, MaskImage, Kernel>;
            auto filter = Filter::New();
            filter->SetInput(binary); filter->SetKernel(kernel); filter->SetDilateValue(255);
            return collect.template operator()<Filter>(filter, PixelType::UInt8);
        }
        using Filter = itk::BinaryErodeImageFilter<MaskImage, MaskImage, Kernel>;
        auto filter = Filter::New();
        filter->SetInput(binary); filter->SetKernel(kernel); filter->SetErodeValue(255);
        filter->SetBoundaryToForeground(false);
        return collect.template operator()<Filter>(filter, PixelType::UInt8);
    }
    case Operation::Resample: {
        using Filter = itk::ResampleImageFilter<ItkImage, ItkImage>;
        auto filter = Filter::New();
        auto outputSpacing = spacing;
        auto outputSize = size;
        std::size_t outputBytes = sizeof(T);
        for (unsigned int axis = 0; axis < Dimension; ++axis) {
            outputSpacing[axis] = options.outputSpacing[axis];
            const auto intervals = std::floor((size[axis] - 1) * spacing[axis] / outputSpacing[axis] + 1e-9);
            if (!std::isfinite(intervals) || intervals > static_cast<double>(std::numeric_limits<typename ItkImage::SizeValueType>::max() - 1))
                throw std::overflow_error("Resampled axis is too large");
            outputSize[axis] = static_cast<typename ItkImage::SizeValueType>(intervals) + 1;
            if (outputSize[axis] > std::numeric_limits<std::size_t>::max() / outputBytes)
                throw std::overflow_error("Resampled image exceeds the addressable buffer size");
            outputBytes *= outputSize[axis];
        }
        filter->SetInput(input); filter->SetSize(outputSize);
        filter->SetOutputSpacing(outputSpacing); filter->SetOutputOrigin(origin); filter->SetOutputDirection(direction);
        filter->SetTransform(itk::IdentityTransform<double, Dimension>::New());
        if (options.nearestNeighbor)
            filter->SetInterpolator(itk::NearestNeighborInterpolateImageFunction<ItkImage, double>::New());
        else filter->SetInterpolator(itk::LinearInterpolateImageFunction<ItkImage, double>::New());
        filter->SetDefaultPixelValue(T{});
        return collect.template operator()<Filter>(filter, source.pixelType());
    }
    }
    throw std::invalid_argument("Unknown processing operation");
}

template <unsigned int Dimension>
Image dispatchVolume(const Image &source, std::size_t volume, const ProcessingOptions &options,
                     std::stop_token stop, const Progress &progress) {
    switch (source.pixelType()) {
    case PixelType::UInt8: return runVolume<std::uint8_t, Dimension>(source, volume, options, stop, progress);
    case PixelType::UInt16: return runVolume<std::uint16_t, Dimension>(source, volume, options, stop, progress);
    case PixelType::Int16: return runVolume<std::int16_t, Dimension>(source, volume, options, stop, progress);
    case PixelType::Float32: return runVolume<float, Dimension>(source, volume, options, stop, progress);
    case PixelType::Float64: return runVolume<double, Dimension>(source, volume, options, stop, progress);
    default: throw std::invalid_argument("Processing requires a scalar image; split color components first");
    }
}

} // namespace

QString operationName(Operation operation) {
    switch (operation) {
    case Operation::Gaussian: return QStringLiteral("Gaussian blur");
    case Operation::Median: return QStringLiteral("Median filter");
    case Operation::Threshold: return QStringLiteral("Threshold");
    case Operation::Dilate: return QStringLiteral("Binary dilation");
    case Operation::Erode: return QStringLiteral("Binary erosion");
    case Operation::Resample: return QStringLiteral("Resample");
    }
    throw std::invalid_argument("Unknown processing operation");
}

Image process(const Image &image, const ProcessingOptions &options, std::stop_token stop, Progress progress) {
    if (image.empty()) throw std::invalid_argument("Cannot process an empty image");
    if (componentCount(image.pixelType()) != 1) throw std::invalid_argument("Processing requires a scalar image");
    if (!std::isfinite(options.sigma) || options.sigma < 0 || options.sigma > 1e6)
        throw std::invalid_argument("Gaussian sigma must be finite and between 0 and 1000000");
    if (options.radius > 128) throw std::invalid_argument("Filter radius cannot exceed 128 voxels");
    if (!std::isfinite(options.lower) || !std::isfinite(options.upper) || options.lower > options.upper)
        throw std::invalid_argument("Threshold lower bound must be finite and no greater than upper bound");
    const unsigned int dimensions = image.depth() > 1 ? 3 : 2;
    if (options.operation == Operation::Resample) {
        if (options.outputSpacing.size() != dimensions)
            throw std::invalid_argument("Supply output spacing for every spatial image axis");
        for (auto spacing : options.outputSpacing)
            if (!std::isfinite(spacing) || spacing <= 0)
                throw std::invalid_argument("Resampling spacing must be positive and finite");
    }
    if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
    if (progress) progress(0.0);
    const Image source = image.materialized();
    const auto volumePixels = image.width() * image.height() * (dimensions == 3 ? image.depth() : 1);
    const auto volumes = image.pixelCount() / volumePixels;
    Image result;
    for (std::size_t volume = 0; volume < volumes; ++volume) {
        if (stop.stop_requested()) throw std::runtime_error("Processing cancelled");
        Progress volumeProgress = [&progress, volume, volumes](double value) {
            if (progress) progress((static_cast<double>(volume) + value) / static_cast<double>(volumes));
        };
        Image block = dimensions == 3 ? dispatchVolume<3>(source, volume, options, stop, volumeProgress)
                                      : dispatchVolume<2>(source, volume, options, stop, volumeProgress);
        if (result.empty()) {
            auto shape = image.shape();
            auto metadata = image.metadata();
            for (unsigned int axis = 0; axis < dimensions; ++axis) {
                shape[axis] = block.dimension(axis);
                metadata.axes[axis] = block.metadata().axes[axis];
            }
            metadata.intensityScale = block.metadata().intensityScale;
            metadata.intensityOffset = block.metadata().intensityOffset;
            metadata.intensityUnit = block.metadata().intensityUnit;
            if (options.operation == Operation::Threshold || options.operation == Operation::Dilate || options.operation == Operation::Erode)
                metadata.attributes.remove(QStringLiteral("DICOM.0028,0004"));
            metadata.attributes.insert(QStringLiteral("ImageCXX.LastOperation"), operationName(options.operation));
            if (volumes == 1 && shape == block.shape()) {
                block.metadata() = std::move(metadata);
                if (progress) progress(1.0);
                return block;
            }
            result = Image::allocate(block.pixelType(), std::move(shape), std::move(metadata));
        }
        std::memcpy(result.mutableData() + volume * block.byteCount(), block.data(), block.byteCount());
    }
    if (progress) progress(1.0);
    return result;
}

} // namespace imagecxx
