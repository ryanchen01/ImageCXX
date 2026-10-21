#include "processing/processing.h"
#include "core/memory.h"
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <algorithm>
#include <cstring>

using namespace imagecxx;

TEST(Projection, MethodsPreserveTypesComponentsAndIndependentVolumes) {
    for (auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32,
                      PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        ImageMetadata metadata; metadata.name = "Original"; metadata.sourcePath = "/original.tif";
        metadata.intensityScale = 2; metadata.intensityOffset = -5; metadata.intensityUnit = "HU";
        auto source = Image::allocate(type, {1, 1, 3, 2, 2}, metadata);
        const std::array values{10., 30., 20.};
        for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 3; ++z) for (std::size_t k = 0; k < componentCount(type); ++k)
                source.setValue(std::array<std::size_t, 5>{0, 0, z, c, t}, values[z] + 40*c + 80*t + k, k);
        ProjectionOptions options; options.rotationAxis = 2; options.totalRotation = 0;
        options.surfaceDepthCue = options.interiorDepthCue = 0;
        for (auto method : {ProjectionMethod::NearestPoint, ProjectionMethod::BrightestPoint, ProjectionMethod::MeanValue}) {
            options.method = method;
            double progress = -1;
            const auto output = project3D(source, options, {}, [&](double p) { EXPECT_GE(p, progress); progress = p; });
            EXPECT_EQ(output.pixelType(), type); EXPECT_EQ(output.shape(), (std::vector<std::size_t>{2, 2, 1, 2, 2}));
            for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
                for (std::size_t k = 0; k < componentCount(type); ++k) {
                    const double expected = method == ProjectionMethod::NearestPoint ? 10 : method == ProjectionMethod::BrightestPoint ? 30 : 20;
                    EXPECT_DOUBLE_EQ(output.sample(1, 1, 0, c, t, k), expected + 40*c + 80*t + k);
                }
            EXPECT_DOUBLE_EQ(progress, 1); EXPECT_TRUE(output.metadata().sourcePath.isEmpty());
            EXPECT_EQ(output.metadata().axes[2].name, "Angle"); EXPECT_EQ(output.metadata().axes[2].unit, "degrees");
            EXPECT_EQ(output.metadata().intensityScale, 2); EXPECT_EQ(output.metadata().intensityOffset, -5);
            EXPECT_EQ(output.metadata().intensityUnit, "HU");
        }
        EXPECT_EQ(source.sample(0, 0, 1), 30); EXPECT_EQ(source.metadata().sourcePath, "/original.tif");
    }
}

TEST(Projection, RotationsInterpolationTransparencyOpacityAndDepthCueing) {
    auto source = Image::allocate(PixelType::Float64, {1, 1, 3});
    source.setValue(std::array<std::size_t, 3>{0, 0, 0}, 10);
    source.setValue(std::array<std::size_t, 3>{0, 0, 1}, 30);
    source.setValue(std::array<std::size_t, 3>{0, 0, 2}, 20);
    ProjectionOptions options; options.totalRotation = 0; options.interiorDepthCue = options.surfaceDepthCue = 0;
    options.rotationAxis = 0; options.initialAngle = 90;
    auto output = project3D(source, options);
    EXPECT_EQ(output.sample(0, 3), 10); EXPECT_EQ(output.sample(0, 2), 30); EXPECT_EQ(output.sample(0, 1), 20);
    options.rotationAxis = 1; output = project3D(source, options);
    EXPECT_EQ(output.sample(1, 0), 10); EXPECT_EQ(output.sample(2, 0), 30); EXPECT_EQ(output.sample(3, 0), 20);
    options.rotationAxis = 2; options.initialAngle = 0; options.interiorDepthCue = 50;
    EXPECT_DOUBLE_EQ(project3D(source, options).sample(1, 1), 22.5);
    options.interiorDepthCue = 0; options.opacity = 50;
    EXPECT_DOUBLE_EQ(project3D(source, options).sample(1, 1), 20);
    options.opacity = 0; options.lower = 15; options.upper = 25;
    EXPECT_EQ(project3D(source, options).sample(1, 1), 20);
    options.method = ProjectionMethod::NearestPoint; options.surfaceDepthCue = 100;
    EXPECT_DOUBLE_EQ(project3D(source, options).sample(1, 1), 0);
    options.lower = 50; EXPECT_THROW(project3D(source, options), std::invalid_argument);
    auto two = source.region(std::array<std::size_t, 3>{0, 0, 0}, std::array<std::size_t, 3>{1, 1, 2});
    options = {}; options.totalRotation = 0; options.rotationAxis = 0; options.initialAngle = 90;
    options.sliceSpacing = 4; options.interiorDepthCue = 0;
    EXPECT_EQ(project3D(two, options).sample(0, 4), 30);
    options.interpolate = true;
    EXPECT_DOUBLE_EQ(project3D(two, options).sample(0, 4), 20);
    options.sliceSpacing = .1; EXPECT_NO_THROW(project3D(two, options));
    auto equal = Image::allocate(PixelType::Float64, {1, 1, 3});
    for (std::size_t z = 0; z < 3; ++z) equal.setValue(std::array<std::size_t, 3>{0, 0, z}, 30);
    options = {}; options.initialAngle = 180; options.totalRotation = 0; options.interiorDepthCue = 100;
    const auto tied = project3D(equal, options);
    EXPECT_DOUBLE_EQ(tied.sample(2, 0), 30);
    EXPECT_EQ(tied.sample(1, 0), 0);
}

