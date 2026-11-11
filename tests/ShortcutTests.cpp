#include "app/ShortcutManager.hpp"
#include <QApplication>
#include <QAction>
#include <QShortcut>
#include <QWidget>
#include <QKeyEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QDialog>
#include <QTableWidget>
#include <cassert>
int main(int argc, char** argv) {
    QTemporaryDir directory; assert(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QApplication app(argc, argv); app.setOrganizationName("TurtleCodeTests"); app.setApplicationName("ShortcutTests");
    auto& manager = ShortcutManager::instance();
    QMap<QString, QKeySequence> defaults;
    for (const auto& command : manager.commands()) defaults[command.id] = command.standard;
    assert(manager.validate(defaults).isEmpty());
    assert(defaults.value("code.formatDocument")==QKeySequence("Ctrl+Shift+I"));
    auto values = defaults;
    values["file.open"] = QKeySequence("Ctrl+S"); assert(!manager.validate(values).isEmpty());
    values = defaults; values["file.open"] = QKeySequence("Ctrl+K"); assert(!manager.validate(values).isEmpty());
    values = defaults; values["file.open"] = QKeySequence("Ctrl+C"); assert(!manager.validate(values).isEmpty());
    values = defaults; values["file.open"] = QKeySequence("A"); assert(!manager.validate(values).isEmpty());
    values = defaults; values["file.open"] = QKeySequence("Ctrl+C, Ctrl+O"); assert(!manager.validate(values).isEmpty());
    QWidget window; QAction open(&window), folder(&window); window.addAction(&open); window.addAction(&folder);
    manager.bind("file.open", &open); manager.bind("file.openFolder", &folder);
    QShortcut cursor(&window); manager.bind("editor.nextOccurrence", &cursor);
    values = defaults; values["file.open"] = QKeySequence("Ctrl+Alt+O");
    values["editor.nextOccurrence"] = QKeySequence("Ctrl+Alt+D");
    values["file.save"] = {};
    QString error; assert(manager.save(values, &error));
    assert(open.shortcut() == QKeySequence("Ctrl+Alt+O"));
    assert(cursor.key() == QKeySequence("Ctrl+Alt+D"));
    assert(manager.sequence("file.save").isEmpty());
    QKeyEvent oldEvent(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier);
    QKeyEvent newEvent(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier | Qt::AltModifier);
    assert(!manager.matches("editor.nextOccurrence", &oldEvent)); assert(manager.matches("editor.nextOccurrence", &newEvent));
    assert(QSettings().value("shortcuts/file.open").toString() == "Ctrl+Alt+O");
    assert(QSettings().contains("shortcuts/file.save"));
    int opened = 0, folders = 0;
    QObject::connect(&open, &QAction::triggered, [&] { ++opened; });
    QObject::connect(&folder, &QAction::triggered, [&] { ++folders; });
    window.show(); window.activateWindow(); window.setFocus(); QTest::qWait(50);
    QTest::keyClick(&window, Qt::Key_O, Qt::ControlModifier | Qt::AltModifier); assert(opened == 1);
    QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(&window, Qt::Key_O, Qt::ControlModifier); assert(folders == 1);
    assert(manager.save(defaults, &error)); assert(!QSettings().contains("shortcuts/file.open"));
    assert(open.shortcut() == QKeySequence("Ctrl+O"));
    bool shown = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
        auto* table = dialog->findChild<QTableWidget*>(); assert(table && table->rowCount() == manager.commands().size());
        shown = true; dialog->reject();
    });
    manager.showSettings(&window); assert(shown);
}
