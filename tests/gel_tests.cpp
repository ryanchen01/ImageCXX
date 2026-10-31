#include "processing/processing.h"
#include <gtest/gtest.h>
#include <limits>
using namespace imagecxx;

TEST(Gels, ProfilesAverageWidthPreserveCalibrationAndSupportBothDirections) {
    for (auto type : {PixelType::UInt8, PixelType::UInt16, PixelType::Int16, PixelType::Float32, PixelType::Float64}) {
        auto image = Image::allocate(type, {4, 5});
        image.metadata().axes[0].spacing = .5; image.metadata().axes[1].spacing = .25;
        image.metadata().axes[1].unit = "mm";
        image.metadata().intensityScale = 2; image.metadata().intensityOffset = 100;
        image.metadata().intensityUnit = "a.u.";
        for (std::size_t y = 0; y < 5; ++y) for (std::size_t x = 0; x < 4; ++x)
            image.setValue(std::array<std::size_t, 2>{x, y}, 10*y + x);
        Roi roi; roi.points = {{1, 1}, {3, 5}};
        auto vertical = gelLaneProfile(image, roi);
        EXPECT_EQ(vertical.distance, (std::vector<double>{0, .25, .5, .75}));
        EXPECT_EQ(vertical.intensity, (std::vector<double>{-123, -143, -163, -183}));
        EXPECT_EQ(vertical.distanceUnit, "mm"); EXPECT_EQ(vertical.intensityUnit, "a.u.");
        auto horizontal = gelLaneProfile(image, roi, true, false);
        EXPECT_EQ(horizontal.distance, (std::vector<double>{0, .5}));
        EXPECT_EQ(horizontal.intensity, (std::vector<double>{152, 154}));
        // Fractional rectangles select pixels by their centers, with an exclusive upper edge.
        roi.points = {{.5, .5}, {2.5, 3.5}};
        EXPECT_EQ(gelLaneProfile(image, roi, false, false).intensity, (std::vector<double>{101, 121, 141}));
    }
}

TEST(Gels, PeakAreaSubtractsSlopingBaselineAndUsesPhysicalDistance) {
    LineProfile p; p.distance = {0, .5, 1, 1.5, 2}; p.intensity = {10, 16, 22, 18, 14};
    EXPECT_DOUBLE_EQ(gelPeakArea(p, 0, 4), 10);
    p.intensity = {-10, -4, 2, -2, -6}; EXPECT_DOUBLE_EQ(gelPeakArea(p, 0, 4), 10);
    p.intensity = {10, 1, 12, 1, 14}; EXPECT_DOUBLE_EQ(gelPeakArea(p, 0, 4), 0);
    EXPECT_THROW(gelPeakArea(p, 4, 0), std::invalid_argument);
    EXPECT_THROW(gelPeakArea(p, 0, 5), std::invalid_argument);
    p.intensity[2] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(gelPeakArea(p, 0, 4), std::invalid_argument);
    p.intensity[2] = 12; p.distance[2] = 0;
    EXPECT_THROW(gelPeakArea(p, 0, 4), std::invalid_argument);
}

TEST(Gels, RejectInvalidLanesColorNonfiniteAndHonorCancellation) {
    auto image = Image::allocate(PixelType::Float64, {4, 5});
    Roi roi; roi.points = {{0, 0}, {2, 5}};
    std::stop_source stop; stop.request_stop();
    EXPECT_THROW(gelLaneProfile(image, roi, false, true, stop.get_token()), std::runtime_error);
    image.setValue(std::array<std::size_t, 2>{1, 2}, std::numeric_limits<double>::quiet_NaN());
    EXPECT_THROW(gelLaneProfile(image, roi), std::invalid_argument);
    image = Image::allocate(PixelType::RGB8, {4, 5});
    EXPECT_THROW(gelLaneProfile(image, roi), std::invalid_argument);
    image = Image::allocate(PixelType::UInt8, {4, 5});
    roi.type = RoiType::Line; EXPECT_THROW(gelLaneProfile(image, roi), std::invalid_argument);
    roi.type = RoiType::Rectangle;
    for (const auto &points : {std::vector<QPointF>{{-1, 0}, {2, 5}}, {{0, 0}, {5, 5}}, {{0, 0}, {2, 1}}, {{0, 0}, {.1, 5}}}) {
        roi.points = points; EXPECT_THROW(gelLaneProfile(image, roi), std::invalid_argument);
    }
}
