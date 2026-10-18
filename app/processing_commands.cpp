#include "processing_commands.h"
#include <stdexcept>

namespace imagecxx {
void registerProcessingCommands(CommandRegistry &registry,
    const std::function<void(const QString &, const QVariantMap &)> &execute)
{
    const Parameter scope{"scope", "Apply to", ParameterKind::Choice, "Full image", 0, 0,
                          {"Full image", "Current slice", "ROI bounds"}};
    const std::vector<Command> definitions{
        {"process.gaussian", "Gaussian blur...", "Process", {}, true,
            {{"sigma", "Sigma in axis units", ParameterKind::Real, 1.0, .001, 10000, {}}, scope}, {}},
        {"process.median", "Median filter...", "Process", {}, true,
            {{"radius", "Radius in pixels", ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.threshold", "Threshold to mask...", "Process", {}, true,
            {{"lower", "Lower raw value", ParameterKind::Real, 128.0, -1e15, 1e15, {}},
             {"upper", "Upper raw value", ParameterKind::Real, 255.0, -1e15, 1e15, {}}, scope}, {}},
        {"process.dilate", "Dilate binary mask...", "Process", {}, true,
            {{"radius", "Radius in pixels", ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.erode", "Erode binary mask...", "Process", {}, true,
            {{"radius", "Radius in pixels", ParameterKind::Integer, 1, 1, 32, {}}, scope}, {}},
        {"process.resample", "Resample...", "Process", {}, true,
            {{"spacingX", "X spacing", ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"spacingY", "Y spacing", ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"spacingZ", "Z spacing", ParameterKind::Real, 1.0, .000001, 1000000, {}},
             {"nearest", "Nearest neighbor interpolation", ParameterKind::Boolean, false, 0, 0, {}}, scope}, {}}
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
    else throw std::invalid_argument("Unknown processing command");
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
