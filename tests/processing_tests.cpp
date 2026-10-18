#include "processing/processing.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

using namespace imagecxx;

TEST(Analysis, MeasuresCalibratedRoiAndPopulationDeviation) {
    auto image = Image::allocate(PixelType::UInt16, {3, 2});
    auto *pixels = reinterpret_cast<std::uint16_t *>(image.mutableData());
    for (int i = 0; i < 6; ++i) pixels[i] = i;
    image.metadata().intensityScale = 2; image.metadata().intensityOffset = -1;
    image.metadata().axes[0].spacing = 0.5; image.metadata().axes[1].spacing = 2;
    Roi roi;
    roi.points = {{0, 0}, {2, 2}};
    const auto result = measure(image, roi);
    EXPECT_EQ(result.count, 4);
    EXPECT_DOUBLE_EQ(result.sum, 12);
    EXPECT_DOUBLE_EQ(result.mean, 3);
    EXPECT_DOUBLE_EQ(result.area, 4);
    EXPECT_NEAR(result.standardDeviation, std::sqrt(10.0), 1e-12);
    const auto bins = histogram(image, 5, roi);
    EXPECT_EQ(std::accumulate(bins.counts.begin(), bins.counts.end(), std::uint64_t{}), 4);
}

TEST(Analysis, HandlesConstantImagesAndNonFiniteSamples) {
    auto image = Image::allocate(PixelType::Float64, {2, 2});
    auto *pixels = reinterpret_cast<double *>(image.mutableData());
    pixels[0] = pixels[1] = pixels[2] = 7;
    pixels[3] = std::numeric_limits<double>::quiet_NaN();
    const auto bins = histogram(image, 8);
    EXPECT_EQ(bins.counts[0], 3);
    EXPECT_EQ(bins.nonFiniteCount, 1);
    EXPECT_EQ(measure(image).count, 3);
    EXPECT_DOUBLE_EQ(measure(image).area, 4);
    EXPECT_DOUBLE_EQ(measure(image).standardDeviation, 0);
    EXPECT_THROW(histogram(image, 0), std::invalid_argument);
}

TEST(Analysis, ProfilesInterpolateAndUsePhysicalDistance) {
    auto image = Image::allocate(PixelType::Float64, {3, 2});
    auto *pixels = reinterpret_cast<double *>(image.mutableData());
    for (int i = 0; i < 6; ++i) pixels[i] = i % 3;
    image.metadata().axes[0].spacing = 2;
    auto result = lineProfile(image, {0, 0}, {2, 0}, 5);
    ASSERT_EQ(result.intensity.size(), 5);
    EXPECT_DOUBLE_EQ(result.intensity[1], 0.5);
    EXPECT_DOUBLE_EQ(result.distance.back(), 4);
    EXPECT_THROW(lineProfile(image, {-1, 0}, {2, 0}), std::out_of_range);
    Roi roi;
    roi.points = {{0, 0}, {1, 2}};
    const auto mask = createMask(image, roi);
    EXPECT_EQ(mask.pixels.sample(0, 0), 255);
    EXPECT_EQ(mask.pixels.sample(1, 0), 0);
    const auto color = Image::allocate(PixelType::RGB8, {3, 2});
    EXPECT_EQ(createMask(color, roi).pixels.sample(0, 0), 255);
}

TEST(Analysis, ExtremeFiniteValuesDoNotOverflowHistogramOrDeviation) {
    auto image = Image::allocate(PixelType::Float64, {2, 2});
    auto *pixels = reinterpret_cast<double *>(image.mutableData());
    pixels[0] = pixels[1] = -1e308;
    pixels[2] = pixels[3] = 1e308;
    const auto bins = histogram(image, 4);
    EXPECT_EQ(bins.counts, (std::vector<std::uint64_t>{2, 0, 0, 2}));
    for (auto center : bins.centers) EXPECT_TRUE(std::isfinite(center));
    const auto measurement = measure(image);
    EXPECT_NEAR(measurement.mean, 0, 1e292);
    EXPECT_NEAR(measurement.sum, 0, 1e292);
    EXPECT_NEAR(measurement.standardDeviation / 1e308, 1.0, 1e-14);
}

