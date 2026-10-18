#include "batch.h"
#include "processing_commands.h"
#include "io/image_io.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>
#include <QFile>
#include <stdexcept>

namespace imagecxx {
BatchResult runBatch(const QStringList &inputs, const QString &directory, const QString &id,
                     const QVariantMap &parameters, std::stop_token stop, Progress progress)
{
    CommandRegistry registry;
    registerProcessingCommands(registry, [](const QString &, const QVariantMap &) {});
    const auto validated = registry.validate(id, parameters);
    if (validated.value("scope").toString() != "Full image")
        throw std::invalid_argument("Batch processing requires the Full image scope");
    const auto options = processingOptions(id, validated);
    if (!QDir().mkpath(directory)) throw std::runtime_error("Cannot create batch output directory");
    BatchResult result;
    QSet<QString> targets;
    const QString suffix = id.section('.', -1);
    for (qsizetype i = 0; i < inputs.size(); ++i) {
        if (stop.stop_requested()) break;
        const QString input = inputs[i];
        const QString target = QDir(directory).filePath(QFileInfo(input).completeBaseName() + "_" + suffix + ".tif");
        QString temporary;
        try {
            if (targets.contains(target) || QFileInfo::exists(target))
                throw std::runtime_error("Output already exists or input names collide");
            targets.insert(target);
            const auto update = [&, i](double fraction) { if (progress) progress((i + fraction) / inputs.size()); };
            const auto image = readImage(input, {}, stop, [update](double p) { update(p * .2); });
            auto imageOptions = options;
            if (image.depth() == 1 && imageOptions.outputSpacing.size() > 2) imageOptions.outputSpacing.resize(2);
            auto filtered = process(image, imageOptions, stop, [update](double p) { update(.2 + p * .6); });
            if (stop.stop_requested()) break;
            temporary = QDir(directory).filePath(".imagecxx-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tif");
            writeImage(filtered, temporary, stop, [update](double p) { update(.8 + p * .2); });
            if (stop.stop_requested()) { QFile::remove(temporary); break; }
            if (!QFile::rename(temporary, target)) throw std::runtime_error("Cannot publish batch output");
            result.outputs.append(target);
        } catch (const std::exception &error) {
            if (!temporary.isEmpty()) QFile::remove(temporary);
            if (!stop.stop_requested()) result.errors.append(input + ": " + QString::fromUtf8(error.what()));
        }
        if (progress) progress(double(i + 1) / inputs.size());
    }
    return result;
}
} // namespace imagecxx
