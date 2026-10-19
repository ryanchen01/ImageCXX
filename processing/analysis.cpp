#include "processing/processing.h"
#include "processing_detail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imagecxx {
namespace {

void validateSlice(const Image &slice, bool scalar = true) {
    if (slice.empty() || slice.pixelCount() != slice.width() * slice.height())
        throw std::invalid_argument("Analysis requires a single 2D slice");
    if (scalar && componentCount(slice.pixelType()) != 1)
        throw std::invalid_argument("Select a scalar channel before measuring a color image");
}

double intensity(const Image &slice, std::size_t x, std::size_t y, bool calibrated) {
    const auto value = slice.sample(x, y);
    return calibrated ? value * slice.metadata().intensityScale + slice.metadata().intensityOffset : value;
}

} // namespace

Measurement measure(const Image &slice, const std::optional<Roi> &roi, bool calibrated) {
    validateSlice(slice);
    if (roi && roi->type == RoiType::Line) throw std::invalid_argument("Use a line profile for line ROIs");
    Measurement result;
    result.imageName = slice.metadata().name;
    result.roiName = roi ? roi->name : QStringLiteral("Entire slice");
    result.minimum = std::numeric_limits<double>::infinity();
    result.maximum = -std::numeric_limits<double>::infinity();
    long double mean = 0, m2 = 0, sum = 0, scale = 0;
    std::size_t selectedCount = 0;
    for (std::size_t y = 0; y < slice.height(); ++y) {
        for (std::size_t x = 0; x < slice.width(); ++x) {
            if (roi && !roi->contains(QPointF(x + 0.5, y + 0.5))) continue;
            ++selectedCount;
            const auto value = intensity(slice, x, y, calibrated);
            if (!std::isfinite(value)) continue;
            ++result.count;
            const auto nextScale = std::max(scale, std::abs(static_cast<long double>(value)));
            if (nextScale > scale) {
                const auto ratio = scale / nextScale;
                mean *= ratio; m2 *= ratio * ratio; sum *= ratio;
                scale = nextScale;
            }
            const auto normalized = scale > 0 ? static_cast<long double>(value) / scale : 0;
            sum += normalized;
            result.minimum = std::min(result.minimum, value);
            result.maximum = std::max(result.maximum, value);
            const auto delta = normalized - mean;
            mean += delta / static_cast<long double>(result.count);
            m2 += delta * (normalized - mean);
        }
    }
    result.sum = static_cast<double>(sum * scale);
    result.mean = static_cast<double>(mean * scale);
    if (result.count == 0) result.minimum = result.maximum = result.mean = std::numeric_limits<double>::quiet_NaN();
    result.standardDeviation = result.count ? static_cast<double>(std::sqrt(std::max(0.0L, m2 / result.count)) * scale)
                                          : std::numeric_limits<double>::quiet_NaN();
    const auto &axes = slice.metadata().axes;
    result.area = static_cast<double>(selectedCount) * axes[0].spacing * axes[1].spacing;
    result.areaUnit = axes[0].unit == axes[1].unit ? axes[0].unit + QStringLiteral("²")
                                                : axes[0].unit + QStringLiteral("·") + axes[1].unit;
    result.intensityUnit = calibrated ? slice.metadata().intensityUnit : QString();
    return result;
}

Histogram histogram(const Image &slice, std::size_t bins, const std::optional<Roi> &roi, bool calibrated) {
    validateSlice(slice);
    if (bins == 0 || bins > 1048576) throw std::invalid_argument("Histogram bin count must be between 1 and 1048576");
    if (roi && roi->type == RoiType::Line) throw std::invalid_argument("Use a line profile for line ROIs");
    Histogram result;
    result.centers.resize(bins);
    result.counts.assign(bins, 0);
    result.minimum = std::numeric_limits<double>::infinity();
    result.maximum = -std::numeric_limits<double>::infinity();
    for (std::size_t y = 0; y < slice.height(); ++y) {
        for (std::size_t x = 0; x < slice.width(); ++x) {
            if (roi && !roi->contains(QPointF(x + 0.5, y + 0.5))) continue;
            const auto value = intensity(slice, x, y, calibrated);
            if (!std::isfinite(value)) { ++result.nonFiniteCount; continue; }
            ++result.finiteCount;
            result.minimum = std::min(result.minimum, value);
            result.maximum = std::max(result.maximum, value);
        }
    }
    if (result.finiteCount == 0) {
        result.minimum = result.maximum = std::numeric_limits<double>::quiet_NaN();
        std::fill(result.centers.begin(), result.centers.end(), std::numeric_limits<double>::quiet_NaN());
        return result;
    }
    const auto range = static_cast<long double>(result.maximum) - static_cast<long double>(result.minimum);
    const bool halfRange = !std::isfinite(range);
    const auto denominator = halfRange ? result.maximum / 2 - result.minimum / 2 : range;
    for (std::size_t bin = 0; bin < bins; ++bin)
        result.centers[bin] = range > 0 ? std::lerp(result.minimum, result.maximum,
            (static_cast<double>(bin) + 0.5) / static_cast<double>(bins)) : result.minimum;
    for (std::size_t y = 0; y < slice.height(); ++y) {
        for (std::size_t x = 0; x < slice.width(); ++x) {
            if (roi && !roi->contains(QPointF(x + 0.5, y + 0.5))) continue;
            const auto value = intensity(slice, x, y, calibrated);
            if (!std::isfinite(value)) continue;
            const auto numerator = halfRange ? value / 2 - result.minimum / 2
                                             : static_cast<long double>(value) - result.minimum;
            const auto bin = range > 0 ? std::min(bins - 1, static_cast<std::size_t>((numerator / denominator) * bins)) : 0;
            ++result.counts[bin];
        }
    }
    return result;
}

