#pragma once

#include "core/image.h"
#include <QPointF>
#include <QRectF>
#include <QUuid>
#include <vector>

namespace imagecxx {

enum class RoiType { Rectangle, Ellipse, Polygon, Line };

struct Roi {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString name;
    RoiType type = RoiType::Rectangle;
    // Rectangle/ellipse/line use opposite corners or endpoints. Polygon uses all points.
    std::vector<QPointF> points;
    std::size_t z = 0;
    std::size_t channel = 0;
    std::size_t time = 0;
    bool visible = true;
    std::uint32_t color = 0xffffd573;
    QRectF bounds() const;
    bool contains(QPointF point) const;
};

struct Mask {
    QString name;
    Image pixels; // UInt8, zero outside and nonzero inside.
    QString sourceRoiId;
};

struct Measurement {
    QString imageName;
    QString roiName;
    std::size_t count = 0; // Finite intensity samples; area includes all selected pixels.
    double area = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double standardDeviation = 0.0;
    double sum = 0.0;
    QString areaUnit;
    QString intensityUnit;
};

struct Histogram {
    std::vector<double> centers;
    std::vector<std::uint64_t> counts;
    double minimum = 0.0;
    double maximum = 0.0;
    std::size_t finiteCount = 0;
    std::size_t nonFiniteCount = 0;
};

struct LineProfile {
    std::vector<double> distance;
    std::vector<double> intensity;
    QString distanceUnit;
    QString intensityUnit;
};

} // namespace imagecxx
