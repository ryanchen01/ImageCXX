#include "command_registry.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imagecxx {
void CommandRegistry::add(Command command)
{
    if (command.id.isEmpty() || !command.execute || std::any_of(commands_.begin(), commands_.end(),
        [&](const Command &c) { return c.id == command.id; }))
        throw std::invalid_argument("Command needs a unique ID and an implementation");
    commands_.push_back(std::move(command));
}

const Command &CommandRegistry::command(const QString &id) const
{
    const auto found = std::find_if(commands_.begin(), commands_.end(),
                                   [&](const Command &c) { return c.id == id; });
    if (found == commands_.end()) throw std::invalid_argument("Unknown command");
    return *found;
}

QVariantMap CommandRegistry::validate(const QString &id, const QVariantMap &parameters) const
{
    const auto &definition = command(id);
    QVariantMap result;
    for (const auto &parameter : definition.parameters) {
        QVariant value = parameters.value(parameter.id, parameter.defaultValue);
        if (parameter.kind == ParameterKind::Real || parameter.kind == ParameterKind::Integer) {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || !std::isfinite(number) || number < parameter.minimum || number > parameter.maximum
                || (parameter.kind == ParameterKind::Integer && std::floor(number) != number))
                throw std::invalid_argument((parameter.label + " is outside its supported range").toStdString());
            value = number;
        } else if (parameter.kind == ParameterKind::Choice && !parameter.choices.contains(value.toString())) {
            throw std::invalid_argument((parameter.label + " has an unsupported choice").toStdString());
        } else if (parameter.kind == ParameterKind::Boolean && value.metaType().id() != QMetaType::Bool) {
            throw std::invalid_argument((parameter.label + " must be a boolean").toStdString());
        }
        result.insert(parameter.id, value);
    }
    for (auto i = parameters.begin(); i != parameters.end(); ++i) {
        if (!result.contains(i.key())) throw std::invalid_argument("Unknown command parameter");
    }
    return result;
}

void CommandRegistry::execute(const QString &id, const QVariantMap &parameters) const
{
    command(id).execute(validate(id, parameters));
}
} // namespace imagecxx