TEST(ItkProcessing, ThresholdPreservesIntegerBoundSemanticsAndCreatesMask) {
    auto image = Image::allocate(PixelType::Int16, {3, 2});
    auto *pixels = reinterpret_cast<std::int16_t *>(image.mutableData());
    for (int i = 0; i < 6; ++i) pixels[i] = i - 2;
    image.metadata().intensityScale = 3;
    ProcessingOptions options;
    options.operation = Operation::Threshold; options.lower = -0.2; options.upper = 1.8;
    const auto mask = process(image, options);
    EXPECT_EQ(mask.pixelType(), PixelType::UInt8);
    EXPECT_EQ(mask.sample(1, 0), 0);
    EXPECT_EQ(mask.sample(2, 0), 255);
    EXPECT_EQ(mask.sample(0, 1), 255);
    EXPECT_EQ(mask.sample(1, 1), 0);
    EXPECT_DOUBLE_EQ(mask.metadata().intensityScale, 1);
    EXPECT_EQ(image.sample(2, 0), 0);
}

TEST(ItkProcessing, MedianRemovesImpulseAndGaussianPreservesConstantVolume) {
    auto image = Image::allocate(PixelType::UInt16, {5, 5});
    reinterpret_cast<std::uint16_t *>(image.mutableData())[12] = 60000;
    ProcessingOptions options;
    options.operation = Operation::Median;
    const auto median = process(image, options);
    EXPECT_EQ(median.sample(2, 2), 0);
    EXPECT_EQ(median.pixelType(), PixelType::UInt16);
    auto volume = Image::allocate(PixelType::Float64, {5, 5, 5, 2});
    auto *pixels = reinterpret_cast<double *>(volume.mutableData());
    std::fill(pixels, pixels + 125, 1.000000030003);
    std::fill(pixels + 125, pixels + 250, 9.000000030003);
    options.operation = Operation::Gaussian; options.sigma = 0.8;
    const auto blurred = process(volume, options);
    EXPECT_NEAR(blurred.sample(2, 2, 2, 0), 1.000000030003, 1e-13);
    EXPECT_NEAR(blurred.sample(2, 2, 2, 1), 9.000000030003, 1e-13);
}

TEST(ItkProcessing, SingletonZKeepsFiveDimensionalChannelTimeBlocksIndependent) {
    auto image = Image::allocate(PixelType::Int16, {3, 3, 1, 2, 3});
    auto *pixels = reinterpret_cast<std::int16_t *>(image.mutableData());
    for (int block = 0; block < 6; ++block)
        std::fill(pixels + block * 9, pixels + (block + 1) * 9, static_cast<std::int16_t>(-20 + block * 10));
    image.metadata().axes[3].name = "channel";
    image.metadata().axes[4].spacing = 3.5;
    image.metadata().axes[4].unit = "s";
    ProcessingOptions options;
    options.operation = Operation::Median;
    auto filtered = process(image, options);
    EXPECT_EQ(filtered.shape(), image.shape());
    EXPECT_EQ(filtered.pixelType(), PixelType::Int16);
    for (std::size_t t = 0; t < 3; ++t)
        for (std::size_t c = 0; c < 2; ++c)
            EXPECT_EQ(filtered.sample(1, 1, 0, c, t), -20 + static_cast<int>(c + 2 * t) * 10);
    options.operation = Operation::Resample;
    options.outputSpacing = {0.5, 0.5};
    auto resampled = process(image, options);
    EXPECT_EQ(resampled.shape(), (std::vector<std::size_t>{5, 5, 1, 2, 3}));
    EXPECT_DOUBLE_EQ(resampled.metadata().axes[4].spacing, 3.5);
    EXPECT_EQ(resampled.metadata().axes[3].name, "channel");
    for (std::size_t t = 0; t < 3; ++t)
        for (std::size_t c = 0; c < 2; ++c)
            EXPECT_EQ(resampled.sample(2, 2, 0, c, t), -20 + static_cast<int>(c + 2 * t) * 10);
}

