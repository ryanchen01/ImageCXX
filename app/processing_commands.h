#pragma once
#include "command_registry.h"
#include "processing/processing.h"
namespace imagecxx {
void registerProcessingCommands(CommandRegistry &registry,
    const std::function<void(const QString &, const QVariantMap &)> &execute);
ProcessingOptions processingOptions(const QString &commandId, const QVariantMap &parameters);
}