TEST(Projection, RotatedUniformVolumesHaveNoInteriorHoles) {
    auto source = Image::allocate(PixelType::Float64, {17, 17, 17});
    std::fill_n(reinterpret_cast<double *>(source.mutableData()), source.pixelCount(), 50.);
    ProjectionOptions options; options.totalRotation = 0; options.initialAngle = 45;
    options.surfaceDepthCue = options.interiorDepthCue = 0;
    for (int axis = 0; axis < 3; ++axis) for (bool interpolate : {false, true})
        for (auto method : {ProjectionMethod::NearestPoint, ProjectionMethod::BrightestPoint, ProjectionMethod::MeanValue}) {
            options.rotationAxis = axis; options.interpolate = interpolate; options.method = method;
            const auto output = project3D(source, options);
            for (std::size_t y = output.height()/2 - 3; y <= output.height()/2 + 3; ++y)
                for (std::size_t x = output.width()/2 - 3; x <= output.width()/2 + 3; ++x)
                    EXPECT_DOUBLE_EQ(output.sample(x, y), 50) << "axis=" << axis << " x=" << x << " y=" << y;
        }
}

TEST(Projection, LinearInterpolationMatchesAnAnalyticRampAtAnObliqueAngle) {
    auto source = Image::allocate(PixelType::Float64, {17, 17, 17});
    for (std::size_t z = 0; z < 17; ++z) for (std::size_t y = 0; y < 17; ++y) for (std::size_t x = 0; x < 17; ++x)
        source.setValue(std::array<std::size_t, 3>{x, y, z}, 100 + 2*x + 3*y + 5*z);
    ProjectionOptions options; options.rotationAxis = 2; options.initialAngle = 45; options.totalRotation = 0;
    options.method = ProjectionMethod::MeanValue; options.interpolate = true; options.upper = 1000;
    options.surfaceDepthCue = options.interiorDepthCue = 0;
    const auto output = project3D(source, options);
    for (int py = -3; py <= 3; ++py) for (int px = -3; px <= 3; ++px)
        EXPECT_NEAR(output.sample(output.width()/2 + px, output.height()/2 + py),
            180 + (2*px + 3*py + 2*py - 3*px) / std::sqrt(2.), 1e-12);
}

TEST(Projection, ValidatesGeometryMemoryAndCancellation) {
    auto source = Image::allocate(PixelType::Float32, {4, 3, 2});
    ProjectionOptions options;
    auto geometry = projectionGeometry(source, options);
    EXPECT_EQ(geometry.shape[2], 36u); EXPECT_EQ(geometry.requiredMemoryBytes, geometry.byteCount);
    options.angleIncrement = 7; EXPECT_EQ(projectionGeometry(source, options).shape[2], 52u);
    options.totalRotation = 90; options.angleIncrement = 30; EXPECT_EQ(projectionGeometry(source, options).shape[2], 4u);
    options.totalRotation = 0; EXPECT_EQ(projectionGeometry(source, options).shape[2], 1u);
    EXPECT_THROW(project3D(Image{}, options), std::invalid_argument);
    EXPECT_THROW(project3D(source.slice(), options), std::invalid_argument);
    options.angleIncrement = 0; EXPECT_THROW(project3D(source, options), std::invalid_argument);
    options = {}; options.sliceSpacing = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(project3D(source, options), std::invalid_argument);
    options.sliceSpacing = 1e300; EXPECT_THROW(project3D(source, options), std::overflow_error);
    options = {}; options.opacity = 101; EXPECT_THROW(project3D(source, options), std::invalid_argument);
    options = {}; options.rotationAxis = 3; EXPECT_THROW(project3D(source, options), std::invalid_argument);
    options = {}; options.lower = -std::numeric_limits<double>::infinity();
    EXPECT_THROW(project3D(source, options), std::invalid_argument);
    options = {};
    std::stop_source cancel; cancel.request_stop();
    EXPECT_THROW(project3D(source, options, cancel.get_token()), std::runtime_error);
    cancel = std::stop_source{};
    EXPECT_THROW(project3D(source, options, cancel.get_token(), [&](double) { cancel.request_stop(); }), std::runtime_error);
    EXPECT_EQ(source.sample(0, 0), 0);
}

