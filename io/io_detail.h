#pragma once

#include "io/image_io.h"
#include <QJsonObject>
#include <QImage>

class DcmItem;

namespace imagecxx::detail {
std::size_t dicomImageBytes(DcmItem &dataset);
void checkIo(std::stop_token stop, const IoProgress &progress, double fraction);
void atomicCopy(const QString &source, const QString &destination,
                std::stop_token stop, const IoProgress &progress = {});
QJsonObject imageDescription(const Image &image);
QJsonObject parseImageDescription(const QByteArray &bytes);
QMap<QString, QString> imageDescriptionText(const Image &image);
QJsonObject parseImageDescriptionText(const QMap<QString, QString> &text);
QJsonObject rasterImageDescription(const QImage &image);
ImageMetadata descriptionMetadata(const QJsonObject &object);
double spatialUnitToMillimeters(const QString &unit);
} // namespace imagecxx::detail
