#include "core/history.h"
#include "core/image.h"
#include "core/roi.h"
#include <QTemporaryFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

using namespace imagecxx;

TEST(ImageModel, PreservesSignedSamplesAndDetachesSnapshots) {
    auto image = Image::allocate(PixelType::Int16, {3, 2, 2});
    const std::array<std::size_t, 3> index{2, 1, 1};
    image.setValue(index, -1234);
    auto snapshot = image;
    image.setValue(index, 3456);
    EXPECT_EQ(snapshot.value(index), -1234);
    EXPECT_EQ(image.value(index), 3456);
    EXPECT_EQ(image.strides(), (std::vector<std::size_t>{2, 6, 12}));
    EXPECT_EQ(image.byteCount(), 24);
    EXPECT_THROW(image.sample(3, 0), std::out_of_range);
}

TEST(ImageModel, Float64DoesNotLosePrecisionOrNonFiniteValues) {
    auto image = Image::allocate(PixelType::Float64, {2, 2});
    const std::array<std::size_t, 2> first{0, 0}, second{1, 0};
    image.setValue(first, 1.000000000000003);
    image.setValue(second, std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(image.value(first), 1.000000000000003);
    EXPECT_TRUE(std::isnan(image.value(second)));
    EXPECT_DOUBLE_EQ(image.clone().value(first), image.value(first));
}

TEST(ImageModel, ViewsRetainStridesAndDetachOnMutation) {
    auto image = Image::allocate(PixelType::UInt16, {4, 3, 2});
    const std::array<std::size_t, 3> index{2, 1, 1};
    image.setValue(index, 50000);
    auto slice = image.slice(1);
    EXPECT_EQ(slice.sample(2, 1), 50000);
    const std::array<std::size_t, 2> start{1, 1}, extent{2, 2};
    auto region = slice.region(start, extent);
    EXPECT_FALSE(region.contiguous());
    EXPECT_EQ(region.sample(1, 0), 50000);
    const std::array<std::size_t, 2> local{1, 0};
    region.setValue(local, 100);
    EXPECT_TRUE(region.contiguous());
    EXPECT_EQ(region.sample(1, 0), 100);
    EXPECT_EQ(image.value(index), 50000);
}

TEST(ImageModel, MappedFilesStayReadOnlyAndEditsDetach) {
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    const std::uint16_t pixels[]{42, 256, 32768, 65535};
    ASSERT_EQ(file.write("HEAD", 4), 4);
    ASSERT_EQ(file.write(reinterpret_cast<const char *>(pixels), sizeof(pixels)), sizeof(pixels));
    ASSERT_TRUE(file.flush());
    auto image = Image::map(file.fileName(), PixelType::UInt16, {2, 2}, 4);
    EXPECT_EQ(image.sample(1, 1), 65535);
    EXPECT_EQ(image.storageKind(), QStringLiteral("Memory mapped"));
    const std::array<std::size_t, 2> index{0, 0};
    image.setValue(index, 99);
    EXPECT_EQ(image.storageKind(), QStringLiteral("RAM"));
    file.seek(4);
    const auto bytes = file.read(2);
    std::uint16_t original;
    std::memcpy(&original, bytes.data(), 2);
    EXPECT_EQ(original, 42);
    EXPECT_THROW(Image::map(file.fileName(), PixelType::UInt16, {100, 100}), std::invalid_argument);
}

TEST(ImageModel, ValidatesDimensionsAndPreservesCalibration) {
    ImageMetadata metadata;
    metadata.axes = {{"X", 0.5, 10, "µm"}, {"Y", 2.0, -5, "µm"}};
    metadata.direction = {0, -1, 1, 0};
    metadata.intensityScale = 2;
    metadata.intensityOffset = -100;
    metadata.attributes.insert("instrument", "example");
    auto image = Image::allocate(PixelType::UInt8, {8, 8}, metadata);
    const std::array<std::size_t, 2> start{2, 3}, extent{4, 4};
    auto region = image.region(start, extent);
    EXPECT_DOUBLE_EQ(region.metadata().axes[0].origin, 4);
    EXPECT_DOUBLE_EQ(region.metadata().axes[1].origin, -4);
    EXPECT_EQ(region.metadata().attributes.value("instrument"), "example");
    EXPECT_DOUBLE_EQ(region.metadata().intensityScale, 2);
    EXPECT_THROW(Image::allocate(PixelType::UInt8, {0, 4}), std::invalid_argument);
    EXPECT_THROW(Image::allocate(PixelType::UInt8, {4}), std::invalid_argument);
    EXPECT_THROW(Image::allocate(PixelType::Float64, {std::numeric_limits<std::size_t>::max(), 4}), std::overflow_error);
    metadata.direction = {1, 0, 1};
    EXPECT_THROW(Image::allocate(PixelType::UInt8, {4, 4}, metadata), std::invalid_argument);
}

TEST(ImageModel, ObliqueSliceAndNestedRegionsKeepCompleteWorldOrigin) {
    ImageMetadata metadata;
    metadata.axes = {{"X", 0.5, 10, "mm"}, {"Y", 2, 20, "mm"}, {"Z", 3, 30, "mm"}};
    metadata.direction = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    auto image = Image::allocate(PixelType::Int16, {8, 8, 4}, metadata);
    auto slice = image.slice(2);
    EXPECT_EQ(slice.shape(), (std::vector<std::size_t>{8, 8}));
    EXPECT_DOUBLE_EQ(slice.metadata().axes[0].origin, 10);
    EXPECT_DOUBLE_EQ(slice.metadata().axes[1].origin, 14);
    const std::array<std::size_t, 2> start{2, 3}, extent{4, 4};
    auto cropped = slice.region(start, extent);
    const auto origin = QJsonDocument::fromJson(cropped.metadata().attributes.value(
        "ImageCXX.EmbeddedPlaneOrigin").toUtf8()).array();
    ASSERT_EQ(origin.size(), 3);
    EXPECT_DOUBLE_EQ(origin[0].toDouble(), 11);
    EXPECT_DOUBLE_EQ(origin[1].toDouble(), 14);
    EXPECT_DOUBLE_EQ(origin[2].toDouble(), 36);
    EXPECT_EQ(cropped.metadata().direction, metadata.direction);
    EXPECT_EQ(cropped.clone().metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"),
              cropped.metadata().attributes.value("ImageCXX.EmbeddedPlaneOrigin"));
}

TEST(ImageHistory, RestoresImagesAndClearsRedoAfterNewEdit) {
    auto image = Image::allocate(PixelType::UInt8, {2, 2});
    ImageHistory history(32);
    history.push(image, "Set pixel");
    const std::array<std::size_t, 2> index{0, 0};
    image.setValue(index, 255);
    auto restored = history.undo(image);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->sample(0, 0), 0);
    auto redone = history.redo(*restored);
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->sample(0, 0), 255);
    history.push(*redone, "New edit");
    EXPECT_FALSE(history.canRedo());
    history.setByteBudget(2);
    EXPECT_FALSE(history.canUndo());
    EXPECT_LE(history.retainedBytes(), history.byteBudget());
}

TEST(RoiGeometry, EllipsesAndPolygonsUsePixelCenters) {
    Roi roi;
    roi.type = RoiType::Ellipse;
    roi.points = {{0, 0}, {4, 2}};
    EXPECT_TRUE(roi.contains({2, 1}));
    EXPECT_FALSE(roi.contains({0, 0}));
    roi.type = RoiType::Polygon;
    roi.points = {{0, 0}, {3, 0}, {0, 3}};
    EXPECT_TRUE(roi.contains({0.5, 0.5}));
    EXPECT_FALSE(roi.contains({2.5, 2.5}));
}
