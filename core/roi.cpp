#include "core/roi.h"

#include <algorithm>
#include <cmath>

namespace imagecxx {

QPainterPath Roi::path() const {
    if (type == RoiType::Composite) return region;
    QPainterPath result;
    if (points.size() < 2) return result;
    if (type == RoiType::Rectangle) result.addRect(bounds());
    else if (type == RoiType::Ellipse) result.addEllipse(bounds());
    else {
        result.moveTo(points.front());
        for (std::size_t i = 1; i < points.size(); ++i) result.lineTo(points[i]);
        if (type == RoiType::Polygon) result.closeSubpath();
    }
    return result;
}

QRectF Roi::bounds() const {
    if (type == RoiType::Composite) return region.boundingRect();
    if (points.empty()) return {};
    double left = points.front().x(), right = left;
    double top = points.front().y(), bottom = top;
    for (auto point : points) {
        left = std::min(left, point.x()); right = std::max(right, point.x());
        top = std::min(top, point.y()); bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

bool Roi::contains(QPointF point) const {
    if (type == RoiType::Composite) return region.contains(point);
    if (points.size() < 2 || type == RoiType::Line) return false;
    const auto rectangle = bounds();
    if (!rectangle.contains(point)) return false;
    if (type == RoiType::Rectangle) return true;
    if (type == RoiType::Ellipse) {
        if (rectangle.width() == 0 || rectangle.height() == 0) return false;
        const auto dx = (point.x() - rectangle.center().x()) / (rectangle.width() / 2);
        const auto dy = (point.y() - rectangle.center().y()) / (rectangle.height() / 2);
        return dx * dx + dy * dy <= 1.0;
    }
    if (points.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto &a = points[i]; const auto &b = points[j];
        if ((a.y() > point.y()) != (b.y() > point.y()) &&
            point.x() < (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

} // namespace imagecxx