LineProfile lineProfile(const Image &slice, QPointF start, QPointF end, std::size_t samples, bool calibrated) {
    validateSlice(slice);
    const auto inside = [&slice](QPointF point) {
        return std::isfinite(point.x()) && std::isfinite(point.y()) && point.x() >= 0 && point.y() >= 0
            && point.x() <= static_cast<double>(slice.width() - 1)
            && point.y() <= static_cast<double>(slice.height() - 1);
    };
    if (!inside(start) || !inside(end)) throw std::out_of_range("Line endpoints must be inside the image");
    const auto dx = end.x() - start.x(), dy = end.y() - start.y();
    if (samples == 0) samples = static_cast<std::size_t>(std::ceil(std::hypot(dx, dy))) + 1;
    if (samples > 10000000) throw std::invalid_argument("Line profile has too many samples");
    LineProfile result;
    const auto &axes = slice.metadata().axes;
    const auto length = std::hypot(dx * axes[0].spacing, dy * axes[1].spacing);
    if (axes[0].unit != axes[1].unit) throw std::invalid_argument("Line profile axes must have matching units");
    result.distanceUnit = axes[0].unit;
    result.intensityUnit = calibrated ? slice.metadata().intensityUnit : QString();
    result.distance.reserve(samples); result.intensity.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const double t = samples > 1 ? static_cast<double>(i) / (samples - 1) : 0;
        const auto x = std::clamp(start.x() + t * dx, 0.0, static_cast<double>(slice.width() - 1));
        const auto y = std::clamp(start.y() + t * dy, 0.0, static_cast<double>(slice.height() - 1));
        const auto x0 = static_cast<std::size_t>(std::floor(x)), y0 = static_cast<std::size_t>(std::floor(y));
        const auto x1 = std::min(x0 + 1, slice.width() - 1), y1 = std::min(y0 + 1, slice.height() - 1);
        const auto fx = x - x0, fy = y - y0;
        const double values[]{intensity(slice, x0, y0, calibrated), intensity(slice, x1, y0, calibrated),
                              intensity(slice, x0, y1, calibrated), intensity(slice, x1, y1, calibrated)};
        const double weights[]{(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        double value = 0;
        for (int corner = 0; corner < 4; ++corner) if (weights[corner] > 0) value += weights[corner] * values[corner];
        result.distance.push_back(t * length);
        result.intensity.push_back(value);
    }
    return result;
}

Mask createMask(const Image &slice, const Roi &roi) {
    validateSlice(slice, false);
    if (roi.type == RoiType::Line) throw std::invalid_argument("A line ROI cannot define an area mask");
    Mask result;
    result.name = roi.name + QStringLiteral(" mask");
    result.sourceRoiId = roi.id;
    auto metadata = slice.metadata();
    metadata.name = result.name;
    detail::resetMaskMetadata(metadata);
    result.pixels = Image::allocate(PixelType::UInt8, {slice.width(), slice.height()}, std::move(metadata));
    auto *bytes = reinterpret_cast<std::uint8_t *>(result.pixels.mutableData());
    for (std::size_t y = 0; y < slice.height(); ++y)
        for (std::size_t x = 0; x < slice.width(); ++x)
            bytes[y * slice.width() + x] = roi.contains(QPointF(x + 0.5, y + 0.5)) ? 255 : 0;
    return result;
}

} // namespace imagecxx