TEST(Reslice, ExactPermutationPreservesPixelsAndWorldCoordinatesForEveryOrientation) {
    for (auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32,
                      PixelType::Float64, PixelType::RGB8, PixelType::RGBA8}) {
        ImageMetadata metadata;
        metadata.axes = {{"X", 2, 11, "mm"}, {"Y", 3, -7, "mm"}, {"Z", 5, 23, "mm"},
                         {"C", 1, 0, ""}, {"T", 4, 9, "s"}};
        metadata.direction = {.6, -.8, 0, .8, .6, 0, 0, 0, 1};
        metadata.intensityScale = 2; metadata.intensityOffset = -31; metadata.intensityUnit = "HU";
        metadata.name = "Original"; metadata.sourcePath = "/original.tif";
        metadata.channels = {{"First", 0xff00ffff}, {"Second", 0xffff00ff}};
        auto source = Image::allocate(type, {4, 3, 2, 2, 2}, metadata);
        std::vector<std::size_t> index(5, 0);
        for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t z = 0; z < 2; ++z) for (std::size_t y = 0; y < 3; ++y)
                for (std::size_t x = 0; x < 4; ++x) for (std::size_t k = 0; k < componentCount(type); ++k) {
                    index = {x, y, z, c, t}; source.setValue(index, x + 4*y + 12*z + 24*c + 48*t + k, k);
                }
        const auto snapshot = source.clone();
        for (auto start : {ResliceStart::Top, ResliceStart::Left, ResliceStart::Bottom, ResliceStart::Right})
            for (bool flip : {false, true}) for (bool rotate : {false, true}) {
                ResliceOptions options; options.start = start; options.flipVertically = flip;
                options.rotate90 = rotate; options.avoidInterpolation = true; options.outputSpacing = .1;
                const auto result = reslice(source, options);
                const bool horizontal = start == ResliceStart::Top || start == ResliceStart::Bottom;
                const bool reverse = start == ResliceStart::Bottom || start == ResliceStart::Right;
                EXPECT_EQ(result.pixelType(), type); EXPECT_EQ(result.channels(), 2u); EXPECT_EQ(result.frames(), 2u);
                EXPECT_DOUBLE_EQ(result.metadata().intensityScale, 2); EXPECT_DOUBLE_EQ(result.metadata().intensityOffset, -31);
                EXPECT_EQ(result.metadata().intensityUnit, "HU"); EXPECT_EQ(result.metadata().channels[1].name, "Second");
                EXPECT_EQ(result.metadata().axes[4].spacing, 4); EXPECT_EQ(result.metadata().axes[4].origin, 9);
                EXPECT_TRUE(result.metadata().sourcePath.isEmpty());
                for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
                    for (std::size_t z = 0; z < result.depth(); ++z)
                        for (std::size_t y = 0; y < result.height(); ++y)
                            for (std::size_t x = 0; x < result.width(); ++x) {
                                const auto profile = rotate ? y : x, depth = rotate ? x : y;
                                const auto swept = reverse ? (horizontal ? source.height() : source.width()) - 1 - z : z;
                                const std::array<std::size_t, 3> input{horizontal ? profile : swept,
                                    horizontal ? swept : profile, flip ? source.depth() - 1 - depth : depth};
                                for (std::size_t k = 0; k < componentCount(type); ++k)
                                    EXPECT_EQ(result.sample(x, y, z, c, t, k), source.sample(input[0], input[1], input[2], c, t, k));
                                const std::array<std::size_t, 3> out{x, y, z};
                                for (std::size_t row = 0; row < 3; ++row) {
                                    double before = metadata.axes[row].origin, after = result.metadata().axes[row].origin;
                                    for (std::size_t col = 0; col < 3; ++col) {
                                        before += metadata.direction[row*3+col] * input[col] * metadata.axes[col].spacing;
                                        after += result.metadata().direction[row*3+col] * out[col] * result.metadata().axes[col].spacing;
                                    }
                                    EXPECT_NEAR(before, after, 1e-12);
                                }
                            }
            }
        EXPECT_EQ(std::memcmp(source.data(), snapshot.data(), source.byteCount()), 0);
        EXPECT_EQ(source.metadata().sourcePath, "/original.tif");
    }
}

