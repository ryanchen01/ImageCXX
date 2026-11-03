#include "processing_commands.h"
#include <QCoreApplication>
#include <stdexcept>

namespace imagecxx {
void registerProcessingCommands(CommandRegistry &registry,
    const std::function<void(const QString &, const QVariantMap &)> &execute)
{
    const Parameter scope{"scope", QCoreApplication::translate("ProcessingCommands", "Apply to"), ParameterKind::Choice, QT_TRANSLATE_NOOP("CommandChoices", "Full image"), 0, 0,
                          {QT_TRANSLATE_NOOP("CommandChoices", "Full image"), QT_TRANSLATE_NOOP("CommandChoices", "Current slice"), QT_TRANSLATE_NOOP("CommandChoices", "ROI bounds")}};
    const std::vector<Command> definitions{
        {"process.gaussian", QCoreApplication::translate("ProcessingCommands", "Gaussian blur..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"sigma", QCoreApplication::translate("ProcessingCommands", "Sigma in axis units"), ParameterKind::Real, 1.0, .001, 10000, {}}, scope}, {}},
        {"process.median", QCoreApplication::translate("ProcessingCommands", "Median filter..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"radius", QCoreApplication::translate("ProcessingCommands", "Radius in pixels"), ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.threshold", QCoreApplication::translate("ProcessingCommands", "Threshold to mask..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"lower", QCoreApplication::translate("ProcessingCommands", "Lower raw value"), ParameterKind::Real, 128.0, -1e15, 1e15, {}},
             {"upper", QCoreApplication::translate("ProcessingCommands", "Upper raw value"), ParameterKind::Real, 255.0, -1e15, 1e15, {}}, scope}, {}},
        {"process.dilate", QCoreApplication::translate("ProcessingCommands", "Dilate binary mask..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"radius", QCoreApplication::translate("ProcessingCommands", "Radius in pixels"), ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.erode", QCoreApplication::translate("ProcessingCommands", "Erode binary mask..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"radius", QCoreApplication::translate("ProcessingCommands", "Radius in pixels"), ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.resample", QCoreApplication::translate("ProcessingCommands", "Resample..."), QT_TRANSLATE_NOOP("CommandCategories", "Process"), {}, true,
            {{"spacingX", QCoreApplication::translate("ProcessingCommands", "X spacing"), ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"spacingY", QCoreApplication::translate("ProcessingCommands", "Y spacing"), ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"spacingZ", QCoreApplication::translate("ProcessingCommands", "Z spacing"), ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"nearest", QCoreApplication::translate("ProcessingCommands", "Nearest neighbor interpolation"), ParameterKind::Boolean, false, 0, 0, {}}, scope}, {}}
    };
    for (auto definition : definitions) {
        definition.execute = [execute, id = definition.id](const QVariantMap &parameters) { execute(id, parameters); };
        registry.add(std::move(definition));
    }
}

ProcessingOptions processingOptions(const QString &id, const QVariantMap &parameters)
{
    ProcessingOptions options;
    if (id == "process.gaussian") options.operation = Operation::Gaussian;
    else if (id == "process.median") options.operation = Operation::Median;
    else if (id == "process.threshold") options.operation = Operation::Threshold;
    else if (id == "process.dilate") options.operation = Operation::Dilate;
    else if (id == "process.erode") options.operation = Operation::Erode;
    else if (id == "process.resample") options.operation = Operation::Resample;
    else throw std::invalid_argument(QCoreApplication::translate("ProcessingCommands", "Unknown processing command").toStdString());
    options.sigma = parameters.value("sigma", 1).toDouble();
    options.radius = parameters.value("radius", 1).toUInt();
    options.lower = parameters.value("lower", 128).toDouble();
    options.upper = parameters.value("upper", 255).toDouble();
    if (parameters.contains("spacingX")) options.outputSpacing = {
        parameters.value("spacingX").toDouble(), parameters.value("spacingY").toDouble(),
        parameters.value("spacingZ").toDouble()};
    options.nearestNeighbor = parameters.value("nearest", false).toBool();
    return options;
}
} // namespace imagecxx
