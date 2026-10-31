#pragma once

#include "core/image.h"
#include "core/roi.h"
#include <array>
#include <functional>
#include <optional>
#include <stop_token>

namespace imagecxx {

using Progress = std::function<void(double)>;
enum class Operation { Gaussian, Median, Threshold, Dilate, Erode, Resample };

struct ProcessingOptions {
    Operation operation = Operation::Gaussian;
    // Gaussian sigma and output spacing are in physical axis units.
    double sigma = 1.0;
    unsigned int radius = 1;
    double lower = 0.0;
    double upper = 255.0;
    std::vector<double> outputSpacing;
    bool nearestNeighbor = false;
};

// Scalar 2D/3D spatial images, preserving numeric type and metadata; channel/time
// axes are filtered as independent volumes. Threshold/morphology return UInt8 masks.
Image process(const Image &image, const ProcessingOptions &options,
              std::stop_token stop = {}, Progress progress = {});
QString operationName(Operation operation);

enum class ResliceStart { Top, Left, Bottom, Right };
struct ResliceOptions {
    // Distance between output planes, in the swept source axis's physical units.
    double outputSpacing = 1.0;
    ResliceStart start = ResliceStart::Top;
    bool flipVertically = false;
    bool rotate90 = false;
    // Exact axis permutation: one source pixel per output pixel, without scaling Z.
    bool avoidInterpolation = false;
};

struct ResliceGeometry {
    std::vector<std::size_t> shape;
    ImageMetadata metadata;
    // Output index -> source index, before applying the source world transform.
    std::array<std::size_t, 3> sourceAxes;
    std::array<double, 3> start;
    std::array<double, 3> step;
    std::size_t byteCount = 0;
    std::size_t requiredMemoryBytes = 0;
};

// Full-stack orthogonal XZ/YZ reslicing through ITK. Channel/time axes stay independent.
// Interpolation samples within source voxel centers; Z uses profile-axis spacing.
// rotate90 exchanges the output plane axes, matching ImageJ's reslice option.
ResliceGeometry resliceGeometry(const Image &image, const ResliceOptions &options);
Image reslice(const Image &image, const ResliceOptions &options,
              std::stop_token stop = {}, Progress progress = {});

enum class ProjectionMethod { NearestPoint, BrightestPoint, MeanValue };
struct ProjectionOptions {
    ProjectionMethod method = ProjectionMethod::BrightestPoint;
    int rotationAxis = 1;
    double sliceSpacing = 1;
    int initialAngle = 0, totalRotation = 360, angleIncrement = 10;
    double lower = 1, upper = 255;
    int opacity = 0, surfaceDepthCue = 100, interiorDepthCue = 50;
    bool interpolate = false;
};
struct ProjectionGeometry {
    std::vector<std::size_t> shape;
    std::size_t byteCount = 0, requiredMemoryBytes = 0;
};
// Rotating projections use pixel coordinates and raw intensity bounds. The third
// output axis contains angles, with channels and time points kept separately.
ProjectionGeometry projectionGeometry(const Image &image, const ProjectionOptions &options);
Image project3D(const Image &image, const ProjectionOptions &options,
                std::stop_token stop = {}, Progress progress = {});
Measurement measure(const Image &slice, const std::optional<Roi> &roi = {},
                    bool calibrated = true, std::stop_token stop = {});
Histogram histogram(const Image &slice, std::size_t bins = 256,
                    const std::optional<Roi> &roi = {}, bool calibrated = true, std::stop_token stop = {});
LineProfile lineProfile(const Image &slice, QPointF start, QPointF end,
                        std::size_t samples = 0, bool calibrated = true, std::stop_token stop = {});
// Mean across the lane width, at pixel centers, in calibrated intensity units.
// Dark bands are negated so peaks point upward. Distance starts at zero.
LineProfile gelLaneProfile(const Image &slice, const Roi &lane, bool horizontal = false,
                           bool darkBands = true, std::stop_token stop = {});
// Integrate above the straight baseline joining two samples, using trapezoids.
// Indices are zero-based and inclusive. Negative residuals contribute zero.
double gelPeakArea(const LineProfile &profile, std::size_t first, std::size_t last);
Mask createMask(const Image &slice, const Roi &roi, std::stop_token stop = {});

} // namespace imagecxx