TEST(Reslice, InterpolatedRampRetainsDoublePrecisionCalibrationAndIndependentVolumes) {
    ImageMetadata metadata;
    metadata.axes = {{"X", 2, 10, "mm"}, {"Y", 3, 20, "mm"}, {"Z", 4, 30, "mm"}, {"C"}, {"T"}};
    auto source = Image::allocate(PixelType::Float64, {4, 5, 3, 2, 2}, metadata);
    auto *pixels = reinterpret_cast<double *>(source.mutableData());
    for (std::size_t i = 0; i < source.pixelCount(); ++i) {
        const auto x = i%4, y = i/4%5, z = i/20%3, c = i/60%2, t = i/120;
        pixels[i] = x + 10*y + 100*z + 1000*c + 10000*t + .00000003;
    }
    ResliceOptions options; options.outputSpacing = 1.5;
    double progress = -1;
    auto result = reslice(source, options, {}, [&](double p) { EXPECT_GE(p, progress); progress = p; });
    EXPECT_EQ(result.shape(), (std::vector<std::size_t>{4, 5, 9, 2, 2}));
    EXPECT_DOUBLE_EQ(result.metadata().axes[0].spacing, 2);
    EXPECT_DOUBLE_EQ(result.metadata().axes[1].spacing, 2);
    EXPECT_DOUBLE_EQ(result.metadata().axes[2].spacing, 1.5);
    for (std::size_t t = 0; t < 2; ++t) for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t z = 0; z < 9; ++z) for (std::size_t y = 0; y < 5; ++y)
            for (std::size_t x = 0; x < 4; ++x)
                EXPECT_NEAR(result.sample(x, y, z, c, t), x + 5*z + 50*y + 1000*c + 10000*t + .00000003, 2e-12);
    EXPECT_EQ(progress, 1);
    options.start = ResliceStart::Right; options.flipVertically = true; options.rotate90 = true;
    result = reslice(source, options);
    EXPECT_EQ(result.shape(), (std::vector<std::size_t>{3, 5, 5, 2, 2}));
    EXPECT_NEAR(result.sample(1, 2, 1, 1, 1), 2.25 + 20 + 125 + 1000 + 10000 + .00000003, 2e-12);
}

TEST(Reslice, SupportsStridedRegionsAndLazyStorageWithoutMaterializingSource) {
    auto full = Image::allocate(PixelType::UInt16, {6, 5, 4});
    auto *pixels = reinterpret_cast<std::uint16_t *>(full.mutableData());
    std::iota(pixels, pixels + full.pixelCount(), 0);
    const auto region = full.region(std::array<std::size_t, 3>{1, 1, 1}, std::array<std::size_t, 3>{3, 2, 2});
    ResliceOptions options; options.avoidInterpolation = true;
    const auto result = reslice(region, options);
    EXPECT_EQ(result.sample(2, 1, 1), full.sample(3, 2, 2));
    EXPECT_EQ(result.metadata().axes[0].origin, 1); EXPECT_EQ(result.metadata().axes[1].origin, 1);
    EXPECT_EQ(result.metadata().axes[2].origin, 1);
    class LazyStorage final : public Storage {
    public:
        mutable std::size_t reads = 0;
        std::size_t size() const override { return 24; }
        const std::byte *data() const override { return nullptr; }
        std::byte *mutableData() override { return nullptr; }
        bool readOnly() const override { return true; }
        QString kind() const override { return "Lazy"; }
        void read(std::size_t offset, std::span<std::byte> destination) const override {
            EXPECT_EQ(destination.size(), 1u); ++reads;
            destination[0] = std::byte(offset);
        }
    };
    auto storage = std::make_shared<LazyStorage>();
    const auto lazy = Image::fromStorage(PixelType::UInt8, {3, 4, 2}, storage);
    const auto output = reslice(lazy, options);
    EXPECT_EQ(output.sample(2, 1, 3), 23); EXPECT_EQ(storage->reads, 24u);
}

