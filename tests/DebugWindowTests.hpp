#include "debug/DebugPanel.hpp"
#include <QClipboard>
#include <QPushButton>
static void debugWindowTests(QTemporaryDir& directory) {
    if(!qEnvironmentVariableIsEmpty("TURTLE_DEBUG_SIDEBAR_BACKGROUND")) {
        QSettings().setValue("appearance/backgroundImage",qEnvironmentVariable("TURTLE_DEBUG_SIDEBAR_BACKGROUND"));
        QSettings().setValue("appearance/backgroundOpacity",5);
        QSettings().setValue("appearance/theme","tokyo-night");
    }
    ThemeManager::instance().initialize();
    if(qEnvironmentVariableIsSet("TURTLE_DEBUG_WINDOW_SCREENSHOT") || qEnvironmentVariableIsSet("TURTLE_DEBUG_SETUP_SCREENSHOT") || qEnvironmentVariableIsSet("TURTLE_DEBUG_CONFIG_SCREENSHOT")) { I18n::saveLanguage("ru"); I18n::initialize(); }
    const auto source=QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()+"/../examples/debug/demo.py";
    QFile input(source); assert(input.open(QIODevice::ReadOnly));
    const auto path=directory.filePath("demo.py"); QFile output(path); assert(output.open(QIODevice::WriteOnly)); output.write(input.readAll()); output.close();
    MainWindow window; window.resize(1280,900); window.show(); window.openPaths({directory.path(),path});
    auto* editor=window.findChild<EditorWidget*>(); assert(editor && waitFor([&]{return !editor->isBusy();}));
    auto* activity=window.findChild<QToolButton*>("debugActivity"); auto* panel=window.findChild<DebugPanel*>(); assert(activity && panel);
    activity->click(); assert(activity->isChecked() && panel->isVisible());
    auto* setup=panel->findChild<QPushButton*>("debugConfigure"); assert(setup && setup->isVisible());
    if(qEnvironmentVariableIsSet("TURTLE_DEBUG_SETUP_SCREENSHOT")) {QTest::qWait(50); assert(window.grab().save(qEnvironmentVariable("TURTLE_DEBUG_SETUP_SCREENSHOT")));}
    setup->click(); assert(QFileInfo::exists(panel->configurationPath()));
    assert(waitFor([&]{for(auto* candidate:window.findChildren<EditorWidget*>()) if(candidate->filePath()==panel->configurationPath() && !candidate->isBusy()) return true; return false;}));
    if(qEnvironmentVariableIsSet("TURTLE_DEBUG_CONFIG_SCREENSHOT")) {QTest::qWait(50); assert(window.grab().save(qEnvironmentVariable("TURTLE_DEBUG_CONFIG_SCREENSHOT")));}
    // An unsaved configuration must be saved before the on-disk version can run.
    for(auto* candidate:window.findChildren<EditorWidget*>()) if(candidate->filePath()==panel->configurationPath()) {
        QTest::keyClick(candidate,Qt::Key_Space); emit panel->startRequested(); assert(!panel->active()); candidate->documentUndo();
    }
    window.openPaths({path}); assert(!setup->isVisible());
    // A margin click toggles exactly the clicked source line.
    QTest::mouseClick(editor->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(7,editor->fontMetrics().height()/2));
    assert(panel->breakpoints(path).contains(1));
    QTest::mouseClick(editor->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(7,editor->fontMetrics().height()/2));
    assert(panel->breakpoints(path).isEmpty());
    editor->setNavigationPosition(7,1); QTest::keyClick(editor,Qt::Key_F9); assert(panel->breakpoints(path).contains(7));
    editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_Return);
    assert(panel->breakpoints(path).contains(8)); editor->documentUndo(); assert(panel->breakpoints(path).contains(7));
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier | Qt::ShiftModifier); assert(panel->breakpoints(path).contains(8)); editor->documentUndo();
    const std::string large(1100000,'x');
    editor->setNavigationPosition(1,1); QApplication::clipboard()->setText(QString::fromStdString(large+"\n"));
    QTest::keyClick(editor,Qt::Key_V,Qt::ControlModifier);
    assert(waitFor([&]{return !editor->isBusy();})); assert(panel->breakpoints(path).contains(8));
    editor->documentUndo(); assert(waitFor([&]{return !editor->isBusy();})); assert(panel->breakpoints(path).contains(7));
    panel->useFile(path);
    assert(!panel->findChild<QLineEdit*>("debugProgram"));
    for(auto* a:window.findChildren<QAction*>()) if(a->text()==I18n::text("Git")) { a->trigger(); break; }
    assert(!activity->isChecked()); activity->click(); assert(activity->isChecked());
    // Unsaved buffers must not be launched against different disk contents.
    QTest::keyClick(editor,Qt::Key_X); emit panel->startRequested(); assert(!panel->active()); editor->documentUndo();
    const auto python=qEnvironmentVariable("TURTLE_TEST_DEBUG_PYTHON");
    if(!python.isEmpty()) {
        QFile configuration(panel->configurationPath()); assert(configuration.open(QIODevice::WriteOnly|QIODevice::Truncate));
        auto json=DebugConfiguration::defaults(true); json["program"]="demo.py"; json["python"]=python;
        configuration.write(QJsonDocument(json).toJson()); configuration.close();
        emit panel->startRequested(); auto* frames=panel->findChild<QTreeWidget*>("debugStack");
        assert(waitFor([&]{return frames->topLevelItemCount()>0;}));
        auto* vars=panel->findChild<QTreeWidget*>("debugVariables");
        assert(waitFor([&]{return vars->topLevelItemCount()>0 && vars->topLevelItem(0)->childCount()>0;}));
    }
    if(!qEnvironmentVariable("TURTLE_DEBUG_WINDOW_SCREENSHOT").isEmpty()) {
        if(auto* terminal=window.findChild<TerminalPanel*>()) terminal->showDebugConsole();
        if(!python.isEmpty()) {
            auto* expression=panel->console()->findChild<QLineEdit*>("debugEvaluate"); expression->setText("1 + 41"); QTest::keyClick(expression,Qt::Key_Return);
            auto* output=panel->console()->findChild<QPlainTextEdit*>("debugOutput"); assert(waitFor([&]{return output->toPlainText().contains("42");}));
        }
        QTest::qWait(200); assert(window.grab().save(qEnvironmentVariable("TURTLE_DEBUG_WINDOW_SCREENSHOT")));
    }
    closeDiscard(window);
}

