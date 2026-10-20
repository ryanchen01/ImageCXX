#pragma once

#include "io/image_io.h"
#include <QJsonObject>

namespace imagecxx::detail {
void checkIo(std::stop_token stop, const IoProgress &progress, double fraction);
void atomicCopy(const QString &source, const QString &destination,
                std::stop_token stop, const IoProgress &progress = {});
QJsonObject imageDescription(const Image &image);
ImageMetadata descriptionMetadata(const QJsonObject &object);
double spatialUnitToMillimeters(const QString &unit);
} // namespace imagecxx::detail