TEST(Reslice, RejectsInvalidGeometryOverflowAndCancellationWithoutChangingSource) {
    auto source = Image::allocate(PixelType::Float64, {8, 7, 6});
    ResliceOptions options;
    EXPECT_THROW(reslice(Image{}, options), std::invalid_argument);
    EXPECT_THROW(reslice(source.slice(), options), std::invalid_argument);
    options.outputSpacing = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(reslice(source, options), std::invalid_argument);
    options.outputSpacing = 0; EXPECT_THROW(reslice(source, options), std::invalid_argument);
    options.outputSpacing = 1e-300; EXPECT_THROW(reslice(source, options), std::overflow_error);
    options.outputSpacing = 1e-6;
    EXPECT_GT(resliceGeometry(source, options).byteCount, std::size_t{1024} * 1024 * 1024);
    options.outputSpacing = 1;
    std::stop_source cancellation; cancellation.request_stop();
    EXPECT_THROW(reslice(source, options, cancellation.get_token()), std::runtime_error);
    cancellation = std::stop_source{};
    EXPECT_THROW(reslice(source, options, cancellation.get_token(), [&](double) { cancellation.request_stop(); }), std::runtime_error);
    source.metadata().direction = {1, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_THROW(reslice(source, options), std::invalid_argument);
    source.metadata().direction.clear(); source.metadata().axes[2].unit = "mm";
    EXPECT_THROW(reslice(source, options), std::invalid_argument);
    EXPECT_EQ(source.sample(0, 0), 0);
}

TEST(Reslice, MemoryBudgetUsesAvailableMemoryAndIncludesWorkingBuffers) {
    const std::size_t gib = std::size_t{1024} * 1024 * 1024;
    EXPECT_NO_THROW(detail::checkMemoryBudget(2 * gib, 4 * gib));
    EXPECT_NO_THROW(detail::checkMemoryBudget(3 * gib, 4 * gib));
    EXPECT_NO_THROW(detail::checkMemoryBudget(3 * gib + 1, 4 * gib));
    EXPECT_NO_THROW(detail::checkMemoryBudget(900, 1000));
    EXPECT_THROW(detail::checkMemoryBudget(901, 1000), std::length_error);
    EXPECT_NO_THROW(detail::checkMemoryBudget(90, 101));
    EXPECT_THROW(detail::checkMemoryBudget(91, 101), std::length_error);
    EXPECT_THROW(detail::checkMemoryBudget(1, 0), std::length_error);
    EXPECT_THROW(detail::checkMemoryBudget(1, 1), std::length_error);
    EXPECT_THROW(detail::checkMemoryBudget(1, std::nullopt), std::runtime_error);
    const auto largest = std::numeric_limits<std::size_t>::max();
    EXPECT_THROW(detail::checkMemoryBudget(largest, largest), std::length_error);
    auto source = Image::allocate(PixelType::UInt8, {8, 7, 6});
    const auto geometry = resliceGeometry(source, {});
    EXPECT_GT(geometry.requiredMemoryBytes, geometry.byteCount);
    EXPECT_NO_THROW(detail::checkMemoryBudget(geometry.byteCount, geometry.byteCount * 2));
    EXPECT_THROW(detail::checkMemoryBudget(geometry.requiredMemoryBytes, geometry.byteCount * 2), std::length_error);
    const auto available = detail::availableMemoryBytes();
    ASSERT_TRUE(available); EXPECT_GT(*available, 0u);
}

TEST(Reslice, MatchingUnitsAreExactLabelsWithoutImplicitConversion) {
    auto source = Image::allocate(PixelType::UInt8, {2, 3, 4});
    for (const auto &unit : {QString("mm"), QString("µm"), QString("px"), QString(""), QString("unknown")}) {
        for (std::size_t axis = 0; axis < 3; ++axis) source.metadata().axes[axis].unit = unit;
        EXPECT_NO_THROW(resliceGeometry(source, {}));
        source.metadata().axes[2].unit = unit == "mm" ? "µm" : "mm";
        EXPECT_THROW(resliceGeometry(source, {}), std::invalid_argument);
    }
    source.metadata().axes[0].unit = "um"; source.metadata().axes[1].unit = "µm"; source.metadata().axes[2].unit = "μm";
    EXPECT_THROW(resliceGeometry(source, {}), std::invalid_argument);
    source.metadata().axes[0].unit = "px"; source.metadata().axes[1].unit = "pixel"; source.metadata().axes[2].unit = "pixels";
    EXPECT_THROW(resliceGeometry(source, {}), std::invalid_argument);
}

TEST(Reslice, AllocationFailureAndWorkingSizeOverflowLeaveSourceIntact) {
    class FailingStorage final : public Storage {
    public:
        std::size_t size() const override { return std::numeric_limits<std::size_t>::max(); }
        const std::byte *data() const override { return nullptr; }
        std::byte *mutableData() override { return nullptr; }
        bool readOnly() const override { return true; }
        QString kind() const override { return "Failing"; }
        void read(std::size_t, std::span<std::byte>) const override { throw std::bad_alloc{}; }
    };
    auto storage = std::make_shared<FailingStorage>();
    const auto source = Image::fromStorage(PixelType::UInt8, {2, 3, 4}, storage);
    EXPECT_THROW(reslice(source, {}), std::runtime_error);
    EXPECT_EQ(source.shape(), (std::vector<std::size_t>{2, 3, 4}));
    const auto huge = Image::fromStorage(PixelType::UInt8, {std::numeric_limits<std::size_t>::max()/16, 2, 2}, storage);
    EXPECT_THROW(resliceGeometry(huge, {}), std::overflow_error);
}

TEST(Reslice, ColorInterpolationKeepsComponentsAndExactSamplesIgnoreAdjacentNan) {
    auto color = Image::allocate(PixelType::RGBA8, {2, 2, 2});
    std::vector<std::size_t> index(3);
    for (std::size_t z = 0; z < 2; ++z) for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 2; ++x) for (std::size_t c = 0; c < 4; ++c) {
            index = {x, y, z}; color.setValue(index, 10*x + 20*y + 40*z + c, c);
        }
    ResliceOptions options; options.outputSpacing = .5;
    const auto result = reslice(color, options);
    for (std::size_t c = 0; c < 4; ++c) EXPECT_EQ(result.sample(1, 1, 1, 0, 0, c), 60 + c);
    auto scalar = Image::allocate(PixelType::Float64, {2, 2, 2});
    auto *data = reinterpret_cast<double *>(scalar.mutableData());
    data[0] = 1.000000030003; data[2] = std::numeric_limits<double>::quiet_NaN();
    const auto floats = reslice(scalar, options);
    EXPECT_EQ(floats.sample(0, 0, 0), data[0]); EXPECT_TRUE(std::isnan(floats.sample(0, 0, 1)));
}