TEST(ItkProcessing, ObliqueSliceFilteringUsesIntrinsicPlaneCoordinates) {
    ImageMetadata metadata;
    metadata.axes = {{"X", 0.5, 10, "mm"}, {"Y", 2, 20, "mm"}, {"Z", 3, 30, "mm"}};
    metadata.direction = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    auto volume = Image::allocate(PixelType::Float64, {5, 5, 3}, metadata);
    auto *pixels = reinterpret_cast<double *>(volume.mutableData());
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) pixels[(z * 5 + y) * 5 + x] = x + 10 * z + 0.00000003;
    auto slice = volume.slice(1);
    ProcessingOptions options;
    options.operation = Operation::Median;
    auto filtered = process(slice, options);
    EXPECT_NEAR(filtered.sample(2, 2), 12.00000003, 1e-13);
    EXPECT_EQ(filtered.metadata().direction, metadata.direction);
    EXPECT_EQ(filtered.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"),
              slice.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"));
    options.operation = Operation::Resample;
    options.outputSpacing = {0.25, 1};
    auto resampled = process(slice, options);
    EXPECT_EQ(resampled.shape(), (std::vector<std::size_t>{9, 9}));
    EXPECT_NEAR(resampled.sample(3, 2), 11.50000003, 1e-13);
    EXPECT_EQ(resampled.metadata().direction, metadata.direction);
    EXPECT_EQ(resampled.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"),
              slice.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"));
    auto singleton = Image::allocate(PixelType::Float64, {5, 5, 1}, metadata);
    options.operation = Operation::Gaussian;
    EXPECT_NO_THROW(process(singleton, options));
}

TEST(ItkProcessing, MorphologyExpandsAndErodesBinaryForeground) {
    auto image = Image::allocate(PixelType::UInt8, {7, 7});
    reinterpret_cast<std::uint8_t *>(image.mutableData())[24] = 1;
    ProcessingOptions options;
    options.operation = Operation::Dilate;
    auto dilated = process(image, options);
    EXPECT_EQ(dilated.sample(3, 3), 255);
    EXPECT_EQ(dilated.sample(3, 2), 255);
    EXPECT_EQ(dilated.sample(0, 0), 0);
    options.operation = Operation::Erode;
    auto eroded = process(dilated, options);
    EXPECT_EQ(eroded.sample(3, 3), 255);
    EXPECT_EQ(eroded.sample(3, 2), 0);
}

TEST(ItkProcessing, ResamplingPreservesGeometryAndInterpolates) {
    auto image = Image::allocate(PixelType::Float64, {3, 3});
    auto *pixels = reinterpret_cast<double *>(image.mutableData());
    for (int i = 0; i < 9; ++i) pixels[i] = i % 3;
    image.metadata().axes[0].origin = 10;
    image.metadata().axes[1].origin = -4;
    image.metadata().axes[0].spacing = 2;
    ProcessingOptions options;
    options.operation = Operation::Resample; options.outputSpacing = {1, 1};
    const auto result = process(image, options);
    EXPECT_EQ(result.shape(), (std::vector<std::size_t>{5, 3}));
    EXPECT_DOUBLE_EQ(result.sample(1, 1), 0.5);
    EXPECT_DOUBLE_EQ(result.metadata().axes[0].origin, 10);
    EXPECT_DOUBLE_EQ(result.metadata().axes[1].origin, -4);
    EXPECT_DOUBLE_EQ(result.metadata().axes[0].spacing, 1);
}

TEST(ItkProcessing, CancellationAndValidationFailWithoutPublishingResult) {
    auto image = Image::allocate(PixelType::UInt8, {5, 5});
    ProcessingOptions options;
    std::stop_source cancellation;
    cancellation.request_stop();
    EXPECT_THROW(process(image, options, cancellation.get_token()), std::runtime_error);
    options.sigma = -1;
    EXPECT_THROW(process(image, options), std::invalid_argument);
    options.sigma = 1;
    options.operation = Operation::Resample;
    options.outputSpacing = {1e-12, 1e-12};
    EXPECT_THROW(process(image, options), std::overflow_error);
    options.operation = Operation::Gaussian;
    double progress = -1;
    process(image, options, {}, [&progress](double value) { progress = value; });
    EXPECT_DOUBLE_EQ(progress, 1);
}
