#pragma once

#include "core/image.h"
#include <QImage>
#include <QString>
#include <functional>

class QObject;
class QTreeWidget;
class QTreeWidgetItem;

namespace imagecxx {
class JobScheduler;

QString propertyClipboardText(QTreeWidget *tree, int column, const QTreeWidgetItem *clicked = nullptr);
Image imageFromRaster(QImage raster);
void copyImageToClipboard(QObject *owner, const Image &slice, double minimum, double maximum, bool inverted,
                          JobScheduler &jobs, const std::function<void(const QString &)> &onPngUnavailable);
Image readImageFromClipboard(bool systemImage);

} // namespace imagecxx
