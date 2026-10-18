#pragma once

#include <QKeySequence>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <vector>

namespace imagecxx {

enum class ParameterKind { Real, Integer, Boolean, Choice };
struct Parameter {
    QString id;
    QString label;
    ParameterKind kind = ParameterKind::Real;
    QVariant defaultValue;
    double minimum = 0;
    double maximum = 100;
    QStringList choices;
};
struct Command {
    QString id;
    QString label;
    QString category;
    QKeySequence shortcut;
    bool needsImage = true;
    std::vector<Parameter> parameters;
    std::function<void(const QVariantMap &)> execute;
};

// Shared by menus, command search, generated forms and batch execution.
class CommandRegistry {
public:
    void add(Command command);
    const std::vector<Command> &commands() const { return commands_; }
    const Command &command(const QString &id) const;
    QVariantMap validate(const QString &id, const QVariantMap &parameters) const;
    void execute(const QString &id, const QVariantMap &parameters = {}) const;
private:
    std::vector<Command> commands_;
};

} // namespace imagecxx