TEST(Reslice, ItkIntegerInterpolationRoundsHalfAwayFromZeroAndKeepsTypeLimits) {
    for (auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::RGB8, PixelType::RGBA8}) {
        auto source = Image::allocate(type, {3, 3, 2});
        const double low = type == PixelType::Int16 ? -32768 : 0;
        const double high = type == PixelType::UInt16 ? 65535 : type == PixelType::Int16 ? 32767 : 255;
        for (std::size_t z = 0; z < 2; ++z) for (std::size_t y = 0; y < 3; ++y)
            for (std::size_t x = 0; x < 3; ++x) for (std::size_t c = 0; c < componentCount(type); ++c) {
                const double value = x == 0 ? low : x == 2 ? high : y == 0 ? (low < 0 ? -1 : 0) : (low < 0 ? 0 : 1);
                source.setValue(std::array<std::size_t, 3>{x,y,z}, value, c);
            }
        ResliceOptions options; options.outputSpacing = .5;
        const auto result = reslice(source, options);
        EXPECT_EQ(result.pixelType(), type);
        for (std::size_t c = 0; c < componentCount(type); ++c) {
            EXPECT_EQ(result.sample(0,0,1,0,0,c), low);
            EXPECT_EQ(result.sample(2,0,1,0,0,c), high);
            EXPECT_EQ(result.sample(1,0,1,0,0,c), low < 0 ? -1 : 1);
        }
        for (std::size_t z = 0; z < 2; ++z) {
            source.setValue(std::array<std::size_t, 3>{1,0,z}, low);
            source.setValue(std::array<std::size_t, 3>{1,1,z}, high);
        }
        EXPECT_EQ(reslice(source, options).sample(1,0,1), std::round((low+high)/2));
    }
}

