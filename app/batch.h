#pragma once
#include "app/command_registry.h"
#include "processing/processing.h"
#include <QStringList>
namespace imagecxx {
struct BatchResult { QStringList outputs; QStringList errors; };
BatchResult runBatch(const QStringList &inputs, const QString &directory,
                     const QString &commandId, const QVariantMap &parameters,
                     std::stop_token stop = {}, Progress progress = {});
}
