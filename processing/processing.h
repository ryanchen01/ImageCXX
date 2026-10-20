#pragma once

#include "core/image.h"
#include "core/roi.h"
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
Measurement measure(const Image &slice, const std::optional<Roi> &roi = {},
                    bool calibrated = true, std::stop_token stop = {});
Histogram histogram(const Image &slice, std::size_t bins = 256,
                    const std::optional<Roi> &roi = {}, bool calibrated = true, std::stop_token stop = {});
LineProfile lineProfile(const Image &slice, QPointF start, QPointF end,
                        std::size_t samples = 0, bool calibrated = true, std::stop_token stop = {});
Mask createMask(const Image &slice, const Roi &roi, std::stop_token stop = {});

} // namespace imagecxx