TEST(Reslice, ItkInterpolationPreservesObliqueWorldCoordinatesAcrossAllOrientations) {
    ImageMetadata metadata;
    metadata.axes = {{"X", .25, 10, "mm"}, {"Y", .5, 20, "mm"}, {"Z", 1.25, 30, "mm"}};
    metadata.direction = {.36,-.48,.8, .8,.6,0, -.48,.64,.6};
    auto source = Image::allocate(PixelType::Float64, {5,4,3}, metadata);
    for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 5; ++x)
        source.setValue(std::array<std::size_t, 3>{x,y,z}, x+10*y+100*z+.00000003);
    for (auto start : {ResliceStart::Top, ResliceStart::Left, ResliceStart::Bottom, ResliceStart::Right})
        for (bool flip : {false,true}) for (bool rotate : {false,true}) {
            ResliceOptions options; options.outputSpacing = .25; options.start = start;
            options.flipVertically = flip; options.rotate90 = rotate;
            const auto result = reslice(source, options);
            const bool horizontal = start == ResliceStart::Top || start == ResliceStart::Bottom;
            const bool reverse = start == ResliceStart::Bottom || start == ResliceStart::Right;
            const std::size_t profile = horizontal ? 0 : 1, sweep = horizontal ? 1 : 0;
            for (std::size_t z = 0; z < result.depth(); ++z) for (std::size_t y = 0; y < result.height(); ++y)
                for (std::size_t x = 0; x < result.width(); ++x) {
                    const double p = double(rotate ? y : x);
                    double d = double(rotate ? x : y)*metadata.axes[profile].spacing/metadata.axes[2].spacing;
                    double s = z*options.outputSpacing/metadata.axes[sweep].spacing;
                    if (flip) d = source.depth()-1-d;
                    if (reverse) s = source.dimension(sweep)-1-s;
                    const std::array<double,3> input{horizontal?p:s, horizontal?s:p, d};
                    EXPECT_NEAR(result.sample(x,y,z), input[0]+10*input[1]+100*input[2]+.00000003, 2e-12);
                    const std::array<std::size_t,3> output{x,y,z};
                    for (std::size_t row = 0; row < 3; ++row) {
                        double before = metadata.axes[row].origin, after = result.metadata().axes[row].origin;
                        for (std::size_t col = 0; col < 3; ++col) {
                            before += metadata.direction[row*3+col]*input[col]*metadata.axes[col].spacing;
                            after += result.metadata().direction[row*3+col]*output[col]*result.metadata().axes[col].spacing;
                        }
                        EXPECT_NEAR(before, after, 1e-12);
                    }
                }
        }
}

TEST(Reslice, ItkSamplingKeepsVoxelCenterBoundariesAndIgnoresLargeWorldOriginInSampling) {
    ImageMetadata metadata;
    metadata.axes = {{"X",1,1e16,"mm"}, {"Y",1,-2e16,"mm"}, {"Z",.8,3e16,"mm"}};
    auto source = Image::allocate(PixelType::Float64, {3,7,3}, metadata);
    for (std::size_t z = 0; z < 3; ++z) for (std::size_t y = 0; y < 7; ++y) for (std::size_t x = 0; x < 3; ++x)
        source.setValue(std::array<std::size_t,3>{x,y,z}, 7+x+10*y+100*z);
    for (double spacing : {.6,.7}) {
        ResliceOptions options; options.outputSpacing = spacing;
        const auto result = reslice(source, options);
        EXPECT_EQ(result.depth(), spacing == .6 ? 11u : 9u);
        for (std::size_t z = 0; z < result.depth(); ++z) for (std::size_t y = 0; y < result.height(); ++y)
            for (std::size_t x = 0; x < result.width(); ++x)
                EXPECT_NEAR(result.sample(x,y,z), 7+x+10*z*spacing+125*y, 2e-12);
    }
}

TEST(Reslice, ItkNearestRetainsDoubleSignedZeroAndNonFiniteSamples) {
    auto source = Image::allocate(PixelType::Float64, {2,2,2});
    const std::array<std::uint64_t,8> values{0x8000000000000000ULL,0x7ff8000000000055ULL,
        0x7ff0000000000000ULL,0xfff0000000000000ULL,1,0x8000000000000001ULL,
        0x7fefffffffffffffULL,0xffefffffffffffffULL};
    std::memcpy(source.mutableData(), values.data(), source.byteCount());
    ResliceOptions options; options.avoidInterpolation = true; options.flipVertically = true;
    const auto result = reslice(source, options);
    for (std::size_t z = 0; z < 2; ++z) for (std::size_t y = 0; y < 2; ++y) for (std::size_t x = 0; x < 2; ++x) {
        std::uint64_t actual;
        std::memcpy(&actual, result.data()+((z*2+y)*2+x)*sizeof(double), sizeof(double));
        EXPECT_EQ(actual, values[((1-y)*2+z)*2+x]);
    }
}

