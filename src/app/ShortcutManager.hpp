#pragma once
#include <QKeySequence>
#include <QMap>
#include <QPointer>
#include <QVector>
class QAction;
class QShortcut;
class QWidget;
class QKeyEvent;
struct ShortcutCommand { QString id, name, category, scope, englishName; QKeySequence standard; };
class ShortcutManager {
public:
    static ShortcutManager& instance();
    const QVector<ShortcutCommand>& commands() const { return commands_; }
    QKeySequence sequence(const QString& id) const;
    void bind(const QString& id, QAction* action);
    void bind(const QString& id, QShortcut* shortcut);
    QObject* target(const QString& id, QWidget* window, QWidget* editor = nullptr) const;
    bool matches(const QString& id, const QKeyEvent* event) const;
    QString validate(const QMap<QString, QKeySequence>& values) const;
    bool save(const QMap<QString, QKeySequence>& values, QString* error);
    void showSettings(QWidget* parent);
private:
    ShortcutManager();
    void refresh();
    QVector<ShortcutCommand> commands_;
    QMap<QString, QKeySequence> overrides_;
    struct Binding { QString id; QPointer<QAction> action; QPointer<QShortcut> shortcut; };
    QVector<Binding> bindings_;
};
