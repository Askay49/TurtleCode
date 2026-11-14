#include "document/RecoveryStore.hpp"
#include "document/FileState.hpp"
#include "app/MainWindow.hpp"
#include "app/ShortcutManager.hpp"
#include "terminal/TerminalPanel.hpp"
#include "terminal/TerminalWidget.hpp"
#include "terminal/TerminalSession.hpp"
#include <QToolButton>
#include <QSplitter>
#include <QStackedWidget>
#include "app/ThemeManager.hpp"
#include "app/WorkspaceBackground.hpp"
#include <QImage>
#include "app/I18n.hpp"
#include <QMenuBar>
#include <QComboBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFontInfo>
#include <algorithm>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QFrame>
#include <QMenu>
#include <QCheckBox>
#include "editor/EditorWidget.hpp"
#include "language/LspClient.hpp"
#include "language/LanguageRegistry.hpp"
#include "project/ProjectExplorer.hpp"
#include <QDir>
#include <QApplication>
#include <QAction>
#include <QInputDialog>
#include <QMessageBox>
#include <QTreeWidget>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QScrollBar>
#include <QScrollArea>
#include <QWindow>
#include <QSpinBox>
#include <QDebug>
#include <QDateTime>
#include <QJsonObject>
#include <cassert>
static bool waitFor(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed()<10000) { QApplication::processEvents(); QThread::msleep(1); }
    return predicate();
}
static void closeDiscard(MainWindow& window) {
    QTimer responder;responder.setInterval(1);
    QObject::connect(&responder,&QTimer::timeout,[]{
        if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if(auto* button=message->button(QMessageBox::Discard))button->click();
    });responder.start();window.close();
    assert(waitFor([&]{const auto editors=window.findChildren<EditorWidget*>();return !window.isVisible() || std::any_of(editors.begin(),editors.end(),[](auto* e){return e->isBusy();});}));
}
static void externalFileTests(QTemporaryDir& directory) {
    const auto path = directory.filePath("external.txt");
    const auto write = [&](const QByteArray& text, bool atomic = false) {
        if (atomic) {
            QSaveFile file(path); assert(file.open(QIODevice::WriteOnly));
            assert(file.write(text) == text.size()); assert(file.commit());
        } else {
            QFile file(path); assert(file.open(QIODevice::WriteOnly));
            assert(file.write(text) == text.size());
        }
    };
    write("one\ntwo\nthree\n");
    MainWindow window; window.show(); window.openPaths({path});
    auto* editor = window.findChild<EditorWidget*>(); assert(editor);
    assert(waitFor([&] { return !editor->isBusy(); }));
    auto text = [&] { return QByteArray::fromStdString(editor->document()->buffer().text()); };
    auto conflict = [&]() -> QMessageBox* {
        for (auto* message : window.findChildren<QMessageBox*>("externalFileConflict"))
            if (message->isVisible()) return message;
        return nullptr;
    };
    auto click = [&](QMessageBox::ButtonRole role) {
        auto* message = conflict(); assert(message);
        for (auto* button : message->buttons())
            if (message->buttonRole(button) == role) { button->click(); return; }
        assert(false);
    };
    auto* debounce = window.findChild<QTimer*>("externalFileDebounce"); assert(debounce);
    // Complete initial watch registration before writing.
    assert(waitFor([&] { return !debounce->isActive(); }));
    editor->setNavigationPosition(2, 2);
    write("new one\nnew two\nnew three\n");
    assert(waitFor([&] { return text().startsWith("new one"); }));
    assert(!editor->isModified() && editor->navigationPosition() == QPoint(2, 2));
    assert(editor->document()->diskState == diskFileState(path));
    editor->documentUndo(); assert(text().startsWith("new one"));
    // Atomic saves replace the inode; subsequent saves must still be observed.
    write("atomic one\n", true);
    assert(waitFor([&] { return text() == "atomic one\n"; }));
    write("atomic two\n", true);
    assert(waitFor([&] { return text() == "atomic two\n"; }));
    // A reload clears Undo rather than permitting a return to stale content.
    QTest::keyClick(editor, Qt::Key_X); assert(editor->isModified());
    editor->documentUndo(); assert(!editor->isModified());
    write("after undo\n");
    assert(waitFor([&] { return text() == "after undo\n"; }));
    editor->documentUndo(); assert(text() == "after undo\n" && !editor->isModified());

    QTest::keyClick(editor, Qt::Key_Y); const auto local = text();
    write("outside with conflict\n");
    assert(waitFor([&] { return conflict() && conflict()->isVisible(); }));
    assert(text() == local && editor->isModified());
    click(QMessageBox::RejectRole);
    assert(waitFor([&] { return !conflict(); }));
    // Keeping the local copy suppresses repeated prompts but keeps Save's protection.
    QTest::qWait(2300); assert(!conflict() && text() == local);
    assert(editor->document()->diskState != diskFileState(path));
    QTimer cancel; cancel.setInterval(1);
    QObject::connect(&cancel, &QTimer::timeout, [] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if (auto* button = message->button(QMessageBox::Cancel)) button->click();
    });
    QAction* save = nullptr;
    for (auto* a : window.findChildren<QAction*>()) if (a->text() == "&Save") save = a;
    assert(save); cancel.start(); save->trigger(); cancel.stop();
    assert(text() == local && editor->isModified());
    // A newer external version offers a fresh choice.
    write("reload accepted\n", true);
    assert(waitFor([&] { return conflict() && conflict()->isVisible(); }));
    click(QMessageBox::DestructiveRole);
    assert(waitFor([&] { return text() == "reload accepted\n" && !editor->isModified(); }));
    assert(waitFor([&] { return !conflict(); }));
    // Busy documents defer synchronization until the operation finishes.
    editor->setBusy(true); write("deferred\n"); QTest::qWait(700);
    assert(text() == "reload accepted\n"); editor->setBusy(false);
    assert(waitFor([&] { return text() == "deferred\n"; }));
    // Never install an asynchronous result over edits made after accepting Reload.
    QTest::keyClick(editor, Qt::Key_Z); write("racing reload\n");
    assert(waitFor([&] { return conflict() && conflict()->isVisible(); }));
    click(QMessageBox::DestructiveRole);
    QTest::keyClick(editor, Qt::Key_Q); const auto newerLocal = text();
    assert(waitFor([&] { return conflict() && conflict()->isVisible(); }));
    assert(text() == newerLocal && editor->isModified());
    click(QMessageBox::DestructiveRole);
    assert(waitFor([&] { return text() == "racing reload\n" && !editor->isModified(); }));
    assert(waitFor([&] { return !conflict(); }));
    // A deleted clean file becomes an unsaved copy and survives; Save recreates it.
    assert(QFile::remove(path));
    assert(waitFor([&] { return editor->isModified(); }));
    assert(editor->filePath() == path && text() == "racing reload\n");
    QTimer overwrite; overwrite.setInterval(1);
    QObject::connect(&overwrite, &QTimer::timeout, [] {
        if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            for (auto* button : message->buttons())
                if (message->buttonRole(button) == QMessageBox::DestructiveRole) { button->click(); break; }
    });
    overwrite.start(); save->trigger();
    assert(waitFor([&] { return !editor->isBusy(); })); overwrite.stop();
    assert(QFileInfo::exists(path) && !editor->isModified());
    // Our own save does not trigger a spurious reload or clear its Undo history.
    QTest::qWait(600); QTest::keyClick(editor, Qt::Key_W); save->trigger();
    assert(waitFor([&] { return !editor->isBusy(); })); QTest::qWait(600);
    assert(!conflict() && !editor->isModified());
    editor->documentUndo(); assert(editor->isModified());
    // Undo back to the saved state before another external update.
    editor->documentUndo(true); assert(!editor->isModified());
    write("still watching\n", true);
    assert(waitFor([&] { return text() == "still watching\n"; }));
    // Losing the parent directory must not hang the watcher reconciliation.
    const auto folder = directory.filePath("vanishing"); assert(QDir().mkpath(folder));
    const auto nested = folder + "/nested.txt";
    { QFile file(nested); assert(file.open(QIODevice::WriteOnly)); file.write("nested\n"); }
    window.openPaths({nested});
    auto* tabs = window.findChild<QTabWidget*>();
    auto* nestedEditor = qobject_cast<EditorWidget*>(tabs->currentWidget()); assert(nestedEditor);
    assert(waitFor([&] { return !nestedEditor->isBusy(); }));
    assert(QDir(folder).removeRecursively());
    assert(waitFor([&] { return nestedEditor->isModified(); }));
    assert(nestedEditor->document()->buffer().text() == "nested\n");
    closeDiscard(window);
}

