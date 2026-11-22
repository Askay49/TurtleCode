#pragma once
#include "language/LspManager.hpp"
#include "language/LspSettingsDialog.hpp"
#include <QListWidget>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QDockWidget>

static void lspWindowTests(QTemporaryDir& directory) {
    auto configuration=LspConfiguration::load();
    auto& clangd=configuration.profiles[0].server;
    clangd.command=QStandardPaths::findExecutable("python3");
    clangd.arguments={LSP_FAKE_SERVER,"--any-language"};
    auto other=configuration.profiles[0];other.id="python";other.server.name="Python test";
    other.server.arguments={LSP_FAKE_SERVER,"--any-language","--no-format"};
    other.server.settings={{"custom",QJsonObject{{"test",1}}}};
    configuration.profiles.append(other);configuration.bindings["python"]="python";configuration.save();
    QSettings().setValue("lsp/enabled",true);QSettings().setValue("lsp/semanticHighlighting",true);
    QSettings().setValue("editor/recoveryEnabled",false);
    const auto cpp=directory.filePath("mixed.cpp"),py=directory.filePath("mixed.py");
    for(const auto& path:{cpp,py}){QFile file(path);assert(file.open(QIODevice::WriteOnly));file.write("int value = 1;\n");}
    MainWindow window;window.show();window.openPaths({cpp,py});
    auto* manager=window.findChild<LspManager*>();assert(manager);
    assert(waitFor([&]{return manager->ready(cpp)&&manager->ready(py);}));
    auto* tabs=window.findChild<QTabWidget*>();assert(tabs);
    EditorWidget *c=nullptr,*p=nullptr;
    for(auto* editor:window.findChildren<EditorWidget*>()){if(editor->filePath()==cpp)c=editor;else if(editor->filePath()==py)p=editor;}
    assert(c&&p);auto* clientC=manager->clientFor(cpp);auto* clientP=manager->clientFor(py);assert(clientC!=clientP);
    tabs->setCurrentWidget(c);assert(waitFor([&]{return c->semanticTokenCount()>0;}));
    tabs->setCurrentWidget(p);assert(waitFor([&]{return p->semanticTokenCount()>0;}));
    assert(manager->canFormat(cpp)&&!manager->canFormat(py));
    // Outline uses hierarchical C++ symbols and flat Python symbols, independently
    // of the editor minimap. Stale replies from another tab cannot replace it.
    auto* outline=window.findChild<QDockWidget*>("outlinePanel");
    auto* outlineAction=window.findChild<QAction*>("view.outline");
    auto* outlineTree=window.findChild<QTreeWidget*>("outlineTree");
    auto* outlineFilter=window.findChild<QLineEdit*>("outlineFilter");
    assert(outline && outlineAction && outlineTree && outlineFilter && outline->isVisible());
    tabs->setCurrentWidget(c);
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1
        && outlineTree->topLevelItem(0)->text(0).startsWith("Sample");}));
    auto* sample=outlineTree->topLevelItem(0); assert(sample->childCount()==1);
    outlineFilter->setText("VALUE"); assert(!sample->isHidden() && !sample->child(0)->isHidden());
    outlineFilter->setText("missing"); assert(sample->isHidden()); outlineFilter->clear();
    const auto bytes=c->document()->buffer().text();
    QTest::mouseClick(outlineTree->viewport(),Qt::LeftButton,Qt::NoModifier,
                      outlineTree->visualItemRect(sample->child(0)).center());
    assert(c->lspPosition()==QPoint(0,4) && c->document()->buffer().text()==bytes);
    tabs->setCurrentWidget(p);
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1
        && outlineTree->topLevelItem(0)->text(0)=="python_value";}));
    outlineAction->trigger(); assert(!outline->isVisible() && !QSettings().value("view/outline",true).toBool());
    assert(c->miniMapEnabled() && p->miniMapEnabled());
    {
        MainWindow second; second.show(); QApplication::processEvents();
        assert(!second.findChild<QDockWidget*>("outlinePanel")->isVisible());
        assert(!second.findChild<QAction*>("view.outline")->isChecked());
    }
    tabs->setCurrentWidget(c); QTest::qWait(500); assert(!outline->isVisible());
    outlineAction->trigger(); assert(outline->isVisible() && QSettings().value("view/outline").toBool());
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1
        && outlineTree->topLevelItem(0)->text(0).startsWith("Sample");}));
    outline->close();
    assert(!QSettings().value("view/outline",true).toBool());
    outlineAction->trigger();
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1;}));
    // Empty results and errors have distinct messages; document reloads refresh
    // the tree without applying a delayed reply belonging to another document.
    c->loadBuffer(PieceTreeBuffer("// outline_empty\n"),cpp);
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==0;}));
    c->loadBuffer(PieceTreeBuffer("// outline_error\n"),cpp);
    auto* outlineStatus=window.findChild<QLabel*>("outlineStatus");
    assert(waitFor([&]{return outlineStatus->text()=="Could not load outline.";}));
    c->loadBuffer(PieceTreeBuffer(bytes),cpp);
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1;}));
    tabs->setCurrentWidget(p); QTest::qWait(390); tabs->setCurrentWidget(c);
    assert(!outlineTree->isEnabled());
    assert(waitFor([&]{return outlineTree->isEnabled() && outlineTree->topLevelItemCount()==1
        && outlineTree->topLevelItem(0)->text(0).startsWith("Sample");}));
    if(!qEnvironmentVariableIsEmpty("TURTLE_CODE_OUTLINE_SCREENSHOT")) {
        QSettings().setValue("appearance/theme","tokyo-night");
        ThemeManager::instance().initialize(); window.resize(1400,850); QApplication::processEvents();
        if(auto* host=qobject_cast<QMainWindow*>(outline->parentWidget())) host->resizeDocks({outline},{280},Qt::Horizontal);
        QApplication::processEvents();
        window.grab().save(qEnvironmentVariable("TURTLE_CODE_OUTLINE_SCREENSHOT"));
    }
    tabs->setCurrentWidget(c); c->setNavigationPosition(1,1);
    // A mouse context menu targets the clicked symbol, even when the caret was elsewhere.
    const QPoint clicked(c->lineNumberAreaWidth()+12+QFontMetrics(c->font()).horizontalAdvance("int v"), QFontMetrics(c->font()).height()/2);
    QContextMenuEvent context(QContextMenuEvent::Mouse, clicked, c->viewport()->mapToGlobal(clicked));
    QApplication::sendEvent(c->viewport(), &context);
    auto* menu=c->findChild<QMenu*>("editorCodeContextMenu"); assert(menu && menu->isVisible());
    assert(menu->actions().size()==5 && c->lspPosition()==QPoint(0,5));
    const auto actions=menu->actions(); menu->close(); QApplication::processEvents();
    assert(actions[4]->shortcut()==QKeySequence(Qt::Key_F2));
    actions[1]->trigger();
    assert(waitFor([&]{return c->lspPosition()==QPoint(0,4);})); // LocationLink selects the name, not the full range.
    actions[2]->trigger(); assert(waitFor([&]{return c->lspPosition()==QPoint(0,0);}));
    c->setNavigationPosition(1,6); actions[3]->trigger();
    assert(waitFor([&]{return window.findChild<QDialog*>("symbolLocations")!=nullptr;}));
    auto* locations=window.findChild<QDialog*>("symbolLocations");
    auto* table=locations->findChild<QTableWidget*>(); assert(table && table->rowCount()==2);
    table->setCurrentCell(1,0);
    locations->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Open)->click();
    assert(waitFor([&]{return c->lspPosition()==QPoint(0,10);}));
    locations->close(); QApplication::processEvents();
    // The keyboard menu keeps the caret location.
    QContextMenuEvent keyboard(QContextMenuEvent::Keyboard, QPoint(), QPoint());
    QApplication::sendEvent(c->viewport(), &keyboard);
    menu=c->findChild<QMenu*>("editorCodeContextMenu"); assert(menu && menu->isVisible());
    assert(c->lspPosition()==QPoint(0,10)); menu->close();

    manager->restart("python");assert(waitFor([&]{return manager->ready(py);}));assert(manager->ready(cpp));
    assert(c->semanticTokenCount()>0); // Restarting Python does not clear C++ colors.
    {
        LspSettingsDialog dialog(manager,&window);dialog.show();
        auto* languages=dialog.findChild<QListWidget*>("lspLanguages");assert(languages && languages->count()==LanguageRegistry::languages().size());
        auto* profiles=dialog.findChild<QComboBox*>("lspProfile");assert(profiles);
        for(int i=0;i<languages->count();++i)if(languages->item(i)->data(Qt::UserRole).toString()=="python")languages->setCurrentRow(i);
        assert(profiles->currentData().toString()=="python");
        auto* command=dialog.findChild<QLineEdit*>("lspCommand");assert(command&&command->text()==other.server.command);
        auto* logLevel=dialog.findChild<QComboBox*>("lspLogLevel");assert(logLevel && logLevel->currentIndex()==1);
        auto* check=dialog.findChild<QPushButton*>("lspCheckConnection");assert(check);check->click();
        auto* status=dialog.findChild<QLabel*>("lspProfileStatus");assert(status);
        assert(waitFor([&]{return status->text().contains("Server capabilities:");}));
        assert(!status->text().contains("Formatting"));
        QTest::qWait(50);
        if(!qEnvironmentVariableIsEmpty("TURTLE_CODE_LSP_SCREENSHOT"))dialog.grab().save(qEnvironmentVariable("TURTLE_CODE_LSP_SCREENSHOT"));
        // Changing an assignment and then cancelling never changes settings/routes.
        profiles->setCurrentIndex(0);dialog.reject();
        assert(LspConfiguration::load().bindings.value("python")=="python" && manager->clientFor(py)==clientP);
    }
    {
        LspSettingsDialog dialog(manager,&window);dialog.show();
        auto* languages=dialog.findChild<QListWidget*>("lspLanguages");
        for(int i=0;i<languages->count();++i)if(languages->item(i)->data(Qt::UserRole).toString()=="yaml")languages->setCurrentRow(i);
        auto* profiles=dialog.findChild<QComboBox*>("lspProfile");profiles->setCurrentIndex(profiles->findData("python"));
        auto* id=dialog.findChild<QLineEdit*>("lspLanguageId");id->setText("custom-yaml");
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        assert(dialog.result()==QDialog::Accepted&&dialog.serverChanged);
        assert(LspConfiguration::load().bindings.value("yaml")=="python" && LspConfiguration::load().languageIds.value("yaml")=="custom-yaml");
    }
    closeDiscard(window);
}

