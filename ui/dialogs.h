#pragma once

#include "io/image_io.h"
#include "processing/processing.h"
#include <functional>
#include <optional>

class QWidget;

namespace imagecxx {
class JobScheduler;

std::optional<ResliceOptions> showResliceDialog(QWidget *parent, const Image &input);
std::optional<ProjectionOptions> showProjectionDialog(QWidget *parent, const Image &input, JobScheduler &jobs);
std::optional<RawImportOptions> showRawImportDialog(QWidget *parent, const QString &path);
void showSelectionPropertiesDialog(QWidget *parent, const Roi &roi, const std::function<void(const Roi &)> &apply);

} // namespace imagecxx