static void safetyTests(QTemporaryDir& directory) {
    const auto write=[&](const QString& path,const QByteArray& text){QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(text)==text.size());};
    const auto read=[](const QString& path){QFile file(path);assert(file.open(QIODevice::ReadOnly));return file.readAll();};
    const auto action=[](MainWindow& window,const QString& text){for(auto* a:window.findChildren<QAction*>())if(a->text()==text)return a;assert(false);return static_cast<QAction*>(nullptr);};
    const auto first=directory.filePath("first.txt"),second=directory.filePath("second.txt");
    write(first,"first\n");write(second,"second\n");
    {
        MainWindow window;window.show();window.openPaths({first,second});
        auto* tabs=window.findChild<QTabWidget*>();assert(tabs);
        assert(waitFor([&]{for(auto* e:window.findChildren<EditorWidget*>())if(e->isBusy())return false;return true;}));
        auto* editor=qobject_cast<EditorWidget*>(tabs->widget(0));assert(editor);
        editor->setNavigationPosition(1,1);QTest::keyClick(editor,Qt::Key_X);assert(editor->isModified());
        QTimer responder;responder.setInterval(1);
        QObject::connect(&responder,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))if(auto* b=m->button(QMessageBox::Cancel))b->click();});
        responder.start();window.close();responder.stop();
        assert(window.isVisible() && tabs->count()==2 && editor->isModified() && editor->isEnabled());
        // Quit must use exactly the same confirmation path.
        responder.start();action(window,"&Quit")->trigger();responder.stop();assert(window.isVisible() && editor->isModified());
        // Saving on exit completes asynchronously before the window disappears.
        QTimer saver;saver.setInterval(1);
        QObject::connect(&saver,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))if(auto* b=m->button(QMessageBox::Save))b->click();});
        saver.start();window.close();window.openPaths({directory.filePath("should-not-open.txt")});assert(tabs->count()==2);assert(waitFor([&]{return !window.isVisible();}));saver.stop();
        assert(read(first)=="xfirst\n");
    }
    {
        MainWindow window;window.show();window.openPaths({first});
        auto* editor=window.findChild<EditorWidget*>();assert(editor);
        assert(waitFor([&]{return !editor->isBusy();}));
        QTest::keyClick(editor,Qt::Key_Y);assert(editor->isModified());
        write(first,"external\n");
        QTimer cancel;cancel.setInterval(1);
        QObject::connect(&cancel,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))if(auto* b=m->button(QMessageBox::Cancel))b->click();});
        cancel.start();action(window,"&Save")->trigger();cancel.stop();
        assert(read(first)=="external\n" && editor->isModified());
        QTimer overwrite;overwrite.setInterval(1);
        QObject::connect(&overwrite,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))for(auto* b:m->buttons())if(m->buttonRole(b)==QMessageBox::DestructiveRole)b->click();});
        overwrite.start();action(window,"&Save")->trigger();overwrite.stop();
        assert(waitFor([&]{return !editor->isBusy();}));assert(!editor->isModified() && read(first)=="yxfirst\n");
        // An unwritable destination keeps both the document and its modified state.
        const auto impossible=directory.filePath("missing-parent/file.txt");
        editor->updateFilePathAfterRename(impossible);QTest::keyClick(editor,Qt::Key_Z);
        QTimer errors;errors.setInterval(1);
        QObject::connect(&errors,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))if(auto* b=m->button(QMessageBox::Ok))b->click();});
        errors.start();action(window,"&Save")->trigger();assert(waitFor([&]{return !editor->isBusy();}));errors.stop();
        assert(editor->isModified() && !QFileInfo::exists(impossible));closeDiscard(window);
    }
    // Backup preference: Cancel, persistence, disabled writes and re-enabling.
    {
        QDir(RecoveryStore::rootDirectory()).removeRecursively();
        MainWindow window;window.show();window.openPaths({first});
        auto* editor=window.findChild<EditorWidget*>();assert(editor);
        assert(waitFor([&]{return !editor->isBusy();}));
        const auto setBackups=[&](bool enabled,bool save) {
            QTimer::singleShot(0,[&,enabled,save] {
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);
                auto* checkbox=dialog->findChild<QCheckBox*>("recoveryEnabled");assert(checkbox);
                assert(checkbox->isChecked()==QSettings().value("editor/recoveryEnabled",true).toBool());
                checkbox->setChecked(enabled);
                if(save)dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                else dialog->reject();
            });
            action(window,"Editor...")->trigger();
        };
        setBackups(false,false);assert(QSettings().value("editor/recoveryEnabled",true).toBool());
        setBackups(false,true);assert(!QSettings().value("editor/recoveryEnabled",true).toBool());
        const auto directories=QDir(RecoveryStore::rootDirectory()).entryList(QDir::Dirs|QDir::NoDotAndDotDot);assert(directories.size()==1);
        const auto base=RecoveryStore::rootDirectory()+"/"+directories.front();
        const auto id=editor->property("recoveryId").toString();
        auto* debounce=window.findChild<QTimer*>("recoveryDebounce");assert(debounce);debounce->setInterval(1);
        QTest::keyClick(editor,Qt::Key_A);debounce->start();
        assert(waitFor([&]{return QFileInfo::exists(base+"/session.json");}));
        assert(!QFileInfo::exists(base+"/"+id+".data"));
        assert(!RecoveryStore::readObject(base+"/session.json")["documents"].toArray().first().toObject()["modified"].toBool());
        setBackups(true,true);assert(QSettings().value("editor/recoveryEnabled",false).toBool());
        assert(waitFor([&]{return RecoveryStore::readObject(base+"/session.json")["documents"].toArray().first().toObject()["modified"].toBool();}));
        const auto payload=read(base+"/"+id+".data");
        setBackups(false,true);QTest::keyClick(editor,Qt::Key_B);debounce->start();
        assert(waitFor([&]{return !debounce->isActive();}));
        // Let the session-only worker publish; the existing payload stays untouched.
        QApplication::processEvents();assert(read(base+"/"+id+".data")==payload);
        setBackups(false,false);assert(!QSettings().value("editor/recoveryEnabled",true).toBool());
        setBackups(true,true);closeDiscard(window);
    }
    // Exercise the actual automatic checkpoint pipeline and its disk-error notice.
    {
        QDir(RecoveryStore::rootDirectory()).removeRecursively();
        MainWindow window;window.show();window.openPaths({first});
        auto* editor=window.findChild<EditorWidget*>();assert(editor);
        assert(waitFor([&]{return !editor->isBusy();}));
        const auto directories=QDir(RecoveryStore::rootDirectory()).entryList(QDir::Dirs|QDir::NoDotAndDotDot);assert(directories.size()==1);
        const auto directoryPath=RecoveryStore::rootDirectory()+"/"+directories.front();
        const auto id=editor->property("recoveryId").toString();
        auto* debounce=window.findChild<QTimer*>("recoveryDebounce");assert(debounce);debounce->setInterval(1);
        assert(QDir().mkpath(directoryPath+"/"+id+".data"));
        QTest::keyClick(editor,Qt::Key_A);
        assert(waitFor([&]{auto* label=window.findChild<QLabel*>("recoveryWarning");return label && label->isVisible();}));
        assert(editor->isModified());
        assert(QDir(directoryPath+"/"+id+".data").removeRecursively());
        QTest::keyClick(editor,Qt::Key_B);
        assert(waitFor([&]{return QFileInfo::exists(directoryPath+"/session.json") && !window.findChild<QLabel*>("recoveryWarning")->isVisible();}));
        PieceTreeBuffer restored;QJsonObject metadata;QString error;
        assert(RecoveryStore::restoreDocument(directoryPath,id,restored,metadata,error));
        assert(restored.text()==editor->document()->buffer().text());
        const auto size=QFileInfo(directoryPath+"/"+id+".data").size();
        QTest::keyClick(editor,Qt::Key_C);
        assert(waitFor([&]{return QFileInfo(directoryPath+"/"+id+".data").size()>size;}));
        closeDiscard(window);
        assert(!QFileInfo::exists(directoryPath+"/"+id+".data"));
    }
    // Restore a crash checkpoint after an external edit: never overwrite either version.
    const auto recoveryRoot=RecoveryStore::rootDirectory();
    QDir(recoveryRoot).removeRecursively();
    QString abandoned;
    {
        RecoveryStore store;abandoned=store.directory();
        PieceTreeBuffer text("\xef\xbb\xbfнесохранённый текст\r\nsecond line\r\n");
        assert(store.writeDocument("lost",text,{{"path",first},{"diskState",QString::fromLatin1(diskFileState(first))}}).isEmpty());
        assert(store.writeSession({{"clean",false},{"time","1"},{"active","lost"},{"roots",QJsonArray{directory.path()}},
            {"documents",QJsonArray{QJsonObject{{"id","lost"},{"path",first},{"modified",true},{"line",2},{"column",3},{"vertical",0},{"horizontal",0}}}}}).isEmpty());
    }
    write(first,"changed while editor was closed\n");
    {
        MainWindow window;window.show();
        QTimer restore;restore.setInterval(1);
        QObject::connect(&restore,&QTimer::timeout,[]{if(auto* m=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))for(auto* b:m->buttons())if(m->buttonRole(b)==QMessageBox::AcceptRole)b->click();});
        restore.start();window.restorePreviousSession();restore.stop();
        assert(waitFor([&]{auto* e=window.findChild<EditorWidget*>();return e && !e->isBusy();}));
        auto* editor=window.findChild<EditorWidget*>();assert(editor->isModified());
        assert(editor->document()->buffer().text()=="\xef\xbb\xbfнесохранённый текст\r\nsecond line\r\n");
        assert(editor->navigationPosition()==QPoint(2,3));
        assert(read(first)=="changed while editor was closed\n");
        assert(editor->document()->diskState!=diskFileState(first));
        closeDiscard(window);
        assert(RecoveryStore::readObject(abandoned+"/session.json")["documents"].toArray().isEmpty());
    }
    // Clean sessions restore paths, workspace and cursor without opening a recovery prompt.
    QDir(recoveryRoot).removeRecursively();
    {
        MainWindow window;window.show();window.openPaths({directory.path(),first,second});
        assert(waitFor([&]{for(auto* e:window.findChildren<EditorWidget*>())if(e->isBusy())return false;return true;}));
        auto* tabs=window.findChild<QTabWidget*>();tabs->setCurrentIndex(0);
        auto* editor=qobject_cast<EditorWidget*>(tabs->widget(0));editor->setNavigationPosition(1,4);
        closeDiscard(window);
    }
    {
        MainWindow window;window.show();window.restorePreviousSession();
        auto* tabs=window.findChild<QTabWidget*>();assert(tabs->count()==2);
        assert(waitFor([&]{for(auto* e:window.findChildren<EditorWidget*>())if(e->isBusy())return false;return true;}));
        auto* editor=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(editor && editor->filePath()==first && editor->navigationPosition()==QPoint(1,4));
        closeDiscard(window);
    }
}
#include "MemoryWorkload.hpp"
#include "FormattingTests.hpp"
#include "CommandPaletteTests.hpp"
#include "ProjectScaleWindowTests.hpp"
#include "GitWindowTests.hpp"

