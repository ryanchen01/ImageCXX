#include "processing/processing.h"
#include <QColor>
#include <QCoreApplication>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace imagecxx {

Image editPixels(const Image &input, PixelEdit operation, const std::optional<Roi> &roi,
                 std::size_t z, std::size_t channel, std::size_t time, std::uint32_t colorValue,
                 std::stop_token stop, Progress progress)
{
    if (input.shape().size() > 5)
        throw std::invalid_argument(QCoreApplication::translate("MainWindow", "Pixel editing supports up to five dimensions.").toStdString());
    if (operation == PixelEdit::Draw && (!roi || (roi->type == RoiType::Line && roi->points.size() < 2)))
        throw std::invalid_argument(QCoreApplication::translate("MainWindow", "Select an ROI first.").toStdString());
    if (roi && roi->type == RoiType::Line && operation != PixelEdit::Draw)
        throw std::invalid_argument(QCoreApplication::translate("MainWindow", "Select an area ROI first.").toStdString());
    Image output = input.clone();
    const auto type = input.pixelType();
    const QColor color = QColor::fromRgba(colorValue);
    const bool painting = operation == PixelEdit::Fill || operation == PixelEdit::Draw;
    double maximum = type == PixelType::UInt16 ? 65535 : type == PixelType::Int16 ? 32767 : 255;
    double minimum = type == PixelType::Int16 ? -32768 : 0;
    if ((operation == PixelEdit::Invert || painting) && (type == PixelType::Float32 || type == PixelType::Float64)) {
        const auto stats = measure(input.slice(z, channel, time), {}, false, stop);
        minimum = stats.minimum;
        maximum = stats.maximum;
        if (painting && !stats.count)
            throw std::runtime_error(QCoreApplication::translate("MainWindow", "Cannot draw without a finite intensity range.").toStdString());
    }
    const double gray = std::lerp(minimum, maximum, qGray(color.rgb()) / 255.0);
    const std::array<double, 3> rgb{double(color.red()), double(color.green()), double(color.blue())};
    const bool line = roi && roi->type == RoiType::Line;
    // Endpoints select their containing pixels; sample one pixel per major-axis step.
    QPointF first = line ? QPointF(std::floor(roi->points[0].x()), std::floor(roi->points[0].y())) : QPointF{};
    QPointF last = line ? QPointF(std::floor(roi->points[1].x()), std::floor(roi->points[1].y())) : QPointF{};
    const bool horizontal = std::abs(last.x() - first.x()) >= std::abs(last.y() - first.y());
    // Use the same interpolation direction so reversing endpoints cannot change rounding.
    if ((horizontal ? first.x() : first.y()) > (horizontal ? last.x() : last.y())) std::swap(first, last);
    const double start = horizontal ? first.x() : first.y();
    const double end = horizontal ? last.x() : last.y();
    for (std::size_t y = 0; y < input.height(); ++y) {
        if (stop.stop_requested())
            throw std::runtime_error(QCoreApplication::translate("MainWindow", "Operation cancelled").toStdString());
        for (std::size_t x = 0; x < input.width(); ++x) {
            const QPointF point(x + .5, y + .5);
            if (line) {
                const double major = horizontal ? x : y;
                if (major < start || major > end) continue;
                const double fraction = start == end ? 0 : (major - start) / (end - start);
                const double minor = std::floor(std::lerp(horizontal ? first.y() : first.x(),
                    horizontal ? last.y() : last.x(), fraction) + .5);
                if ((horizontal ? y : x) != minor) continue;
            } else if (roi && !roi->contains(point)) continue;
            if (!line && operation == PixelEdit::Draw && roi && roi->contains(point + QPointF(1, 0))
                && roi->contains(point - QPointF(1, 0)) && roi->contains(point + QPointF(0, 1))
                && roi->contains(point - QPointF(0, 1))) continue;
            const std::array<std::size_t, 5> index{x, y, z, channel, time};
            for (std::size_t component = 0; component < componentCount(type); ++component) {
                if (type == PixelType::RGBA8 && component == 3) continue;
                double value = operation == PixelEdit::Clear ? 0 : maximum;
                if (painting) value = componentCount(type) > 1 ? rgb[component] : gray;
                if (operation == PixelEdit::Invert) value = minimum + maximum - input.sample(x, y, z, channel, time, component);
                if (operation == PixelEdit::Smooth) {
                    value = 0;
                    int count = 0;
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                        const double sample = input.sample(std::clamp<qint64>(qint64(x) + dx, 0, input.width() - 1),
                            std::clamp<qint64>(qint64(y) + dy, 0, input.height() - 1), z, channel, time, component);
                        ++count;
                        value = std::isfinite(value) && std::isfinite(sample)
                            ? std::lerp(value, sample, 1.0 / count) : value + sample;
                    }
                }
                output.setValue(std::span(index).first(input.shape().size()), value, component);
            }
        }
        if (progress) progress(double(y + 1) / input.height());
    }
    return output;
}

} // namespace imagecxx