static void symbolSearchWindowTests(QTemporaryDir& directory) {
    auto configuration = LspConfiguration::load();
    for (auto& profile : configuration.profiles) {
        const auto extension = profile.id == "clangd" ? ".cpp" : ".py";
        profile.server.arguments = {LSP_FAKE_SERVER, "--any-language", QString("--project-symbols=") + extension};
        profile.server.logLevel = 2;
    }
    configuration.save();
    QSettings().setValue("view/outline", false);
    ShortcutManager::instance().applySettings({{"view.terminal", QKeySequence("Ctrl+T")}});
    const auto project = directory.filePath("symbol-project");
    assert(QDir().mkpath(project));
    const auto cpp = QDir(project).filePath("alpha.cpp");
    const auto py = QDir(project).filePath("beta.py");
    const QByteArray original = "// first line\nint value = 1;\n";
    for (const auto& path : {cpp, py}) {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly));
        assert(file.write(original) == original.size());
    }

    MainWindow window;
    window.show();
    window.openPaths({project});
    auto* manager = window.findChild<LspManager*>();
    assert(manager && !manager->ready(cpp) && !manager->ready(py));
    assert(window.findChild<QAction*>("view.terminal")->shortcut() == QKeySequence("Ctrl+T"));
    const auto runSearch = [&](QWidget* target, const char* actionId, const char* keys,
                               const std::function<void(QDialog*, QLineEdit*, QListWidget*)>& check) {
        auto* action = window.findChild<QAction*>(actionId);
        assert(action && action->shortcut() == QKeySequence(keys));
        window.activateWindow();
        target->setFocus();
        QTest::qWait(30);
        bool visited = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            assert(dialog && dialog->objectName() == "symbolSearch");
            auto* input = dialog->findChild<QLineEdit*>("symbolQuery");
            auto* results = dialog->findChild<QListWidget*>("symbolResults");
            assert(input && results);
            visited = true;
            check(dialog, input, results);
        });
        QTest::keySequence(target, action->shortcut());
        assert(visited);
    };
    runSearch(&window, "navigation.projectSymbols", "Ctrl+Alt+P", [&](auto* dialog, auto* input, auto* results) {
        assert(dialog->template findChild<QComboBox*>("symbolScope")->currentIndex() == 1);
        input->setText("staleSearch");
        assert(waitFor([&] { return manager->log("clangd").contains("\"query\":\"staleSearch\""); }));
        input->setText("SharedLookup");
        assert(results->count() == 0 && waitFor([&] { return results->count() == 2; }));
        assert(manager->log("clangd").contains("Cancel workspace/symbol"));
        assert(results->item(0)->text().contains("SharedLookup  ·  Example  ·  alpha.cpp"));
        assert(results->item(1)->text().contains("SharedLookup  ·  Example  ·  beta.py"));
        QTest::keyClick(input, Qt::Key_Down);
        assert(results->currentRow() == 1);
        const auto row = results->visualItemRect(results->currentItem());
        assert(row.isValid() && results->viewport()->rect().contains(row.center()));
        QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
        QTest::mouseDClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    });
    assert(waitFor([&] {
        auto* editor = window.findChild<EditorWidget*>();
        return editor && editor->filePath() == py && editor->lspPosition() == QPoint(1, 4);
    }));

    window.openPaths({cpp});
    EditorWidget* editor = nullptr;
    assert(waitFor([&] {
        for (auto* item : window.findChildren<EditorWidget*>())
            if (item->filePath() == cpp && !item->isBusy() && manager->ready(cpp)) { editor = item; return true; }
        return false;
    }));
    assert(!window.findChild<QDockWidget*>("outlinePanel")->isVisible());
    editor->setNavigationPosition(2, 5);
    QTest::keyClicks(editor, "fresh_symbol");
    assert(editor->isModified());
    runSearch(editor, "navigation.documentSymbols", "Ctrl+Shift+O", [&](auto* dialog, auto* input, auto* results) {
        auto* scope = dialog->template findChild<QComboBox*>("symbolScope");
        assert(scope->currentIndex() == 0);
        input->setText("fresh");
        assert(waitFor([&] { return results->count() == 1 && results->item(0)->text() == "fresh_symbol"; }));
        scope->setCurrentIndex(1);
        assert(waitFor([&] { return results->count() == 2; }));
        scope->setCurrentIndex(0);
        assert(waitFor([&] { return results->count() == 1 && results->item(0)->text() == "fresh_symbol"; }));
        QTest::keyClick(input, Qt::Key_Return);
    });
    assert(editor->lspPosition() == QPoint(1, 4));
    assert(waitFor([&] { return editor->hasFocus(); }));
    QFile file(cpp);
    assert(file.open(QIODevice::ReadOnly) && file.readAll() == original);
    runSearch(editor, "navigation.documentSymbols", "Ctrl+Shift+O", [&](auto*, auto* input, auto*) {
        QTest::keyClick(input, Qt::Key_Escape);
    });
    assert(editor->lspPosition() == QPoint(1, 4));
    assert(waitFor([&] { return editor->hasFocus(); }));
    closeDiscard(window);
}