static void debugNativeWindowTests() {
    ThemeManager::instance().initialize();
    I18n::saveLanguage("ru"); I18n::initialize();
    const auto project=qEnvironmentVariable("TURTLE_TEST_C_PROJECT"); assert(!project.isEmpty());
    const auto source=QDir(project).filePath("main.c"); assert(QFileInfo(source).isFile());
    MainWindow window; window.resize(1280,900); window.show(); window.openPaths({project,source});
    auto* panel=window.findChild<DebugPanel*>(); auto* editor=window.findChild<EditorWidget*>(); assert(panel && editor);
    assert(waitFor([&]{return !editor->isBusy();})); window.findChild<QToolButton*>("debugActivity")->click();
    panel->toggleBreakpoint(source,9); // Match the source breakpoint in the reported screenshot.
    int highlightedLine=0;
    QObject::connect(panel,&DebugPanel::locationChanged,&window,[&](const QString& path,int line){if(path==source) highlightedLine=line;});
    emit panel->startRequested();
    assert(waitFor([&]{return highlightedLine>0;}));
    auto* frames=panel->findChild<QTreeWidget*>("debugStack"); assert(frames->currentItem() && frames->currentItem()->text(0).contains("main"));
    auto* variables=panel->findChild<QTreeWidget*>("debugVariables"); assert(waitFor([&]{return variables->topLevelItemCount()>0;}));
    panel->perform("next"); assert(waitFor([&]{return frames->currentItem()!=nullptr;}));
    if(qEnvironmentVariableIsSet("TURTLE_DEBUG_NATIVE_SCREENSHOT")) {QTest::qWait(100); assert(window.grab().save(qEnvironmentVariable("TURTLE_DEBUG_NATIVE_SCREENSHOT")));}
    panel->perform("start");
    auto* terminal=window.findChild<TerminalPanel*>(); assert(terminal);
    auto* io=terminal->findChild<TerminalWidget*>("debugProgramTerminal"); assert(io);
    assert(waitFor([&]{return io->screenText().contains("Bonus");})); emit io->input("4\n");
    assert(waitFor([&]{return io->screenText().contains("Result: 20");})); assert(waitFor([&]{return !panel->active();}));
    closeDiscard(window);
}
