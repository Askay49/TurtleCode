#pragma once

#include "VimState.hpp"

#include <array>

class QKeyEvent;
class QSettings;

namespace VimKeymaps {

enum class Mode { Normal, Visual, Operator, Insert, Replace, Command, Count };

struct Binding {
    QString trigger;
    QVector<VimState::Input> keys;
    QString command;
    QString action;
    bool recursive{};
};

using Map = QMap<QString, Binding>;
using Tables = std::array<Map, static_cast<std::size_t>(Mode::Count)>;
using CommandValidator = std::function<bool(const QString&)>;

struct Store {
    Tables mappings;
    Tables abbreviations;
};

struct Leaders {
    QString leader, localLeader;
    bool operator==(const Leaders&) const = default;
};

bool validLeader(const QString& key);
Leaders readLeaders(const QSettings& settings);
QString modeName(Mode mode);
std::optional<Mode> mode(const QString& name);
QString token(const QKeyEvent& event);
QString trigger(const QString& text, const Leaders& leaders);
std::optional<Binding> binding(const QString& lhs, const QString& rhs, bool recursive,
                               const CommandValidator& validCommand, const Leaders& leaders);
qsizetype settingsSeparator(const QString& line);
QString parseSettings(const QString& text, Tables& result, const CommandValidator& validCommand,
                      const Leaders& leaders);

struct Command {
    QVector<Mode> modes;
    enum Operation { Define, Remove, Clear } operation{Define};
    bool recursive{}, abbreviation{};
};

std::optional<Command> exCommand(const QString& name, bool bang);
Store& global();
void reloadSettings(const CommandValidator& validCommand = {});
Map& map(Tables& tables, Mode mode);
const Map& map(const Tables& tables, Mode mode);

}