static void terminalWindowTests(const QTemporaryDir& directory) {
    // This layout check measures the editor with the optional outline closed.
    QSettings().setValue("view/outline", false);
    if (!qEnvironmentVariableIsEmpty("TURTLE_CODE_TERMINAL_SCREENSHOT")) {
        I18n::saveLanguage("ru"); I18n::initialize();
        QSettings().setValue("appearance/theme", "tokyo-night");
        QSettings().setValue("appearance/backgroundImage", qEnvironmentVariable("TURTLE_CODE_TERMINAL_BACKGROUND"));
        QSettings().setValue("appearance/backgroundOpacity", 15);
    }
    ThemeManager::instance().initialize();
    const auto project = directory.filePath("terminal project space");
    assert(QDir().mkpath(project));
    const auto path = project + "/file.txt";
    { QFile file(path); assert(file.open(QIODevice::WriteOnly)); assert(file.write("unchanged\n") == 10); }
    QSettings().setValue("fonts/terminal/size", 17);
    MainWindow window; window.resize(1200, 850); window.show(); window.activateWindow(); window.openPaths({project, path});
    auto* editor = window.findChild<EditorWidget*>();
    assert(editor && waitFor([&] { return !editor->isBusy(); }));
    editor->setFocus(); QApplication::processEvents();
    auto* toggle = window.findChild<QAction*>("view.terminal"); assert(toggle);
    assert(toggle->shortcut() == QKeySequence("Ctrl+`"));
    assert(!window.findChild<TerminalPanel*>());
    auto* workspace = window.findChild<QSplitter*>("workspaceSplitter"); assert(workspace);
    auto* sidebar = workspace->widget(0);
    auto bounds = [&window](QWidget* widget) { return QRect(widget->mapTo(&window, QPoint()), widget->size()); };
    const auto sidebarBefore = bounds(sidebar);
    QTest::keyClick(editor, Qt::Key_QuoteLeft, Qt::ControlModifier);
    auto* panel = window.findChild<TerminalPanel*>(); assert(panel && panel->isVisible());
    auto checkLayout = [&] {
        QApplication::processEvents();
        const auto treeBounds = bounds(sidebar);
        const auto editorBounds = bounds(editor->parentWidget());
        const auto panelBounds = bounds(panel);
        assert(treeBounds.right() < panelBounds.left());
        assert(editorBounds.left() == panelBounds.left());
        assert(editorBounds.right() == panelBounds.right());
        assert(editorBounds.bottom() < panelBounds.top());
        assert(treeBounds.bottom() == panelBounds.bottom());
    };
    checkLayout();
    assert(bounds(sidebar) == sidebarBefore);
    auto* panelTabs = panel->findChild<QTabWidget*>("terminalTabs"); assert(panelTabs);
    assert(panelTabs->count() == 3);
    assert(panelTabs->tabText(0) == I18n::text("OUTPUT"));
    assert(panelTabs->tabText(1) == I18n::text("DEBUG CONSOLE"));
    assert(panelTabs->tabText(2) == I18n::text("TERMINAL"));
    assert(panelTabs->currentWidget() == panel->terminal());
    assert(waitFor([&] { return panel->findChild<QLabel*>("terminalShellName")->text()
                                == QFileInfo(panel->session()->program()).fileName(); }));
    auto* area = window.findChild<QMainWindow*>("editorArea"); assert(area);
    area->resizeDocks({panel}, {200}, Qt::Vertical);
    checkLayout();
    assert(bounds(sidebar) == sidebarBefore);
    window.resize(950, 650); checkLayout();
    workspace->setSizes({220, 730}); checkLayout();
    window.resize(1200, 850); checkLayout();
    auto* terminal = panel->terminal();
    assert(terminal->font().pointSize() == 17);
    assert(waitFor([&] { return panel->session()->processId() > 0; }));
    const auto pid = panel->session()->processId();
    assert(terminal->hasFocus());
    panel->session()->write("printf '\\120\\122\\117\\112\\105\\103\\124\\n'; pwd\n");
    const bool projectReady = waitFor([&] { return terminal->screenText().contains("PROJECT\n") && terminal->screenText().contains(project); });
    if (!projectReady) qDebug() << "Terminal" << terminal->rows() << terminal->columns() << terminal->screenText();
    if (!qEnvironmentVariableIsEmpty("TURTLE_CODE_TERMINAL_SCREENSHOT")) {
        assert(projectReady);
        assert(window.grab().save(qEnvironmentVariable("TURTLE_CODE_TERMINAL_SCREENSHOT")));
        closeDiscard(window); return;
    }
    assert(projectReady);
    QTest::keyClick(terminal, Qt::Key_QuoteLeft, Qt::ControlModifier);
    assert(!panel->isVisible());
    assert(editor->hasFocus());
    panel->session()->write("printf '\\110\\111\\104\\104\\105\\116\\n'\n");
    assert(waitFor([&] { return terminal->screenText().contains("HIDDEN\n"); }));
    QTest::keyClick(editor, Qt::Key_QuoteLeft, Qt::ControlModifier);
    assert(panel->isVisible() && panel->session()->processId() == pid && terminal->hasFocus());
    assert(terminal->screenText().contains("HIDDEN\n"));
    checkLayout();
    // The configured key is honoured both in the editor and in the terminal.
    QString error;
    assert(ShortcutManager::instance().save({{"view.terminal", QKeySequence("Ctrl+Alt+T")}}, &error));
    QTest::keyClick(terminal, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    assert(!panel->isVisible());
    QTest::keyClick(editor, Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
    assert(panel->isVisible() && terminal->hasFocus());
    panel->findChild<QToolButton*>("terminalClose")->click(); assert(!panel->isVisible());
    assert(ShortcutManager::instance().save({}, &error));
    toggle->trigger();
    // Exiting leaves the output on screen, and Restart creates a fresh shell.
    panel->session()->write("exit\n");
    assert(waitFor([&] { return !panel->session()->isRunning() && terminal->screenText().contains("Process exited"); }));
    panel->findChild<QToolButton*>("terminalRestart")->click();
    assert(waitFor([&] { return panel->session()->processId() > 0; }));
    // Project changes restart visible and hidden shells in the actual new cwd.
    // Metacharacters in folder names must never be interpreted as shell input.
    const auto second = directory.filePath("project ' \" $() ; Ω");
    const auto third = directory.filePath("third project");
    assert(QDir().mkpath(second) && QDir().mkpath(third));
    auto checkDirectory = [&](const QString& expected, const QByteArray& marker) {
        assert(waitFor([&] { return panel->session()->processId() > 0; }));
        QByteArray output;
        const auto connection = QObject::connect(panel->session(), &TerminalSession::output,
            &window, [&](const QByteArray& bytes) { output += bytes; });
        // Encode the marker so echoed input cannot satisfy the assertion.
        QByteArray encoded;
        for (const auto byte : marker) encoded += "\\" + QByteArray::number(static_cast<unsigned char>(byte), 8).rightJustified(3, '0');
        panel->session()->write("printf '" + encoded + "\\n'; pwd\n");
        assert(waitFor([&] { return output.contains(marker + "\r\n" + expected.toUtf8() + "\r\n"); }));
        QObject::disconnect(connection);
    };
    auto previousPid = panel->session()->processId();
    window.openPaths({second});
    assert(waitFor([&] { return panel->session()->processId() > 0 && panel->session()->processId() != previousPid; }));
    assert(panel->isVisible()); checkDirectory(second, "SWITCHED");
    previousPid = panel->session()->processId();
    window.openPaths({second}); QApplication::processEvents();
    assert(panel->session()->processId() == previousPid);
    panel->hide(); editor->setFocus();
    window.openPaths({project}); window.openPaths({third});
    assert(waitFor([&] { return panel->session()->processId() > 0 && panel->session()->processId() != previousPid; }));
    assert(!panel->isVisible() && editor->hasFocus());
    checkDirectory(third, "LATEST");
    toggle->trigger(); assert(panel->isVisible());
    checkDirectory(third, "REOPENED");
    // Removing a root also updates the terminal and clears the explicit project.
    previousPid = panel->session()->processId();
    auto* explorer = window.findChild<ProjectExplorer*>(); assert(explorer);
    explorer->setRoots({}); explorer->rootsChanged({});
    assert(waitFor([&] { return panel->session()->processId() > 0 && panel->session()->processId() != previousPid; }));
    checkDirectory(QDir::homePath(), "REMOVED");
    closeDiscard(window);
    assert(waitFor([&] { return !panel->session()->isRunning(); }));
}

#include "LspWindowTests.hpp"
#include "DebugWindowTests.hpp"

int main(int argc,char**argv) {
    QTemporaryDir directory; assert(directory.isValid());
    qputenv("XDG_CONFIG_HOME",directory.path().toUtf8());
    qputenv("XDG_DATA_HOME",directory.filePath("data").toUtf8());
    qputenv("SHELL", "/bin/sh"); // Do not source the user's shell configuration or write its history in tests.
    QApplication app(argc,argv); app.setOrganizationName("TurtleCodeTests"); app.setApplicationName("WindowTests");
    app.setQuitOnLastWindowClosed(false);
    QSettings().setValue("lsp/enabled",false); QSettings().setValue("lsp/autoCompletion",false);
    LanguageRegistry::setEnabled("cpp",false);
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--debug-native"){debugNativeWindowTests();return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--debug"){debugWindowTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--lsp"){lspWindowTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--terminal"){terminalWindowTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--git"){gitWindowTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--project-scale"){projectScaleWindowTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--commands"){commandPaletteTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--formatting"){formattingTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--memory-workload") return memoryWorkload(argc,argv);
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--external-files"){externalFileTests(directory);return 0;}
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--safety"){safetyTests(directory);return 0;}
    if (argc>1 && QString::fromLocal8Bit(argv[1])=="--russian-ui") {
        assert(I18n::saveLanguage("ru")); assert(I18n::initialize());
        QSettings().setValue("appearance/theme","tokyo-night");
        ThemeManager::instance().initialize();
        MainWindow window; window.show();
        auto* minimap=window.findChild<QAction*>("view.minimap");
        assert(minimap && minimap->isChecked() && minimap->text()==QString::fromUtf8("Мини-карта"));
        const auto mapPath=directory.filePath("minimap.txt");
        { QFile file(mapPath); assert(file.open(QIODevice::WriteOnly)); file.write("sample\n"); }
        window.openPaths({mapPath});
        auto* editor=window.findChild<EditorWidget*>();
        assert(editor && editor->miniMapEnabled());
        minimap->trigger();
        assert(!editor->miniMapEnabled() && !QSettings().value("editor/minimap",true).toBool());
        {
            MainWindow second;
            auto* secondAction=second.findChild<QAction*>("view.minimap");
            assert(secondAction && !secondAction->isChecked());
            second.openPaths({mapPath});
            assert(!second.findChild<EditorWidget*>()->miniMapEnabled());
            secondAction->trigger();
            assert(editor->miniMapEnabled() && minimap->isChecked());
        }
        bool fileMenu=false,settingsMenu=false;
        for(auto* action:window.menuBar()->actions()) {
            fileMenu |= action->text()==QString::fromUtf8("&Файл");
            settingsMenu |= action->text()==QString::fromUtf8("&Настройки");
        }
        assert(fileMenu && settingsMenu);
        QAction *languageAction=nullptr,*shortcutsAction=nullptr,*lspAction=nullptr;
        for(auto* action:window.findChildren<QAction*>()) {
            if(action->text()==QString::fromUtf8("Язык интерфейса...")) languageAction=action;
            if(action->text()==QString::fromUtf8("Горячие клавиши...")) shortcutsAction=action;
            if(action->text()==QString::fromUtf8("Языковой сервер...")) lspAction=action;
        }
        assert(languageAction && shortcutsAction && lspAction);
        bool checkedLanguage=false;
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
            assert(dialog->windowTitle()==QString::fromUtf8("Язык интерфейса"));
            auto* combo=dialog->findChild<QComboBox*>("interfaceLanguage"); assert(combo && combo->currentData().toString()=="ru");
            auto* buttons=dialog->findChild<QDialogButtonBox*>(); assert(buttons);
            auto save=buttons->button(QDialogButtonBox::Save)->text(); save.remove('&');
            assert(save==QString::fromUtf8("Сохранить")); checkedLanguage=true; dialog->reject();
        });
        languageAction->trigger(); assert(checkedLanguage);
        bool checkedShortcuts=false;
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
            auto* table=dialog->findChild<QTableWidget*>(); assert(table);
            assert(table->item(0,0)->text()==QString::fromUtf8("Файл"));
            assert(table->item(0,1)->text()==QString::fromUtf8("Открыть файл"));
            assert(table->item(0,2)->text().contains('O')); checkedShortcuts=true; dialog->reject();
        });
        shortcutsAction->trigger(); assert(checkedShortcuts);
        bool checkedLsp=false;
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
            bool found=false;
            for(auto* field:dialog->findChildren<QCheckBox*>()) found |= field->text()==QString::fromUtf8("Автоматически показывать подсказки при вводе");
            assert(found); checkedLsp=true; dialog->reject();
        });
        lspAction->trigger(); assert(checkedLsp);
        QAction* editorAction=nullptr;
        for (auto* action : window.findChildren<QAction*>())
            if (action->text()==I18n::text("Editor...")) editorAction=action;
        assert(editorAction);
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
            if(qEnvironmentVariableIsSet("TURTLE_CODE_BACKGROUND_PREVIEW"))
                dialog->grab().save(qEnvironmentVariable("TURTLE_CODE_BACKGROUND_PREVIEW")+".editor.png");
            dialog->reject();
        });
        editorAction->trigger();
        closeDiscard(window); return 0;
    }

    auto write=[&](const QString& name) {
        const auto path=directory.filePath(name); QFile file(path); assert(file.open(QIODevice::WriteOnly));
        assert(file.write("int value = 1;\n")>0); return path;
    };
    {
        const auto project=directory.filePath("editorconfig-project");assert(QDir().mkpath(project));
        {QFile config(project+"/.editorconfig");assert(config.open(QIODevice::WriteOnly));
         config.write("root=true\n[*]\nindent_style=space\nindent_size=2\nend_of_line=lf\ntrim_trailing_whitespace=true\ninsert_final_newline=true\n");}
        const auto path=project+"/sample.txt";const QByteArray original="x  \r\ny\t";
        {QFile file(path);assert(file.open(QIODevice::WriteOnly));file.write(original);}
        MainWindow window;window.show();window.activateWindow();window.openPaths({path});
        auto* editor=window.findChild<EditorWidget*>();assert(editor);
        assert(waitFor([&]{return !editor->isBusy();}));
        editor->setFocus();QApplication::processEvents();
        QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);
        assert(waitFor([&]{QFile file(path);return !editor->isBusy() && file.open(QIODevice::ReadOnly) && file.readAll()=="x\ny\n";}));
        assert(editor->document()->buffer().text()=="x\ny\n" && !editor->isModified());
        editor->documentUndo();assert(waitFor([&]{return !editor->isBusy();}));
        assert(editor->document()->buffer().text()==original.toStdString() && editor->isModified());
        closeDiscard(window);
    }
    {
        MainWindow window;window.resize(1100,720);window.show();window.activateWindow();
        window.openPaths({write("sidebar.cpp")});
        auto* editor=window.findChild<EditorWidget*>();assert(editor);
        assert(waitFor([&]{return !editor->isBusy();}));
        auto* stack=window.findChild<QStackedWidget*>("sidebarStack");assert(stack);
        auto* sidebar=stack->parentWidget();
        auto* bar=window.findChild<QWidget*>("activityBar");assert(bar);
        auto* splitter=window.findChild<QSplitter*>("workspaceSplitter");assert(splitter);
        editor->setFocus();QApplication::processEvents();
        const int expanded=splitter->widget(1)->width();
        QTest::keyClick(&window,Qt::Key_B,Qt::ControlModifier);QApplication::processEvents();
        assert(sidebar->isHidden() && bar->isVisible());
        assert(splitter->widget(1)->width()>expanded);
        QTest::keyClick(&window,Qt::Key_B,Qt::ControlModifier);QApplication::processEvents();
        assert(sidebar->isVisible() && stack->currentIndex()==0);
        const QStringList buttons{"explorerActivity","searchActivity","gitActivity","debugActivity"};
        for(int i=0;i<buttons.size();++i) {
            // Begin hidden, then reopen each page using its permanent icon.
            if(!sidebar->isHidden())window.findChild<QAction*>("view.explorer")->trigger();
            auto* button=window.findChild<QToolButton*>(buttons[i]);assert(button);
            QTest::mouseClick(button,Qt::LeftButton);QApplication::processEvents();
            assert(sidebar->isVisible() && stack->currentIndex()==i && button->isChecked());
            QTest::mouseClick(button,Qt::LeftButton);QApplication::processEvents();
            assert(sidebar->isHidden() && bar->isVisible() && !button->isChecked());
            // Ctrl+B restores the last page, including Search/Git/Debug.
            QTest::keyClick(&window,Qt::Key_B,Qt::ControlModifier);QApplication::processEvents();
            assert(sidebar->isVisible() && stack->currentIndex()==i && button->isChecked());
        }
        QTest::mouseClick(window.findChild<QToolButton*>("explorerActivity"),Qt::LeftButton);
        assert(sidebar->isVisible() && stack->currentIndex()==0);
        assert(!editor->isModified());closeDiscard(window);
    }
    const auto cpp=write("test.cpp"), text=write("test.txt");
    {
        assert(QDir().mkpath(directory.filePath("quick-project")));
        assert(QDir().mkpath(directory.filePath("quick-project-other")));
        assert(QDir().mkpath(directory.filePath("quick-second")));
        const auto inside=write("quick-project/inside.txt");
        const auto indexed=write("quick-project/indexed.txt");
        const auto outside=write("quick-project-other/outside-only-Ω.txt");
        const auto second=write("quick-second/second.txt");
        QSettings().setValue("editor/recentFiles",QStringList{outside,inside,second});
        const auto checkQuickOpen=[&](MainWindow& window, QStringList expected) {
            QAction* action=nullptr;
            for (auto* candidate:window.findChildren<QAction*>())
                if (candidate->text()==QStringLiteral("Quick Open...")) action=candidate;
            assert(action);
            bool checked=false;
            QTimer::singleShot(0,[&] {
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
                auto* input=dialog->findChild<QLineEdit*>();
                auto* results=dialog->findChild<QListWidget*>(); assert(input && results);
                assert(waitFor([&] { return results->property("indexing").isValid() && !results->property("indexing").toBool(); }));
                QStringList actual;
                for (int i=0;i<results->count();++i) actual.push_back(results->item(i)->data(Qt::UserRole).toString());
                actual.sort(); expected.sort(); assert(actual==expected);
                // Use a unique character: fuzzy matching absolute paths must not
                // accidentally match the randomized temporary-directory suffix.
                input->setText(QString::fromUtf8("Ω"));
                assert(results->count()==0);
                checked=true; dialog->reject();
            });
            action->trigger(); assert(checked);
        };
        MainWindow window; window.show();
        // The default home-directory explorer must not become a quick-open project.
        checkQuickOpen(window,{});
        window.openPaths({inside});
        assert(waitFor([&] {
            const auto editors=window.findChildren<EditorWidget*>();
            return editors.size()==1 && !editors.front()->isBusy();
        }));
        checkQuickOpen(window,{inside});
        window.openPaths({directory.filePath("quick-project"),directory.filePath("quick-second"),outside});
        assert(waitFor([&] {
            const auto editors=window.findChildren<EditorWidget*>();
            return editors.size()==2 && std::none_of(editors.begin(),editors.end(),[](auto* editor) { return editor->isBusy(); });
        }));
        // Even open and recent files outside the workspace must stay out of results.
        checkQuickOpen(window,{inside,indexed});
        window.openPaths({directory.filePath("quick-second")});
        checkQuickOpen(window,{second});
    }
    {
        MainWindow window;
        auto* explorer=window.findChild<ProjectExplorer*>();
        assert(explorer && explorer->topLevelItemCount()>0 && explorer->topLevelItem(0)->isExpanded());
        window.show(); closeDiscard(window);
    }
    {
        assert(QDir().mkpath(directory.filePath("nested")));
        ProjectExplorer explorer; explorer.setRoots({directory.path()});
        assert(explorer.topLevelItemCount()==1 && explorer.topLevelItem(0)->isExpanded());
        assert(waitFor([&] {
            const auto* root=explorer.topLevelItem(0);
            for (int i=0;i<root->childCount();++i) if (root->child(i)->data(0,Qt::UserRole).toString()==cpp) return true;
            return false;
        }));
        const auto* root=explorer.topLevelItem(0);
        for (int i=0;i<root->childCount();++i)
            if (root->child(i)->data(0,Qt::UserRole).toString()==directory.filePath("nested")) assert(!root->child(i)->isExpanded());
        QTemporaryDir second; assert(second.isValid());
        explorer.setRoots({directory.path(), second.path()}); assert(explorer.topLevelItemCount()==1 && explorer.roots()==QStringList{directory.path()});
        explorer.setRoots({second.path()}); assert(explorer.topLevelItemCount()==1 && explorer.topLevelItem(0)->isExpanded());
    }
    {
        MainWindow window; window.show(); window.openPaths({cpp,text});
        assert(waitFor([&] {
            const auto editors=window.findChildren<EditorWidget*>();
            if (editors.size()!=2) return false;
            for (auto* editor:editors) if (editor->isBusy()) return false;
            return true;
        }));
        auto* client=window.findChild<LspClient*>(); assert(client);
        // Force an allocated diagnostic collection. The original destructor freed
        // it before child editor destruction emitted the diagnostic-clear signal.
        client->diagnostics(cpp,QJsonArray{QJsonObject{{"message","test"},
            {"range",QJsonObject{{"start",QJsonObject{{"line",0},{"character",0}}},{"end",QJsonObject{{"line",0},{"character",3}}}}}}});
        closeDiscard(window);
    }
    QApplication::processEvents();
    {
        MainWindow window; window.show(); window.openPaths({cpp,text});
        // Close while file-loading callbacks are still queued.
        closeDiscard(window);
    }
    QApplication::processEvents();
    {
        const auto other=write("other.cpp");
        QSettings settings;
        settings.setValue("lsp/enabled",true);
        settings.setValue("lsp/clangd/command",QStandardPaths::findExecutable("python3"));
        settings.setValue("lsp/clangd/arguments",QString("\"%1\" --rename-other \"%2\"").arg(QString::fromUtf8(LSP_FAKE_SERVER),other));
        MainWindow window; window.show(); window.openPaths({cpp});
        auto* client=window.findChild<LspClient*>(); assert(client);
        assert(waitFor([&] {
            const auto editors=window.findChildren<EditorWidget*>();
            return editors.size()==1 && !editors.first()->isBusy() && client->ready();
        }));
        auto* source=window.findChild<EditorWidget*>(); assert(source);
        assert(waitFor([&] { return source->semanticTokenCount()>0; }));
        source->setNavigationPosition(1,5); source->setFocus(); window.activateWindow();
        QTest::qWait(30);
        int mode=0; bool canceled=false,staleRejected=false; QPointer<EditorWidget> target;
        QTimer responder;
        QObject::connect(&responder,&QTimer::timeout,[&] {
            auto* modal=QApplication::activeModalWidget(); if (!modal) return;
            if (auto* input=qobject_cast<QInputDialog*>(modal)) {
                if (mode==1) { canceled=true; input->reject(); }
                else { input->setTextValue("renamed_value"); input->accept(); }
            } else if (auto* message=qobject_cast<QMessageBox*>(modal)) {
                if (mode==2 && message->windowTitle()=="Rename not applied") { staleRejected=true; message->accept(); }
                else qFatal("Unexpected rename error: %s",qPrintable(message->text()));
            } else if (auto* dialog=qobject_cast<QDialog*>(modal); dialog && dialog->findChild<QTreeWidget*>()) {
                if (mode==2) { assert(target); target->applyWorkspaceEdits({EditChange{0,"","// concurrent edit\n"}}); }
                dialog->accept();
            }
        });
        responder.start(10);
        QTest::keyClick(source,Qt::Key_F2);
        assert(waitFor([&] { return source->document()->buffer().text()=="int renamed_value = 1;\n"; }));
        for (auto* editor:window.findChildren<EditorWidget*>()) if (editor->filePath()==other) target=editor;
        assert(target && target->document()->buffer().text()=="int renamed_value = 1;\n");
        assert(source->isModified() && target->isModified());
        for (const auto& path:{cpp,other}) { QFile file(path); assert(file.open(QIODevice::ReadOnly)); assert(file.readAll()=="int value = 1;\n"); }
        QAction* saveAll=nullptr;
        for (auto* action:window.findChildren<QAction*>()) if (action->text().startsWith("Save All")) saveAll=action;
        assert(saveAll && saveAll->shortcut()==QKeySequence("Ctrl+Alt+S"));
        saveAll->trigger();
        assert(waitFor([&] { return !source->isBusy() && !target->isBusy() && !source->isModified() && !target->isModified(); }));
        for (const auto& path:{cpp,other}) { QFile file(path); assert(file.open(QIODevice::ReadOnly)); assert(file.readAll()=="int renamed_value = 1;\n"); }
        QTest::keyClick(source,Qt::Key_Z,Qt::ControlModifier);
        QTest::keyClick(target,Qt::Key_Z,Qt::ControlModifier);
        assert(source->document()->buffer().text()=="int value = 1;\n");
        assert(target->document()->buffer().text()=="int value = 1;\n");
        QAction* renameAction=nullptr;
        for (auto* action:window.findChildren<QAction*>()) if (action->text().startsWith("Rename symbol")) renameAction=action;
        assert(renameAction);
        mode=1; source->setNavigationPosition(1,5); source->setFocus();
        renameAction->trigger(); assert(waitFor([&] { return canceled; }));
        assert(source->document()->buffer().text()=="int value = 1;\n");
        mode=2; source->setFocus(); renameAction->trigger();
        assert(waitFor([&] { return staleRejected; }));
        assert(source->document()->buffer().text()=="int value = 1;\n");
        assert(target->document()->buffer().text()=="// concurrent edit\nint value = 1;\n");
        responder.stop();
        QAction* lspSettings=nullptr;
        for(auto* action:window.findChildren<QAction*>()) if(action->text()=="Language server...") lspSettings=action;
        assert(lspSettings);
        for(bool enabled:{false,true}) {
            QTimer::singleShot(0,[&,enabled] {
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
                auto* toggle=dialog->findChild<QCheckBox*>("semanticHighlighting"); assert(toggle);
                toggle->setChecked(enabled);
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            });
            lspSettings->trigger();
            assert(QSettings().value("lsp/semanticHighlighting").toBool()==enabled);
            if(!enabled) for(auto* editor:window.findChildren<EditorWidget*>()) assert(editor->semanticTokenCount()==0);
            else assert(waitFor([&] {
                for(auto* editor:window.findChildren<EditorWidget*>()) if(editor->semanticTokenCount()>0) return true;
                return false;
            }));
        }
        closeDiscard(window);
    }
    {
        const auto largePath=directory.filePath("adaptive.cpp");
        QByteArray content; while(content.size()<3*1024*1024) content+="int value = 1;\n";
        { QFile file(largePath); assert(file.open(QIODevice::WriteOnly)); assert(file.write(content)==content.size()); }
        for(bool ranged:{false,true}) {
            QSettings settings; settings.setValue("lsp/enabled",true); settings.setValue("lsp/semanticHighlighting",true);
            settings.setValue("lsp/clangd/command",QStandardPaths::findExecutable("python3"));
            settings.setValue("lsp/clangd/arguments",QString("\"%1\" --compact-hover %2").arg(QString::fromUtf8(LSP_FAKE_SERVER),ranged?"--semantic-range":""));
            MainWindow window; window.show(); window.openPaths({largePath});
            auto* client=window.findChild<LspClient*>(); assert(client);
            assert(waitFor([&]{auto* editor=window.findChild<EditorWidget*>();return editor && !editor->isBusy() && client->ready();}));
            auto* editor=window.findChild<EditorWidget*>(); assert(editor->lspEligible());
            if(ranged) {
                assert(waitFor([&]{return editor->semanticTokenCount()>0;}));
                editor->verticalScrollBar()->setValue(50000);
                const bool scrolled=waitFor([&]{return editor->semanticTokenCount()>0 && editor->semanticSnapshot().contains(50000);});
                if(!scrolled) qDebug()<<"adaptive debug"<<editor->verticalScrollBar()->value()<<editor->semanticTokenCount()<<editor->property("semanticPending")
                    <<editor->property("semanticAppliedWindow")<<editor->property("semanticPendingWindow")<<editor->property("semanticLimitedNotice")<<editor->property("semanticLatencyMs")<<editor->lspPolicy().remainingPause(QDateTime::currentMSecsSinceEpoch())<<client->log();
                assert(scrolled);
                for(auto line:editor->semanticSnapshot().keys()) assert(line>=49920 && line<50250);
            } else {
                QTest::qWait(400); assert(editor->semanticTokenCount()==0);
            }
            bool counted=false;
            client->hover(largePath,0,0,[&](QJsonValue result) {
                const auto methods=result.toObject().value("semanticMethods").toArray();
                if(ranged) { assert(!methods.isEmpty()); for(const auto& method:methods) assert(method.toString()=="textDocument/semanticTokens/range"); }
                else assert(methods.isEmpty());
                counted=true;
            });
            assert(waitFor([&]{return counted;}));
            QAction* definition=nullptr;
            for(auto* action:window.findChildren<QAction*>()) if(action->text()=="Go to Definition") definition=action;
            assert(definition); editor->setNavigationPosition(1,1); definition->trigger();
            assert(waitFor([&]{return editor->lspPosition().x()==1;}));
            closeDiscard(window);
        }
    }
    {
        const auto deferred=directory.filePath("manual-large.cpp");
        QByteArray content="/*\n"; const QByteArray padding="large document padding line\n";
        while(content.size()<17*1024*1024) content+=padding;
        content+="*/\nint value = 1;\n";
        { QFile file(deferred); assert(file.open(QIODevice::WriteOnly)); assert(file.write(content)==content.size()); }
        QSettings().setValue("lsp/clangd/arguments",QString("\"%1\" --compact-hover").arg(QString::fromUtf8(LSP_FAKE_SERVER)));
        MainWindow window; window.show(); window.openPaths({deferred});
        auto* client=window.findChild<LspClient*>(); assert(client);
        assert(waitFor([&]{auto* editor=window.findChild<EditorWidget*>();return editor && !editor->isBusy();}));
        assert(client->documentVersion(deferred)==0 && !client->ready());
        auto* editor=window.findChild<EditorWidget*>(); assert(editor->lspEligible() && !editor->lspAutomaticEligible());
        QAction* definition=nullptr;
        for(auto* action:window.findChildren<QAction*>()) if(action->text()=="Go to Definition") definition=action;
        assert(definition); definition->trigger();
        assert(waitFor([&]{return client->ready() && editor->lspPosition().x()==1;}));
        closeDiscard(window);
    }

    {
        QSettings().setValue("lsp/enabled",false);
        const auto path=directory.filePath("chunked.txt");
        QByteArray original="\xEF\xBB\xBF";
        const QByteArray line=QString::fromUtf8("строка 😀\r\n").toUtf8();
        while(original.size()<5*1024*1024)original+=line;
        {QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(original)==original.size());}
        MainWindow window;window.show();window.openPaths({path});
        assert(waitFor([&]{auto* editor=window.findChild<EditorWidget*>();return editor && !editor->isBusy();}));
        auto* editor=window.findChild<EditorWidget*>();assert(editor->document()->buffer().isFileBacked());
        const auto location=static_cast<std::size_t>(original.size()-line.size());
        const auto replacement=QString::fromUtf8("изменено 😀\r\n").toUtf8().toStdString();
        editor->applyWorkspaceEdits({EditChange{location,line.toStdString(),replacement}});
        auto snapshot=editor->snapshotBuffer();assert(snapshot.isFileBacked());
        QAction* saveAll=nullptr;
        for(auto* action:window.findChildren<QAction*>())if(action->text().startsWith("Save All"))saveAll=action;
        assert(saveAll);saveAll->trigger();
        assert(waitFor([&]{return !editor->isBusy() && !editor->isModified();}));
        {QFile file(path);assert(file.open(QIODevice::ReadOnly));const auto saved=file.readAll();
            assert(saved==original.left(location)+QByteArray::fromStdString(replacement));}
        assert(snapshot.slice(location,replacement.size())==replacement);
        assert(editor->document()->buffer().isFileBacked());
        QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
        assert(editor->document()->buffer().slice(location,line.size())==line.toStdString());
        QTest::keyClick(editor,Qt::Key_Y,Qt::ControlModifier);
        assert(editor->document()->buffer().slice(location,replacement.size())==replacement);
        // Undo twice ends at the on-disk saved revision, allowing normal close.
        QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);saveAll->trigger();
        assert(waitFor([&]{return !editor->isBusy() && !editor->isModified();}));
        closeDiscard(window);
    }

    {
        MainWindow window;window.show();
        const auto third=write("third.txt");window.openPaths({cpp,text,third});
        auto* tabs=window.findChild<QTabWidget*>();assert(tabs && tabs->count()==3);
        assert(waitFor([&]{for(auto* editor:window.findChildren<EditorWidget*>())if(editor->isBusy())return false;return true;}));
        auto* bar=tabs->tabBar();
        const auto checkHeader=[&]{
            QApplication::processEvents();
            int visible=0;for(int i=0;i<bar->count();++i)visible+=bar->isTabVisible(i);
            assert(visible==(tabs->count()>0?1:0));
            if(tabs->count())assert(bar->isTabVisible(tabs->currentIndex()) && bar->tabRect(tabs->currentIndex()).isValid());
            assert(!tabs->tabsClosable() && !tabs->isMovable());
            if(auto* editor=qobject_cast<EditorWidget*>(tabs->currentWidget())) {
                const int tabLeft=bar->mapTo(&window,bar->tabRect(tabs->currentIndex()).topLeft()).x();
                const int gutterRight=editor->viewport()->mapTo(&window,QPoint(editor->lineNumberAreaWidth(),0)).x();
                assert(tabLeft==gutterRight);
                auto* gutter=tabs->findChild<QWidget*>("tabGutter");assert(gutter && gutter->isVisible());
                assert(gutter->mapTo(&window,QPoint(gutter->width(),0)).x()==gutterRight);
                assert(!gutter->geometry().intersects(bar->geometry()));
                const auto gutterPoint=bar->mapFromGlobal(gutter->mapToGlobal(gutter->rect().center()));
                assert(bar->tabAt(gutterPoint)==-1);
            }
        };
        checkHeader();QWidget* initial=tabs->currentWidget();
        QTest::keyPress(initial,Qt::Key_Tab,Qt::ControlModifier);
        auto* popup=window.findChild<QFrame*>("openFileSwitcher");
        auto* list=window.findChild<QListWidget*>("openFileSwitchList");
        assert(popup && popup->isVisible() && list && list->count()==3 && list->currentRow()==1);
        assert(popup->findChildren<QLabel*>().isEmpty());
        for(int row=0;row<list->count();++row) {
            assert(!list->item(row)->text().contains('\n') && !list->item(row)->text().contains('/') && list->item(row)->toolTip().isEmpty());
        }
        assert(tabs->currentWidget()==initial && std::abs(popup->geometry().center().x()-window.rect().center().x())<=1);
        assert(std::abs(popup->geometry().center().y()-window.height()/3)<=1);
        QTest::keyPress(list,Qt::Key_Tab,Qt::ControlModifier);assert(list->currentRow()==2);
        QTest::keyRelease(list,Qt::Key_Control);
        assert(!popup->isVisible() && tabs->currentWidget()!=initial);checkHeader();
        auto* selected=tabs->currentWidget();
        QTest::keyPress(selected,Qt::Key_Backtab,Qt::ControlModifier|Qt::ShiftModifier);
        assert(popup->isVisible() && list->currentRow()==2);
        QTest::keyClick(list,Qt::Key_Escape);
        assert(!popup->isVisible() && tabs->currentWidget()==selected);
        QTest::keyPress(selected,Qt::Key_Tab,Qt::ControlModifier);
        QTest::keyClick(list->viewport(),Qt::Key_Return);
        assert(!popup->isVisible() && tabs->currentWidget()==initial);checkHeader();
        const auto contextAction=[&](const QString& name) {
            QApplication::processEvents();
            QTimer::singleShot(0,[&]{
                auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());assert(menu && menu->objectName()=="currentTabMenu");
                QAction* action=nullptr;for(auto* candidate:menu->actions())if(candidate->objectName()==name)action=candidate;
                assert(action);menu->setActiveAction(action);QTest::keyClick(menu,Qt::Key_Return);
            });
            bar->customContextMenuRequested(bar->tabRect(tabs->currentIndex()).center());
        };
        contextAction("closeCurrentTab");assert(tabs->count()==2);checkHeader();
        auto* editor=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(editor);
        QTest::keyClick(editor,Qt::Key_X);assert(editor->isModified());
        // Rejecting Close All must leave every document open.
        QTimer reject;reject.setInterval(1);QObject::connect(&reject,&QTimer::timeout,[&]{
            if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))message->button(QMessageBox::Cancel)->click();
        });reject.start();contextAction("closeAllTabs");reject.stop();
        assert(tabs->count()==2);checkHeader();
        QTimer confirm;confirm.setInterval(1);QObject::connect(&confirm,&QTimer::timeout,[&]{
            if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))message->button(QMessageBox::Discard)->click();
        });confirm.start();contextAction("closeAllTabs");confirm.stop();assert(tabs->count()==0);checkHeader();
        window.openPaths({text});assert(tabs->count()==1);checkHeader();
        assert(waitFor([&]{return !qobject_cast<EditorWidget*>(tabs->currentWidget())->isBusy();}));
        window.activateWindow();tabs->currentWidget()->setFocus();QApplication::processEvents();
        QTest::keyPress(tabs->currentWidget(),Qt::Key_Tab,Qt::ControlModifier);
        assert(popup->isVisible() && list->count()==1 && list->currentRow()==0);
        QTest::keyRelease(list,Qt::Key_Control);assert(!popup->isVisible());checkHeader();closeDiscard(window);
    }

    {
        QSettings().setValue("appearance/theme","tokyo-night");
        ThemeManager::instance().initialize();
        MainWindow window;window.show();QAction* settingsAction=nullptr;
        MainWindow other;other.show();
        window.openPaths({text});other.openPaths({text});
        auto* editor=window.findChild<EditorWidget*>();auto* otherEditor=other.findChild<EditorWidget*>();
        assert(editor && otherEditor && waitFor([&]{return !editor->isBusy() && !otherEditor->isBusy();}));
        window.findChild<QAction*>("view.terminal")->trigger();
        other.findChild<QAction*>("view.terminal")->trigger();
        const auto original=editor->document()->buffer().text();const auto revision=editor->lspRevision();
        const auto family=QFontInfo(QFontDatabase::systemFont(QFontDatabase::FixedFont)).family();
        for(auto* action:window.findChildren<QAction*>())if(action->text()=="Editor...")settingsAction=action;
        assert(settingsAction);QTimer::singleShot(0,[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);
            auto* memory=dialog->findChild<QSpinBox*>("historyMemoryMiB");auto* disk=dialog->findChild<QSpinBox*>("historyDiskMiB");auto* spill=dialog->findChild<QCheckBox*>("historySpillToDisk");
            assert(memory && disk && spill && spill->isChecked());memory->setValue(8);disk->setValue(16);spill->setChecked(false);assert(!disk->isEnabled());spill->setChecked(true);
            for(const auto& name:{"tree","editor","terminal"}) {
                auto* combo=dialog->findChild<QFontComboBox*>(QString(name)+"FontFamily");assert(combo);
                combo->setCurrentFont(QFont(family));
            }
            auto* treeSize=dialog->findChild<QSpinBox*>("treeFontSize");
            QStyleOptionSpinBox spinOption; spinOption.initFrom(treeSize); spinOption.rect=treeSize->rect();
            treeSize->setValue(18);
            const auto up=treeSize->style()->subControlRect(QStyle::CC_SpinBox,&spinOption,QStyle::SC_SpinBoxUp,treeSize);
            const auto down=treeSize->style()->subControlRect(QStyle::CC_SpinBox,&spinOption,QStyle::SC_SpinBoxDown,treeSize);
            QTest::mouseClick(treeSize,Qt::LeftButton,Qt::NoModifier,up.center()); assert(treeSize->value()==19);
            QTest::mouseClick(treeSize,Qt::LeftButton,Qt::NoModifier,down.center()); assert(treeSize->value()==18);
            dialog->findChild<QSpinBox*>("editorFontSize")->setValue(24);
            dialog->findChild<QSpinBox*>("terminalFontSize")->setValue(16);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });settingsAction->trigger();
        assert(QSettings().value("editor/historyMemoryMiB").toInt()==8 && QSettings().value("editor/historyDiskMiB").toInt()==16);
        for(auto* item:{editor,otherEditor}) {
            assert(item->font().family()==family && item->font().pointSize()==24);
            assert(item->verticalScrollBar()->pageStep()==std::max(1,item->viewport()->height()/(item->fontMetrics().height()+3)));
        }
        for(auto* item:{&window,&other}) {
            auto* tree=item->findChild<ProjectExplorer*>();assert(tree && tree->font().family()==family && tree->font().pointSize()==18);
        }
        for(auto* item:{&window,&other}) {
            auto* terminal=item->findChild<TerminalWidget*>();assert(terminal);
            assert(terminal->font().family()==family && terminal->font().pointSize()==16);
        }
        assert(editor->document()->buffer().text()==original && editor->lspRevision()==revision && !editor->isModified());
        assert(QSettings().value("fonts/terminal/family").toString()==family && QSettings().value("fonts/terminal/size").toInt()==16);
        // Font-only and unrelated saves must not rebuild the theme or corrupt the tree palette.
        int themeChanges=0;
        const auto themeConnection=QObject::connect(&ThemeManager::instance(),&ThemeManager::themeChanged,
            &window,[&]{++themeChanges;});
        for (int size : {26,22,24}) {
            QTimer::singleShot(0,[&,size]{
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
                dialog->findChild<QSpinBox*>("editorFontSize")->setValue(size);
                dialog->findChild<QSpinBox*>("treeFontSize")->setValue(size-6);
                auto* pairs=dialog->findChild<QCheckBox*>("autoPairs"); assert(pairs);
                pairs->setChecked(!pairs->isChecked());
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            });
            settingsAction->trigger(); QApplication::processEvents();
            assert(themeChanges==0);
            for (auto* item : {&window,&other}) {
                auto* tree=item->findChild<ProjectExplorer*>();
                assert(tree->font().pointSize()==size-6);
                assert(tree->palette().color(QPalette::Text)==ThemeManager::instance().color("text.primary"));
            }
        }
        QObject::disconnect(themeConnection);
        // Reopening loads saved values; cancellation leaves both settings and widgets intact.
        QTimer::singleShot(0,[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);
            auto* size=dialog->findChild<QSpinBox*>("editorFontSize");assert(size && size->value()==24);
            assert(dialog->findChild<QFontComboBox*>("editorFontFamily")->currentFont().family()==family);
            assert(dialog->findChild<QSpinBox*>("terminalFontSize")->value()==16);
            size->setValue(40);dialog->findChild<QSpinBox*>("terminalFontSize")->setValue(30);dialog->reject();
        });settingsAction->trigger();
        assert(QSettings().value("fonts/editor/size").toInt()==24 && editor->font().pointSize()==24);
        assert(window.findChild<TerminalWidget*>()->font().pointSize()==16);
        MainWindow fresh;fresh.openPaths({text});
        fresh.findChild<QAction*>("view.terminal")->trigger();
        assert(fresh.findChild<TerminalWidget*>()->font().pointSize()==16);
        auto* freshEditor=fresh.findChild<EditorWidget*>();assert(freshEditor && freshEditor->font().pointSize()==24 && freshEditor->font().family()==family);
        assert(fresh.findChild<ProjectExplorer*>()->font().pointSize()==18);
        assert(waitFor([&]{return !freshEditor->isBusy();}));
        closeDiscard(fresh);closeDiscard(other);closeDiscard(window);
    }

    {
        ThemeManager::instance().initialize();
        MainWindow window; window.resize(1100,720); window.show(); window.openPaths({text});
        auto* editor=window.findChild<EditorWidget*>();
        assert(editor && waitFor([&]{return !editor->isBusy();}));
        auto* tree=window.findChild<ProjectExplorer*>(); assert(tree); tree->clear();
        QWidget* workspace=&window;
        QApplication::processEvents();
        const QImage baseline=workspace->grab().toImage();
        QImage image(workspace->size(),QImage::Format_RGB32);
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
            image.setPixelColor(x,y,QColor(80+x%150,40+y%170,90));
        const auto imageFile=directory.filePath("background.png"); assert(image.save(imageFile));
        QAction* settingsAction=nullptr;
        for(auto* action:window.findChildren<QAction*>()) if(action->text()=="Editor...") settingsAction=action;
        assert(settingsAction);
        const auto saveBackground=[&](const QString& path,int opacity) {
            QTimer::singleShot(0,[&,path,opacity]{
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
                auto* field=dialog->findChild<QLineEdit*>("backgroundImage"); assert(field); field->setText(path);
                auto* spin=dialog->findChild<QSpinBox*>("backgroundOpacity"); assert(spin); spin->setValue(opacity);
                if(qEnvironmentVariableIsSet("TURTLE_CODE_BACKGROUND_PREVIEW")) dialog->grab().save(qEnvironmentVariable("TURTLE_CODE_BACKGROUND_PREVIEW")+".settings.png");
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            });
            settingsAction->trigger(); QApplication::processEvents();
        };
        saveBackground(imageFile,100);
        assert(QSettings().value("appearance/backgroundImage").toString()==imageFile);
        assert(WorkspaceBackground::instance().enabled());
        // Sample empty areas behind the tree, text, gutter and header. The
        // gradient must continue in workspace coordinates across all surfaces.
        const QVector<QPoint> samples{
            tree->viewport()->mapTo(workspace,QPoint(70,tree->viewport()->height()-45)),
            editor->viewport()->mapTo(workspace,QPoint(editor->viewport()->width()-60,editor->viewport()->height()-45)),
            editor->viewport()->mapTo(workspace,QPoint(2,editor->viewport()->height()-45)),
            window.findChild<QWidget*>("tabGutter")->mapTo(workspace,QPoint(2,2)),
            QPoint(window.width()-40,window.menuBar()->height()/2),
            QPoint(200,window.height()-10)};
        const auto checkImage=[&](int opacity) {
            const auto actual=workspace->grab().toImage();
            for(const auto& point:samples) {
                const auto expected=image.pixelColor(point);
                const auto base=baseline.pixelColor(point);
                const auto got=actual.pixelColor(point);
                const auto channel=[&](int foreground,int background) {return (foreground*opacity+background*(100-opacity))/100;};
                if(std::abs(got.red()-channel(expected.red(),base.red()))>2) { fprintf(stderr,"background sample opacity=%d at %d,%d got=%d,%d,%d expected=%d,%d,%d base=%d,%d,%d\n",opacity,point.x(),point.y(),got.red(),got.green(),got.blue(),expected.red(),expected.green(),expected.blue(),base.red(),base.green(),base.blue()); }
                assert(std::abs(got.red()-channel(expected.red(),base.red()))<=2);
                assert(std::abs(got.green()-channel(expected.green(),base.green()))<=2);
                assert(std::abs(got.blue()-channel(expected.blue(),base.blue()))<=2);
            }
        };
        checkImage(100);
        if(qEnvironmentVariableIsSet("TURTLE_CODE_BACKGROUND_PREVIEW")) window.grab().save(qEnvironmentVariable("TURTLE_CODE_BACKGROUND_PREVIEW"));
        saveBackground(imageFile,35); checkImage(35);
        // Reload uses persisted settings; cancelling a removal changes nothing.
        ThemeManager::instance().initialize(); checkImage(35);
        QTimer::singleShot(0,[&]{
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
            assert(dialog->findChild<QSpinBox*>("backgroundOpacity")->value()==35);
            dialog->findChild<QPushButton*>("removeBackgroundImage")->click(); dialog->reject();
        }); settingsAction->trigger(); checkImage(35);
        saveBackground(imageFile,0); checkImage(0);
        saveBackground(QString(),35); checkImage(0);
        assert(!WorkspaceBackground::instance().enabled());
        // A disappeared image safely falls back to the theme on restart.
        QSettings().setValue("appearance/backgroundImage",directory.filePath("missing.png"));
        ThemeManager::instance().refreshAppearance(); checkImage(0);
        QSettings().remove("appearance/backgroundImage");
        QSettings().remove("appearance/backgroundOpacity");
    }

    {
        QSettings().setValue("editor/autoPairs",true);
        MainWindow window;window.show();window.openPaths({text});
        auto* editor=window.findChild<EditorWidget*>();assert(editor && waitFor([&]{return !editor->isBusy();}));
        auto* line=window.findChild<QAction*>("editor.toggleLineComment");auto* block=window.findChild<QAction*>("editor.toggleBlockComment");
        assert(line && block && line->shortcut()==QKeySequence("Ctrl+/") && block->shortcut()==QKeySequence("Ctrl+Shift+A"));
        const auto original=editor->document()->buffer().text();
        window.activateWindow();editor->setFocus();QTest::qWait(30);
        QTest::keyClick(window.windowHandle(),Qt::Key_Tab);
        assert(editor->document()->buffer().text()=="    "+original && QApplication::focusWidget()==editor);
        QTest::keyClick(window.windowHandle(),Qt::Key_Tab,Qt::ShiftModifier);
        assert(editor->document()->buffer().text()==original && QApplication::focusWidget()==editor);
        line->trigger();
        assert(editor->document()->buffer().text()=="// "+original);QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
        QAction* settingsAction=nullptr;for(auto* action:window.findChildren<QAction*>())if(action->text()=="Editor...")settingsAction=action;
        assert(settingsAction);
        const auto setPairs=[&](bool enabled,bool save) {
            QTimer::singleShot(0,[&,enabled,save]{
                auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);
                auto* pairs=dialog->findChild<QCheckBox*>("autoPairs");assert(pairs);
                assert(dialog->findChild<QScrollArea*>());
                auto* buttons=dialog->findChild<QDialogButtonBox*>();assert(buttons && buttons->isVisible());
                assert(dialog->rect().contains(buttons->mapTo(dialog,buttons->rect().bottomRight())));
                assert(pairs->isChecked()==QSettings().value("editor/autoPairs",true).toBool());pairs->setChecked(enabled);
                if(save)dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();else dialog->reject();
            });settingsAction->trigger();
        };
        setPairs(false,true);assert(!QSettings().value("editor/autoPairs").toBool());
        QTest::keyClick(editor,Qt::Key_End,Qt::ControlModifier);QTest::keyClicks(editor,"(");assert(editor->document()->buffer().text()==original+"(");
        QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
        setPairs(true,false);assert(!QSettings().value("editor/autoPairs").toBool());
        setPairs(true,true);QTest::keyClick(editor,Qt::Key_End,Qt::ControlModifier);QTest::keyClicks(editor,"(");
        assert(editor->document()->buffer().text()==original+"()");QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
        closeDiscard(window);
    }

}