TEST(Reslice, ItkMemoryEstimateIncludesPackedInputAndDoubleOutputVolume) {
    auto scalar = Image::allocate(PixelType::UInt8, {8,7,6});
    const auto geometry = resliceGeometry(scalar, {});
    const auto planePixels = geometry.shape[0]*geometry.shape[1];
    const auto expectedDisplay = planePixels*4*4 + scalar.pixelCount()*4;
    EXPECT_EQ(geometry.requiredMemoryBytes, geometry.byteCount + expectedDisplay + scalar.pixelCount()*sizeof(double));
    auto color = Image::allocate(PixelType::RGB8, {8,7,6});
    const auto colorGeometry = resliceGeometry(color, {});
    EXPECT_EQ(colorGeometry.requiredMemoryBytes, colorGeometry.byteCount + expectedDisplay
        + scalar.pixelCount() + scalar.pixelCount()*sizeof(double));
}

TEST(Reslice, ItkCancellationDuringResamplingKeepsSourceUnchanged) {
    auto source = Image::allocate(PixelType::UInt16, {64,64,64});
    std::iota(reinterpret_cast<std::uint16_t *>(source.mutableData()),
              reinterpret_cast<std::uint16_t *>(source.mutableData()) + source.pixelCount(), 0);
    const auto snapshot = source.clone();
    std::stop_source cancellation;
    ResliceOptions options; options.outputSpacing = .5;
    bool cancelledInFilter = false;
    EXPECT_THROW(reslice(source, options, cancellation.get_token(), [&](double progress) {
        if (progress > .1 && progress < .9) {
            cancelledInFilter = true;
            cancellation.request_stop();
        }
    }), std::runtime_error);
    EXPECT_TRUE(cancelledInFilter);
    EXPECT_EQ(std::memcmp(source.data(), snapshot.data(), source.byteCount()), 0);
}

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

TEST(Analysis, CancellationStopsSamplingBeforeTheWholeImageIsRead) {
    class CancellingStorage final : public Storage {
    public:
        mutable std::stop_source cancellation;
        mutable std::size_t reads = 0;
        std::size_t cancelAt = 1;
        std::size_t size() const override { return 16; }
        const std::byte *data() const override { return nullptr; }
        std::byte *mutableData() override { return nullptr; }
        bool readOnly() const override { return true; }
        QString kind() const override { return "Cancelling test storage"; }
        void read(std::size_t, std::span<std::byte> destination) const override {
            std::fill(destination.begin(), destination.end(), std::byte{0});
            if (++reads == cancelAt) cancellation.request_stop();
        }
    };
    for (int operation = 0; operation < 4; ++operation) {
        auto storage = std::make_shared<CancellingStorage>();
        if (operation == 2) storage->cancelAt = 17; // Histogram's second pass.
        const auto image = Image::fromStorage(PixelType::UInt8, {4, 4}, storage);
        const auto stop = storage->cancellation.get_token();
        if (operation == 0) EXPECT_THROW(measure(image, {}, true, stop), std::runtime_error);
        else if (operation < 3) EXPECT_THROW(histogram(image, 8, {}, true, stop), std::runtime_error);
        else EXPECT_THROW(lineProfile(image, {0, 0}, {3, 3}, 4096, true, stop), std::runtime_error);
        EXPECT_LT(storage->reads, operation == 0 ? 16u : operation < 3 ? 32u : 16384u);
    }
    const auto image = Image::allocate(PixelType::UInt8, {4, 4});
    Roi roi; roi.points = {{0, 0}, {4, 4}};
    std::stop_source cancelled; cancelled.request_stop();
    EXPECT_THROW(measure(image, {}, true, cancelled.get_token()), std::runtime_error);
    EXPECT_THROW(histogram(image, 8, {}, true, cancelled.get_token()), std::runtime_error);
    EXPECT_THROW(lineProfile(image, {0, 0}, {3, 3}, 0, true, cancelled.get_token()), std::runtime_error);
    EXPECT_THROW(createMask(image, roi, cancelled.get_token()), std::runtime_error);
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
