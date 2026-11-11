#include "debug/DapClient.hpp"
#include "debug/DebugPanel.hpp"
#include "app/I18n.hpp"
#include "app/ThemeManager.hpp"
#include "app/WorkspaceBackground.hpp"
#include <QSplitter>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include "terminal/TerminalPanel.hpp"
#include "terminal/TerminalWidget.hpp"
#include "terminal/TerminalSession.hpp"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QComboBox>
#include <QTabWidget>
#include <QTest>
#include <QSignalSpy>
#include <QSettings>
#include <QFile>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QPushButton>
#include <QCryptographicHash>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QToolButton>
#include <QTreeWidget>
#include <QPlainTextEdit>

class DebugTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
    QToolButton* button(DebugPanel& panel,const QString& name) {
        for(auto* b:panel.findChildren<QToolButton*>()) if(b->accessibleName()==name) return b;
        return nullptr;
    }
    bool writeConfiguration(const QString& root,const QJsonObject& value) {
        if(!QDir().mkpath(root+"/.turtle")) return false;
        QFile file(DebugConfiguration::legacyPath(root)); if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)) return false;
        const auto data=QJsonDocument(value).toJson(); return file.write(data)==data.size();
    }
    QString stateKey(const QString& root) {
        return "debug/workspaces/"+QString::fromLatin1(QCryptographicHash::hash(QDir(root).absolutePath().toUtf8(),QCryptographicHash::Sha256).toHex());
    }
    void exercise(const QString& interpreter,const QString& native={}) {
        QSettings().clear();
        QTemporaryDir project;
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"watches",QJsonArray{"1 + 41"}}}).toJson());
        DebugPanel panel; panel.setWorkspace(project.path());
        panel.resize(400,1000); panel.show();
        const auto program=native.isEmpty()?QString(DEBUG_SOURCE "/examples/debug/demo.py"):qEnvironmentVariable("TURTLE_TEST_CPP_PROGRAM");
        auto config=DebugConfiguration::defaults(native.isEmpty()); config["program"]=program;
        config[native.isEmpty()?"python":"adapter"]=native.isEmpty()?interpreter:native;
        config["stopOnEntry"]=native.isEmpty(); QVERIFY(writeConfiguration(project.path(),config)); panel.useFile(program);
        panel.toggleBreakpoint(native.isEmpty()?program:QString(DEBUG_SOURCE "/examples/debug/demo.cpp"),native.isEmpty()?7:8);
        QSignalSpy locations(&panel,&DebugPanel::locationChanged);
        QSignalSpy events(panel.findChild<DapClient*>(),&DapClient::eventReceived);
        auto* frames=panel.findChild<QTreeWidget*>("debugStack");
        auto* variables=panel.findChild<QTreeWidget*>("debugVariables");
        panel.start();
        QTRY_VERIFY_WITH_TIMEOUT(frames->topLevelItemCount()>0,20000);
        QTRY_VERIFY_WITH_TIMEOUT(variables->topLevelItemCount()>0 && variables->topLevelItem(0)->childCount()>0,10000);
        auto* watches=panel.findChild<QTreeWidget*>("debugWatches");
        QTRY_VERIFY_WITH_TIMEOUT(watches->topLevelItemCount()==1 && watches->topLevelItem(0)->text(1)=="42",10000);
        auto* expression=panel.findChild<QLineEdit*>("debugEvaluate"); expression->setText("1 + 41"); QTest::keyClick(expression,Qt::Key_Return);
        QTRY_VERIFY(panel.findChild<QPlainTextEdit*>()->toPlainText().contains("> 1 + 41\n") && panel.findChild<QPlainTextEdit*>()->toPlainText().section("> 1 + 41\n",1).contains("42"));
        QVERIFY(!locations.isEmpty()); QVERIFY(button(panel,"Step Over")->isEnabled());
        button(panel,"Step Over")->click();
        QTRY_VERIFY_WITH_TIMEOUT(frames->topLevelItemCount()>0,10000);
        QTRY_VERIFY_WITH_TIMEOUT(variables->topLevelItemCount()>0 && variables->topLevelItem(0)->childCount()>0,10000);
        if(!qEnvironmentVariable("TURTLE_DEBUG_SCREENSHOT").isEmpty()) panel.grab().save(qEnvironmentVariable("TURTLE_DEBUG_SCREENSHOT"));
        button(panel,"Stop Debugging")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.active(),7000);
        // A delayed shutdown timer from the old session must not kill a restarted one.
        config["args"]=QJsonArray{"hello world","--reloaded"}; QVERIFY(writeConfiguration(project.path(),config));
        panel.start();
        QTRY_VERIFY_WITH_TIMEOUT(frames->topLevelItemCount()>0,20000);
        if(native.isEmpty() && interpreter!=qEnvironmentVariable("TURTLE_TEST_DEBUG_PYTHON")) {
            QJsonObject launch;
            for(const auto& event:events) if(event.at(0).toString()=="turtle-test-launch") launch=event.at(1).toJsonObject();
            QCOMPARE(launch.value("args").toArray(),QJsonArray({"hello world","--reloaded"}));
            QCOMPARE(launch.value("cwd").toString(),project.path());
        }
        QTest::qWait(2800); QVERIFY(panel.active());
        button(panel,"Continue")->click();
        // May stop at the configured breakpoint before reaching the end.
        for(int i=0;i<8 && panel.active();++i) {
            QTest::qWait(300);
            if(button(panel,"Continue")->isEnabled()) button(panel,"Continue")->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!panel.active(),10000);
        QVERIFY(panel.findChild<QPlainTextEdit*>()->toPlainText().contains(native.isEmpty() && interpreter=="python3"?QString::fromUtf8("результат: 42"):"Done: 16"));
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("TurtleDebugTests"); QCoreApplication::setApplicationName("TurtleDebugTests");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings_.path());
    }
    void sidebarSections() {
        QSettings().clear();
        QTemporaryDir project;
        QVERIFY(QDir().mkpath(project.filePath(".turtle")));
        QFile launch(project.filePath(".turtle/launch.json")); QVERIFY(launch.open(QIODevice::WriteOnly));
        launch.write(R"({"version":"0.2.0","configurations":[{"name":"openocd","type":"debugpy","request":"launch","program":"demo.py"}]})"); launch.close();
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"watches",QJsonArray{"config","netvars","adcData","config_check"}}}).toJson());
        const auto screenshot=qEnvironmentVariable("TURTLE_DEBUG_SIDEBAR_SCREENSHOT");
        if(!screenshot.isEmpty()) {
            I18n::saveLanguage("ru"); I18n::initialize();
            QSettings().setValue("appearance/theme","tokyo-night");
            QSettings().setValue("appearance/backgroundImage",qEnvironmentVariable("TURTLE_DEBUG_SIDEBAR_BACKGROUND"));
            QSettings().setValue("appearance/backgroundOpacity",5);
        }
        ThemeManager::instance().initialize();
        DebugPanel panel; panel.setObjectName("debugPanel"); panel.setWorkspace(project.path());
        panel.resize(250,980); panel.show(); QApplication::processEvents();
        auto* sections=panel.findChild<QSplitter*>("debugSections"); QVERIFY(sections); QCOMPARE(sections->count(),4);
        const QStringList names{"debugVariables","debugWatches","debugStack","debugBreakpoints"};
        for(int i=0;i<names.size();++i) {
            QCOMPARE(sections->widget(i)->objectName(),names[i]+"Section");
            QVERIFY(panel.findChild<QTreeWidget*>(names[i])->isHeaderHidden());
            QVERIFY(panel.findChild<QToolButton*>(names[i]+"Toggle")->isChecked());
        }
        QVERIFY(panel.findChild<QWidget*>("debugToolbar")->isHidden());
        QVERIFY(panel.findChild<QLabel*>("debugStatus")->isHidden());
        auto* watches=panel.findChild<QTreeWidget*>("debugWatches"); QCOMPARE(watches->topLevelItemCount(),4);
        QCOMPARE(watches->topLevelItem(0)->text(1),I18n::text("No data"));
        QVERIFY(watches->topLevelItem(0)->isFirstColumnSpanned());
        QVERIFY(watches->topLevelItem(0)->data(0,Qt::UserRole+3).toBool());
        auto* toggle=panel.findChild<QToolButton*>("debugVariablesToggle");
        QTest::mouseClick(toggle,Qt::LeftButton); QApplication::processEvents();
        QVERIFY(panel.findChild<QTreeWidget*>("debugVariables")->isHidden()); QCOMPARE(sections->widget(0)->height(),26);
        {DebugPanel restored; QVERIFY(!restored.findChild<QToolButton*>("debugVariablesToggle")->isChecked());}
        QTest::mouseClick(toggle,Qt::LeftButton); QApplication::processEvents();
        QVERIFY(panel.findChild<QTreeWidget*>("debugVariables")->isVisible()); QVERIFY(sections->widget(0)->height()>26);
        panel.toggleBreakpoint(project.filePath("stm32f4xx_it.c"),27);
        auto* points=panel.findChild<QTreeWidget*>("debugBreakpoints");
        QVERIFY(!points->topLevelItem(0)->icon(0).isNull());
        points->topLevelItem(0)->setCheckState(0,Qt::Unchecked);
        QVERIFY(panel.disabledBreakpoints(project.filePath("stm32f4xx_it.c")).contains(27));
        if(!screenshot.isEmpty()) QVERIFY(panel.grab().save(screenshot));
        QSettings().clear(); I18n::initialize(); ThemeManager::instance().initialize();
    }
    void valueMenus_data() {
        QTest::addColumn<QString>("mode");
        for(const auto& mode:{"all","variable","none","cortex"}) QTest::newRow(mode)<<QString(mode);
    }
    void valueMenus() {
        QFETCH(QString,mode); QSettings().clear();
        QTemporaryDir project;
        const auto program=QString(DEBUG_SOURCE "/examples/debug/demo.py");
        QJsonObject config{{"version",1},{"type","dap"},{"adapter",QStandardPaths::findExecutable("python3")},
            {"adapterID",mode=="cortex"?"cortex-debug":"fixture"},{"adapterArgs",QJsonArray{QString(DEBUG_SOURCE "/tests/debug/fake_adapter.py")}},
            {"program",program},{"cwd",project.path()},{"env",QJsonObject{{"TURTLE_VALUE_TEST",mode}}}};
        QVERIFY(writeConfiguration(project.path(),config));
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"watches",QJsonArray{"total"}}}).toJson());
        DebugPanel panel; panel.setWorkspace(project.path()); panel.resize(300,900); panel.show();
        auto* tree=panel.findChild<QTreeWidget*>("debugVariables");
        auto* watches=panel.findChild<QTreeWidget*>("debugWatches");
        QSignalSpy events(panel.findChild<DapClient*>(),&DapClient::eventReceived);
        panel.start(); QTRY_VERIFY(tree->topLevelItemCount()>0 && tree->topLevelItem(0)->childCount()==3);
        const auto total=[&]{return tree->topLevelItem(0)->child(0);};
        const auto invoke=[&](QTreeWidget* view,QTreeWidgetItem* item,const char* id) {
            emit view->customContextMenuRequested(item?view->visualItemRect(item).center():QPoint(view->viewport()->width()-1,view->viewport()->height()-1));
            QPointer<QMenu> menu;
            for(auto* candidate:view->findChildren<QMenu*>("debugValuesMenu")) if(candidate->isVisible()) menu=candidate;
            if(!menu) return false;
            auto* action=menu->findChild<QAction*>(id); const bool enabled=action && action->isEnabled();
            if(enabled) action->trigger();
            if(menu) menu->close();
            return enabled;
        };
        QVERIFY(invoke(tree,total(),"debugCopyValue")); QCOMPARE(QApplication::clipboard()->text(),QString("42"));
        QVERIFY(invoke(tree,total(),"debugCopyExpression")); QCOMPARE(QApplication::clipboard()->text(),QString("total"));
        for(const auto* id:{"debugEvaluateConsole","debugAddToWatch","debugNewWatch","debugRemoveWatch","debugRemoveAllWatches","debugHexValues","debugRefreshValues","debugCollapseAll"})
            QVERIFY(!invoke(tree,total(),id));
        auto* hexButton=panel.findChild<QToolButton*>("debugVariablesHex"); QVERIFY(hexButton);
        QTRY_COMPARE(watches->topLevelItemCount(),1);
        QTRY_COMPARE(watches->topLevelItem(0)->text(1),QString("42"));
        QVERIFY(!invoke(tree,tree->topLevelItem(0)->child(2),"debugSetValue")); // read-only hint
        if(mode=="none") {
            QVERIFY(!invoke(tree,total(),"debugSetValue")); QVERIFY(!hexButton->isEnabled());
        } else {
            QTimer::singleShot(0,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {dialog->setTextValue("55"); dialog->accept();}});
            QVERIFY(invoke(tree,total(),"debugSetValue"));
            QTRY_VERIFY(tree->topLevelItemCount()>0 && tree->topLevelItem(0)->childCount()==3 && total()->text(1)=="55");
            QTRY_COMPARE(watches->topLevelItem(0)->text(1),QString("55"));
            bool correctRequest=false;
            for(const auto& event:events) if(event[0].toString()=="turtle-test-values") {
                const auto data=event[1].toJsonObject(),args=data.value("arguments").toObject();
                if(data.value("command").toString()==(mode=="all"?"setExpression":"setVariable"))
                    correctRequest=mode=="all"?args.value("expression")=="total":args.value("variablesReference")==20 && args.value("name")=="total";
            }
            QVERIFY(correctRequest);
            QVERIFY(hexButton->isEnabled()); hexButton->click();
            QTRY_VERIFY(tree->topLevelItemCount()>0 && tree->topLevelItem(0)->childCount()==3 && total()->text(1)=="0x37");
            QTRY_COMPARE(watches->topLevelItem(0)->text(1),QString("0x37"));
            if(mode=="cortex") {
                QVERIFY(hexButton->isEnabled()); hexButton->click();
                QTRY_VERIFY(tree->topLevelItemCount()>0 && tree->topLevelItem(0)->childCount()==3 && total()->text(1)=="55");
                QVERIFY(hexButton->isEnabled()); hexButton->click();
                QTRY_VERIFY(tree->topLevelItemCount()>0 && tree->topLevelItem(0)->childCount()==3 && total()->text(1)=="0x37");
            }
            QTimer::singleShot(0,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {dialog->setTextValue("reject"); dialog->accept();}});
            QVERIFY(invoke(tree,total(),"debugSetValue"));
            QTRY_VERIFY(panel.findChild<QLabel*>("debugStatus")->text().contains("Cannot set value"));
            QCOMPARE(total()->text(1),QString("0x37"));
        }
        QTimer::singleShot(0,[]{if(auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {dialog->setTextValue("total + 1"); dialog->accept();}});
        QVERIFY(invoke(watches,watches->topLevelItem(0),"debugEditWatch"));
        QCOMPARE(watches->topLevelItem(0)->text(0),QString("total + 1"));
        QVERIFY(!invoke(watches,watches->topLevelItem(0),"debugRemoveWatch"));
        watches->setCurrentItem(watches->topLevelItem(0)); panel.findChild<QToolButton*>("debugRemoveWatchButton")->click();
        QCOMPARE(watches->topLevelItemCount(),0);
        panel.perform("stop"); QTRY_VERIFY(!panel.active());
    }
    void expandedValuesSurviveRefresh() {
        QSettings().clear(); QTemporaryDir project;
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"watches",QJsonArray{"record"}}}).toJson());
        QJsonObject config{{"version",1},{"type","dap"},{"adapter",QStandardPaths::findExecutable("python3")},
            {"adapterID","fixture"},{"adapterArgs",QJsonArray{QString(DEBUG_SOURCE "/tests/debug/fake_adapter.py")}},
            {"program",QString(DEBUG_SOURCE "/examples/debug/demo.py")},{"cwd",project.path()},
            {"env",QJsonObject{{"TURTLE_VALUE_TEST","all"},{"TURTLE_TREE_TEST","1"}}}};
        QVERIFY(writeConfiguration(project.path(),config));
        DebugPanel panel; panel.setWorkspace(project.path()); panel.resize(300,900); panel.show(); panel.start();
        auto* variables=panel.findChild<QTreeWidget*>("debugVariables");
        auto* watches=panel.findChild<QTreeWidget*>("debugWatches");
        const auto record=[&]{return variables->topLevelItem(0)->child(3);};
        QTRY_VERIFY(variables->topLevelItemCount()>0 && variables->topLevelItem(0)->childCount()==4);
        record()->setExpanded(true); QTRY_COMPARE(record()->childCount(),1);
        record()->child(0)->setExpanded(true); QTRY_COMPARE(record()->child(0)->childCount(),1);
        QTRY_VERIFY(watches->topLevelItemCount()==1 && watches->topLevelItem(0)->data(0,Qt::UserRole).toInt()==30);
        watches->topLevelItem(0)->setExpanded(true); QTRY_COMPARE(watches->topLevelItem(0)->childCount(),1);
        watches->topLevelItem(0)->child(0)->setExpanded(true); QTRY_COMPARE(watches->topLevelItem(0)->child(0)->childCount(),1);
        const auto restored=[&] {
            if(variables->topLevelItemCount()!=1 || variables->topLevelItem(0)->childCount()!=4) return false;
            auto* r=record(); auto* w=watches->topLevelItem(0);
            return r->isExpanded() && r->childCount()==1 && r->child(0)->isExpanded() && r->child(0)->childCount()==1
                && w->isExpanded() && w->childCount()==1 && w->child(0)->isExpanded() && w->child(0)->childCount()==1;
        };
        auto* hex=panel.findChild<QToolButton*>("debugVariablesHex"); QVERIFY(hex && hex->isEnabled());
        hex->click(); QTRY_VERIFY(restored());
        QVERIFY(panel.findChild<QToolButton*>("debugWatchesHex")->isChecked());
        QCOMPARE(record()->child(0)->child(0)->text(1),QString("0x2a"));
        panel.perform("next"); QTRY_VERIFY(restored());
        // Deliberately collapsed branches must stay collapsed on the next stop.
        record()->child(0)->setExpanded(false); watches->topLevelItem(0)->setExpanded(false);
        panel.perform("next");
        QTRY_VERIFY(variables->topLevelItemCount()==1 && variables->topLevelItem(0)->childCount()==4 && record()->isExpanded() && record()->childCount()==1);
        QVERIFY(!record()->child(0)->isExpanded()); QVERIFY(!watches->topLevelItem(0)->isExpanded());
        panel.perform("stop"); QTRY_VERIFY(!panel.active());
    }
    void workspacePersistence() {
        QSettings().clear();
        const auto file=settings_.filePath("test.py");
        {
            DebugPanel panel; panel.setWorkspace(settings_.filePath("one")); panel.useFile(file); panel.toggleBreakpoint(file,7);
            panel.moveBreakpoints(file,0,0,0,2); QVERIFY(panel.breakpoints(file).contains(9));
            auto* points=panel.findChild<QTreeWidget*>("debugBreakpoints"); points->topLevelItem(0)->setCheckState(0,Qt::Unchecked);
            QVERIFY(panel.disabledBreakpoints(file).contains(9));
            panel.moveBreakpoints(file,0,0,2,0); QVERIFY(panel.breakpoints(file).contains(7)); QVERIFY(panel.disabledBreakpoints(file).contains(7));
            panel.setWorkspace(settings_.filePath("two")); QVERIFY(panel.breakpoints(file).isEmpty()); QCOMPARE(panel.configurationPath(),settings_.filePath("two/.turtle/launch.json"));
            panel.useFile("/another/script.py"); panel.setWorkspace(settings_.filePath("one"));
            QCOMPARE(panel.configurationPath(),settings_.filePath("one/.turtle/launch.json")); QVERIFY(panel.breakpoints(file).contains(7));
        }
        DebugPanel restored; restored.setWorkspace(settings_.filePath("one")); QVERIFY(restored.breakpoints(file).contains(7)); QVERIFY(restored.disabledBreakpoints(file).contains(7));
        restored.renameSource(file,file+".new"); QVERIFY(restored.breakpoints(file).isEmpty()); QVERIFY(restored.breakpoints(file+".new").contains(7));
    }
    void configurationCreationAndEdits() {
        QSettings().clear(); QTemporaryDir project; DebugPanel panel; panel.setWorkspace(project.path());
        panel.useFile(project.filePath("main.py")); panel.show();
        auto* setup=panel.findChild<QPushButton*>("debugConfigure"); QVERIFY(setup && setup->isVisible());
        QVERIFY(!panel.findChild<QLineEdit*>("debugProgram"));
        QSignalSpy opened(&panel,&DebugPanel::configurationRequested); setup->click();
        QCOMPARE(opened.size(),1); QCOMPARE(opened.first().at(0).toString(),project.filePath(".turtle/launch.json"));
        QFile file(panel.configurationPath()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original=file.readAll(); file.close();
        const auto config=QJsonDocument::fromJson(original).object();
        QCOMPARE(config.value("version").toString(),QString("0.2.0"));
        const auto entry=config.value("configurations").toArray().first().toObject();
        QCOMPARE(entry.value("type").toString(),QString("debugpy"));
        QCOMPARE(entry.value("program").toString(),QString("${file}")); QVERIFY(!setup->isVisible());
        auto manual=config;manual["extra"]=42;
        QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(QJsonDocument(manual).toJson());file.close();
        panel.toggleBreakpoint(project.filePath("main.py"),1); panel.editConfiguration();
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(),manual); file.close();
        QVERIFY(file.remove()); QTRY_VERIFY(setup->isVisible());
        // F5 never creates a configuration implicitly.
        panel.start(); QVERIFY(!QFileInfo::exists(panel.configurationPath())); QVERIFY(!panel.active());
        panel.editConfiguration(); QVERIFY(QFileInfo::exists(panel.configurationPath()));
    }
    void portableConfigurationAndValidation() {
        QTemporaryDir first,second;
        for(const auto& directory:{first.path(),second.path()}) {
            QFile source(QDir(directory).filePath("hello world.py")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("print(1)\n"); source.close();
            auto config=DebugConfiguration::defaults(true); config["program"]="hello world.py"; config["python"]=".venv/bin/python";
            config["args"]=QJsonArray{"a b","${workspaceFolder}","${fileDirname}","${file}"}; QVERIFY(writeConfiguration(directory,config));
            DebugConfiguration parsed; QString error; QVERIFY2(DebugConfiguration::read(directory,source.fileName(),parsed,error),qPrintable(error));
            QCOMPARE(parsed.program,source.fileName()); QCOMPARE(parsed.cwd,directory); QCOMPARE(parsed.python,QDir(directory).filePath(".venv/bin/python"));
            QCOMPARE(parsed.arguments[0].toString(),QString("a b")); QCOMPARE(parsed.arguments[1].toString(),directory);
            QCOMPARE(parsed.arguments[3].toString(),source.fileName());
        }
        auto invalid=DebugConfiguration::defaults(true); invalid["program"]="hello world.py";
        for(const auto& bad:QList<QJsonObject>{QJsonObject{{"version",2}},QJsonObject{{"type","wrong"}},QJsonObject{{"args","a b"}},QJsonObject{{"args",QJsonArray{12}}},QJsonObject{{"python",""}},QJsonObject{{"stopOnEntry","false"}},QJsonObject{{"typo",true}},QJsonObject{{"externalSymbols","yes"}}}) {
            auto config=invalid; for(auto it=bad.begin();it!=bad.end();++it) config[it.key()]=it.value(); QVERIFY(writeConfiguration(first.path(),config));
            DebugConfiguration parsed; QString error; QVERIFY(!DebugConfiguration::read(first.path(),{},parsed,error)); QVERIFY(error.contains("debug.json"));
        }
        DebugPanel panel; panel.setWorkspace(first.path()); QFile malformed(panel.configurationPath()); QVERIFY(malformed.open(QIODevice::WriteOnly|QIODevice::Truncate)); malformed.write("{\n broken"); malformed.close();
        panel.start(); QVERIFY(!panel.active()); QVERIFY(panel.console()->findChild<QPlainTextEdit*>()->toPlainText().contains("debug.json"));
    }
    void configurationMigration() {
        QSettings().clear(); QTemporaryDir project;
        QJsonObject legacy{{"kind",0},{"program",project.filePath("main.py")},{"python",project.filePath(".venv/bin/python")},{"args","--name \"a b\""},{"entry",true}};
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"configuration",legacy}}).toJson());
        DebugPanel panel; panel.setWorkspace(project.path()); QVERIFY(!QFileInfo::exists(panel.configurationPath())); panel.editConfiguration();
        QFile file(panel.configurationPath()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto config=QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(config.value("program").toString(),QString("main.py")); QCOMPARE(config.value("python").toString(),QString(".venv/bin/python"));
        QCOMPARE(config.value("args").toArray(),QJsonArray({"--name","a b"}));
        QVERIFY(!QJsonDocument::fromJson(QSettings().value(stateKey(project.path())).toByteArray()).object().contains("configuration"));
        auto manual=config; manual["program"]="custom.py"; QVERIFY(writeConfiguration(project.path(),manual));
        QSettings().setValue(stateKey(project.path()),QJsonDocument(QJsonObject{{"configuration",legacy}}).toJson());
        DebugPanel existing; existing.setWorkspace(project.path()); existing.editConfiguration(); file.close(); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object(),manual);
        QVERIFY(!QJsonDocument::fromJson(QSettings().value(stateKey(project.path())).toByteArray()).object().contains("configuration"));
    }
    void framingAndErrors() {
        DapClient client; QSignalSpy ready(&client,&DapClient::ready),error(&client,&DapClient::error);
        client.start("python3",{DEBUG_SOURCE "/tests/debug/fake_adapter.py"}); QTRY_COMPARE(ready.size(),1);
        bool reply=false;
        client.request("initialize",{},[&](const QJsonObject& r){reply=r.value("success").toBool();}); QTRY_VERIFY(reply);
        client.request("bad"); QTRY_VERIFY(!error.isEmpty()); QTRY_VERIFY(!client.active());
        DapClient missing; QSignalSpy missingError(&missing,&DapClient::error); missing.start("/no/such/debugger",{}); QTRY_VERIFY(!missingError.isEmpty());
    }
    void vscodeJsonAndTasks() {
        QTemporaryDir project;const auto root=project.path();QDir dir(root);QVERIFY(dir.mkpath(".turtle"));
        const auto file=root+"/main.cpp";{QFile f(file);QVERIFY(f.open(QIODevice::WriteOnly));f.write("int main() {}\n");}
        const auto write=[&](const QString& name,const QByteArray& data) {QFile f(DebugProject::path(root,name));return f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(data)==data.size();};
        const bool jsonCase1=write("settings.json","{/* settings */ \"program\": \"${workspaceFolder}/main.cpp\", \"url\":\"https://a//b/*c*/\",}");QVERIFY(jsonCase1);
        const QByteArray launch="{\n          // A comment, not a string replacement\n          \"version\":\"0.2.0\", \"configurations\":[\n            {\"name\":\"first\",\"type\":\"lldb\",\"request\":\"launch\",\"program\":\"${config:program}\",\n             \"args\":[\"${fileBasename}\",\"${relativeFile}\",\"https://a//b/*c*/\",\"\\\"quoted\\\"\"],},\n            {\"name\":\"second\",\"type\":\"lldb\",\"request\":\"launch\",\"program\":\"${file}\",\n             \"preLaunchTask\":\"Build\",\"linux\":{\"args\":[\"linux\"]}},\n          ],\n        }";
        QVERIFY(write("launch.json",launch));DebugConfiguration c;QString error;
        QVERIFY2(DebugConfiguration::read(root,file,c,error),qPrintable(error));QCOMPARE(c.program,file);
        QCOMPARE(c.arguments[0].toString(),QString("main.cpp"));QCOMPARE(c.arguments[1].toString(),QString("main.cpp"));
        QCOMPARE(c.arguments[2].toString(),QString("https://a//b/*c*/"));
        const bool jsonCase2=write("tasks.json","{\"version\":\"2.0.0\",\"tasks\":[\n          {\"label\":\"Configure\",\"type\":\"process\",\"command\":\"cmake\",\"args\":[\"-B\",\"build\"]},\n          {\"label\":\"Build\",\"type\":\"process\",\"command\":\"cmake\",\"args\":[\"--build\",\"build\"],\"dependsOn\":\"Configure\"},\n        ]}");QVERIFY(jsonCase2);
        QVERIFY2(DebugConfiguration::read(root,file,c,error,"second"),qPrintable(error));QCOMPARE(c.buildSteps.size(),2);
        QCOMPARE(c.buildSteps[0].command,QStringList({"cmake","-B","build"}));QCOMPARE(c.arguments[0].toString(),QString("linux"));
        QVERIFY(!DebugConfiguration::read(root,file,c,error,"missing"));QVERIFY(error.contains("not found"));
        const bool jsonCase3=write("tasks.json","{\"version\":\"2.0.0\",\"tasks\":[{\"label\":\"Build\",\"dependsOn\":\"Build\"}]}");QVERIFY(jsonCase3);
        QVERIFY(!DebugConfiguration::read(root,file,c,error,"second"));QVERIFY(error.contains("cycle"));
        const bool jsonCase4=write("tasks.json","{\"version\":\"2.0.0\",\"tasks\":[]}");QVERIFY(jsonCase4);
        QVERIFY(!DebugConfiguration::read(root,file,c,error,"second"));QVERIFY(error.contains("Task not found"));
        const bool jsonCase5=write("launch.json","{\"version\":\"0.2.0\",\"configurations\":[{\"name\":\"bad\",\"type\":\"lldb\",\"request\":\"launch\",\"program\":\"${command:cmake.launchTargetPath}\"}]}");QVERIFY(jsonCase5);
        QVERIFY(!DebugConfiguration::read(root,file,c,error));QVERIFY(error.contains("Unsupported variable"));
        const bool jsonCase6=write("launch.json","{\"version\":\"0.2.0\",\"configurations\":[{\"name\":\"bad\",\"type\":\"lldb\",\"request\":\"launch\",\"program\":\"${config:program}\"}]}");QVERIFY(jsonCase6);
        const bool jsonCase7=write("settings.json","{\"program\":\"${config:program}\"}");QVERIFY(jsonCase7);
        QVERIFY(!DebugConfiguration::read(root,file,c,error));QVERIFY(error.contains("recursive"));
        QVERIFY(write("launch.json","{\n/* missing end"));
        QVERIFY(!DebugConfiguration::read(root,file,c,error));QVERIFY(error.contains("Unterminated"));QVERIFY(error.contains("launch.json:2"));
        // A malformed launch file is never bypassed in favour of legacy settings.
        auto legacy=DebugConfiguration::defaults(false);legacy["program"]=file;QVERIFY(writeConfiguration(root,legacy));
        QVERIFY(!DebugConfiguration::read(root,file,c,error));QVERIFY(error.contains("launch.json"));
    }
    void vscodeSelectedSession() {
        QSettings().clear();QTemporaryDir project;const auto root=project.path();QDir dir(root);QVERIFY(dir.mkpath(".turtle"));QVERIFY(dir.mkpath("build dir"));
        const auto python=QStandardPaths::findExecutable("python3");QVERIFY(!python.isEmpty());
        const auto program=root+"/build dir/firmware.elf",marker=root+"/order";
        const auto wrapper=root+"/adapter";{QFile f(wrapper);QVERIFY(f.open(QIODevice::WriteOnly));f.write("#!/bin/sh\nexec python3 '" DEBUG_SOURCE "/tests/debug/fake_adapter.py'\n");f.close();QVERIFY(f.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));}
        auto write=[&](const QString& name,const QJsonObject& object){QFile f(DebugProject::path(root,name));const auto data=QJsonDocument(object).toJson();return f.open(QIODevice::WriteOnly|QIODevice::Truncate) && f.write(data)==data.size();};
        auto entry=QJsonObject{{"name","first"},{"type","debugpy"},{"request","launch"},{"program","${config:firmware}"},{"preLaunchTask","Build"}};
        auto second=entry;second["name"]="second";second["args"]=QJsonArray{"selected"};second["env"]=QJsonObject{{"TURTLE_ADAPTER_TEST","ready"}};
        QVERIFY(write("launch.json",{{"version","0.2.0"},{"configurations",QJsonArray{entry,second}}}));
        QVERIFY(write("settings.json",{{"python.defaultInterpreterPath",wrapper},{"firmware","${workspaceFolder}/build dir/firmware.elf"}}));
        const auto options=QJsonObject{{"cwd","${workspaceFolder}/build dir"},{"env",QJsonObject{{"GLOBAL","yes"}}}};
        const auto tasks=QJsonObject{{"version","2.0.0"},{"options",options},{"tasks",QJsonArray{
            QJsonObject{{"label","Configure"},{"type","process"},{"command",python},{"args",QJsonArray{"-c","import os,pathlib,sys; assert os.environ['GLOBAL']=='yes'; pathlib.Path(sys.argv[1]).write_text(os.environ['ORDER'])",marker}},{"options",QJsonObject{{"env",QJsonObject{{"ORDER","first"}}}}}},
            QJsonObject{{"label","Compile"},{"type","shell"},{"command",python},{"args",QJsonArray{"-c","import os,pathlib,sys; assert pathlib.Path(sys.argv[1]).read_text()=='first'; assert pathlib.Path.cwd().name=='build dir'; assert len(sys.argv)==4 and len(sys.argv[3])==9 and sys.argv[3].endswith(';'); pathlib.Path(sys.argv[2]).write_text('firmware')",marker,program,"a b ' $ ;"}}},
            QJsonObject{{"label","Build"},{"dependsOn",QJsonArray{"Configure","Compile"}},{"dependsOrder","sequence"}}
        }}};QVERIFY(write("tasks.json",tasks));
        DebugPanel panel;panel.setWorkspace(root);panel.show();
        auto* combo=panel.findChild<QComboBox*>("debugConfigurations");QCOMPARE(combo->count(),2);combo->setCurrentIndex(1);
        QSignalSpy events(panel.findChild<DapClient*>(),&DapClient::eventReceived);
        panel.start();QTRY_VERIFY(panel.findChild<QTreeWidget*>("debugStack")->topLevelItemCount()>0);
        bool launched=false,env=false;
        for(const auto& event:events) {
            if(event[0].toString()=="turtle-test-launch") {QCOMPARE(event[1].toJsonObject().value("args").toArray(),QJsonArray{"selected"});launched=true;}
            if(event[0].toString()=="turtle-test-environment")env=event[1].toJsonObject().value("value").toString()=="ready";
        }
        QVERIFY(launched && env);panel.perform("stop");QTRY_VERIFY(!panel.active());
        DebugPanel restored;restored.setWorkspace(root);QCOMPARE(restored.findChild<QComboBox*>("debugConfigurations")->currentData().toString(),QString("second"));
        // Changing settings/tasks is picked up before the next start.
        QVERIFY(QFile::remove(program));QVERIFY(write("tasks.json",{{"version","2.0.0"},{"tasks",QJsonArray{QJsonObject{{"label","Build"},{"type","process"},{"command",python},{"args",QJsonArray{"-c","import sys;sys.exit(5)"}}}}}}));
        events.clear();panel.start();QTRY_VERIFY(!panel.active());QVERIFY(events.isEmpty());QVERIFY(!QFileInfo::exists(program));
    }
    void externalAdapterAndPreparation() {
        QTemporaryDir project; QDir root(project.path()); QVERIFY(root.mkpath("source folder"));
        const auto program=root.filePath("source folder/firmware.elf");
        const auto marker=root.filePath("prepared");
        const auto python=QStandardPaths::findExecutable("python3"); QVERIFY(!python.isEmpty());
        QJsonObject config{{"version",1},{"type","dap"},{"adapter",python},{"adapterID","fixture"},
            {"adapterArgs",QJsonArray{QString(DEBUG_SOURCE "/tests/debug/fake_adapter.py")}},
            {"program","source folder/firmware.elf"},{"cwd","${workspaceFolder}"},
            {"preLaunch",QJsonArray{
                QJsonArray{python,"-c","import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('firmware')",program},
                QJsonArray{python,"-c","import pathlib,sys; assert pathlib.Path(sys.argv[1]).read_text()=='firmware'; pathlib.Path(sys.argv[2]).touch()",program,marker}}},
            {"launchArguments",QJsonObject{{"program","${program}"},{"executable","${program}"},
                {"gdbServerConsolePort","${serverConsolePort}"},
                {"args",QJsonArray{"a b","${workspaceFolder}"}},
                {"nested",QJsonObject{{"paths",QJsonArray{"${workspaceFolder}/target","${program}"}}}}}}};
        QVERIFY(writeConfiguration(project.path(),config));
        DebugPanel panel; panel.setWorkspace(project.path()); panel.show();
        auto* client=panel.findChild<DapClient*>(); QSignalSpy events(client,&DapClient::eventReceived);
        auto* output=panel.console()->findChild<QPlainTextEdit*>("debugOutput");
        panel.start(); QVERIFY(panel.active());
        QTRY_VERIFY2(panel.findChild<QTreeWidget*>("debugStack")->topLevelItemCount()>0,
                     qPrintable(panel.findChild<QLabel*>("debugStatus")->text()+"\n"+output->toPlainText()));
        QVERIFY(QFileInfo::exists(marker)); QVERIFY(output->toPlainText().contains("fake OpenOCD log"));
        QVERIFY(!output->toPlainText().contains(QChar(0x1b)));
        bool initialized=false,launched=false;
        for(const auto& event:events) {
            const auto name=event[0].toString(); const auto body=event[1].toJsonObject();
            if(name=="turtle-test-initialize") {QCOMPARE(body.value("adapterID").toString(),QString("fixture")); initialized=true;}
            if(name=="turtle-test-launch") {
                QCOMPARE(body.value("executable").toString(),program);
                QVERIFY(body.value("gdbServerConsolePort").isDouble());
                QCOMPARE(body.value("nested").toObject().value("paths").toArray()[1].toString(),program);
                QCOMPARE(body.value("args").toArray()[0].toString(),QString("a b")); launched=true;
            }
        }
        QVERIFY(initialized && launched);
        panel.perform("stop"); QTRY_VERIFY(!panel.active());
        // Attach uses the same generic adapter transport without LLDB launch options.
        config["request"]="attach"; config.remove("preLaunch");
        QVERIFY(writeConfiguration(project.path(),config)); panel.start();
        QTRY_VERIFY(panel.findChild<QTreeWidget*>("debugStack")->topLevelItemCount()>0);
        panel.perform("stop"); QTRY_VERIFY(!panel.active());
        // Build failure stops the sequence and never starts an adapter.
        QVERIFY(QFile::remove(marker)); events.clear();
        config["preLaunch"]=QJsonArray{QJsonArray{python,"-c","import sys; sys.exit(7)"},
            QJsonArray{python,"-c","import pathlib,sys; pathlib.Path(sys.argv[1]).touch()",marker}};
        QVERIFY(writeConfiguration(project.path(),config)); panel.start(); QTRY_VERIFY(!panel.active());
        QVERIFY(!QFileInfo::exists(marker)); QVERIFY(events.isEmpty());
        QVERIFY(output->toPlainText().contains("Build failed"));
        // Cancelling preparation stops the build before adapter initialization.
        config["preLaunch"]=QJsonArray{QJsonArray{python,"-c",
            "import subprocess,sys,time; subprocess.Popen([sys.executable,'-c',"
            "'import pathlib,sys,time; time.sleep(0.6); pathlib.Path(sys.argv[1]).touch()',sys.argv[1]]); "
            "print('child ready',flush=True); time.sleep(30)",marker}};
        QVERIFY(writeConfiguration(project.path(),config)); panel.start(); QVERIFY(panel.active());
        QTRY_VERIFY(output->toPlainText().contains("child ready"));
        panel.perform("stop"); QTRY_VERIFY(!panel.active()); QVERIFY(events.isEmpty());
        QTest::qWait(800); QVERIFY(!QFileInfo::exists(marker));
        // A missing build executable also returns the UI to an idle state.
        config["preLaunch"]=QJsonArray{QJsonArray{"/no/such/build-command"}};
        QVERIFY(writeConfiguration(project.path(),config)); panel.start(); QTRY_VERIFY(!panel.active());
        QVERIFY(events.isEmpty());
        for(const auto& bad:QList<QJsonObject>{QJsonObject{{"adapterID",""}},
            QJsonObject{{"adapterArgs",QJsonArray{12}}},QJsonObject{{"preLaunch",QJsonArray{"shell string"}}},
            QJsonObject{{"request","restart"}},QJsonObject{{"launchArguments",QJsonObject{{"path","${unknown}"}}}}}) {
            auto invalid=config; for(auto it=bad.begin();it!=bad.end();++it) invalid[it.key()]=it.value();
            QVERIFY(writeConfiguration(project.path(),invalid)); DebugConfiguration parsed; QString error;
            QVERIFY(!DebugConfiguration::read(project.path(),{},parsed,error));
        }
    }
    void fakeSession() {
        // Python wrapper keeps the same panel launch path as debugpy without external dependencies.
        QTemporaryDir dir; QFile file(dir.filePath("python")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nexec python3 '" DEBUG_SOURCE "/tests/debug/fake_adapter.py'\n"); file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        // Fake adapter emits the same final output expected of the real fixture.
        exercise(file.fileName());
    }
    void terminalInput_data() {
        QTest::addColumn<bool>("native"); QTest::newRow("Python") << false; QTest::newRow("C++") << true;
    }
    void terminalInput() {
        QFETCH(bool,native);
        const auto adapter=qEnvironmentVariable(native?"TURTLE_TEST_LLDB_DAP":"TURTLE_TEST_DEBUG_PYTHON");
        if(adapter.isEmpty()) QSKIP("Set adapter environment to run interactive integration");
        QSettings().clear(); DebugPanel panel; TerminalPanel terminal;
        terminal.resize(900,360); terminal.setDebugConsole(panel.console()); panel.setTerminalAvailable(true);
        connect(&panel,&DebugPanel::terminalRequested,&terminal,&TerminalPanel::runDebugProgram);
        connect(&panel,&DebugPanel::terminalStopRequested,&terminal,&TerminalPanel::stopDebugProgram);
        connect(&terminal,&TerminalPanel::debugTerminalReply,&panel,&DebugPanel::terminalReply);
        const auto program=native?QString(DEBUG_INPUT_PROGRAM):QString(DEBUG_SOURCE "/examples/debug/input.py");
        QTemporaryDir project; panel.setWorkspace(project.path()); panel.useFile(program);
        auto config=DebugConfiguration::defaults(!native); config["program"]=program; config[native?"adapter":"python"]=adapter;
        QVERIFY(writeConfiguration(project.path(),config));
        auto* io=terminal.findChild<TerminalWidget*>("debugProgramTerminal"); QVERIFY(io);
        QSignalSpy terminalReplies(&terminal,&TerminalPanel::debugTerminalReply);
        for(int run=0;run<2;++run) {
            panel.start();
            QTRY_VERIFY_WITH_TIMEOUT(terminalReplies.size()>run,20000);
            QVERIFY2(terminalReplies.last().at(1).toBool(),qPrintable(terminalReplies.last().at(3).toString()));
            QTRY_VERIFY_WITH_TIMEOUT(io->screenText().contains("Number:"),20000);
            emit io->input("21\n");
            QTRY_VERIFY_WITH_TIMEOUT(io->screenText().contains("Input result: 42"),10000);
            QTRY_VERIFY_WITH_TIMEOUT(!panel.active(),10000);
        }
        panel.start();
        QTRY_VERIFY_WITH_TIMEOUT(terminalReplies.size()>2,20000);
        QTRY_VERIFY_WITH_TIMEOUT(io->screenText().contains("Number:"),20000);
        panel.perform("stop"); QTRY_VERIFY_WITH_TIMEOUT(!panel.active(),7000);
        for(auto* session:terminal.findChildren<TerminalSession*>()) QTRY_VERIFY_WITH_TIMEOUT(!session->isRunning(),3000);
    }
    void failureAndUnavailableSource() {
        QSettings().clear(); QTemporaryDir project;
        const auto wrapper=project.filePath("adapter"); QFile launcher(wrapper); QVERIFY(launcher.open(QIODevice::WriteOnly));
        launcher.write("#!/bin/sh\nexec python3 '" DEBUG_SOURCE "/tests/debug/fake_adapter.py'\n"); launcher.close();
        QVERIFY(launcher.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        DebugPanel panel; panel.setWorkspace(project.path());
        auto config=DebugConfiguration::defaults(false); config["program"]=QString(DEBUG_INPUT_PROGRAM); config["adapter"]="/no/such/debug-adapter";
        QVERIFY(writeConfiguration(project.path(),config));
        QSignalSpy errors(panel.findChild<DapClient*>(),&DapClient::fatalError);
        auto* status=panel.findChild<QLabel*>("debugStatus"); QVERIFY(status);
        panel.start(); QTRY_VERIFY(!errors.isEmpty()); QTRY_VERIFY(!panel.active());
        QCOMPARE(status->text(),errors.first().at(0).toString());
        config["adapter"]=wrapper; config["args"]=QJsonArray{"--fail-launch"}; QVERIFY(writeConfiguration(project.path(),config));
        panel.start(); QTRY_VERIFY(status->text().contains("Cannot launch test fixture")); QTRY_VERIFY(!panel.active());
        QVERIFY(status->text().contains("Cannot launch test fixture"));
        config["args"]=QJsonArray{"--missing-source"}; QVERIFY(writeConfiguration(project.path(),config));
        panel.start(); QTRY_VERIFY(status->text().contains("Paused outside available source code"));
        QVERIFY(button(panel,"Step Over")->isEnabled()); panel.perform("stop"); QTRY_VERIFY(!panel.active());
        QCOMPARE(status->text(),QString("Session ended"));
    }
    void nativeSourceEntry() {
        const auto adapter=qEnvironmentVariable("TURTLE_TEST_LLDB_DAP"); if(adapter.isEmpty()) QSKIP("Set TURTLE_TEST_LLDB_DAP");
        QSettings().clear(); QTemporaryDir project; DebugPanel panel; TerminalPanel terminal;
        QTcpServer symbolsServer; QVERIFY(symbolsServer.listen(QHostAddress::LocalHost));
        QList<QTcpSocket*> accepted;
        connect(&symbolsServer,&QTcpServer::newConnection,this,[&]{while(symbolsServer.hasPendingConnections()) accepted.append(symbolsServer.nextPendingConnection());});
        const auto oldUrls=qgetenv("DEBUGINFOD_URLS"),oldCache=qgetenv("DEBUGINFOD_CACHE_PATH");
        const auto restore=qScopeGuard([&]{if(oldUrls.isNull()) qunsetenv("DEBUGINFOD_URLS"); else qputenv("DEBUGINFOD_URLS",oldUrls); if(oldCache.isNull()) qunsetenv("DEBUGINFOD_CACHE_PATH"); else qputenv("DEBUGINFOD_CACHE_PATH",oldCache);});
        qputenv("DEBUGINFOD_URLS",("http://127.0.0.1:"+QString::number(symbolsServer.serverPort())).toUtf8());
        qputenv("DEBUGINFOD_CACHE_PATH",project.filePath("symbol-cache").toUtf8());
        terminal.resize(900,360); terminal.setDebugConsole(panel.console()); panel.setTerminalAvailable(true);
        connect(&panel,&DebugPanel::terminalRequested,&terminal,&TerminalPanel::runDebugProgram);
        connect(&panel,&DebugPanel::terminalStopRequested,&terminal,&TerminalPanel::stopDebugProgram);
        connect(&terminal,&TerminalPanel::debugTerminalReply,&panel,&DebugPanel::terminalReply);
        panel.setWorkspace(project.path());
        auto config=DebugConfiguration::defaults(false); config["program"]=QString(DEBUG_INPUT_PROGRAM); config["adapter"]=adapter; config["stopOnEntry"]=true;
        QVERIFY(writeConfiguration(project.path(),config)); QSignalSpy locations(&panel,&DebugPanel::locationChanged);
        auto* frames=panel.findChild<QTreeWidget*>("debugStack"); auto* variables=panel.findChild<QTreeWidget*>("debugVariables");
        panel.start(); QTRY_VERIFY_WITH_TIMEOUT(frames->currentItem()!=nullptr,5000);
        QCOMPARE(accepted.size(),0);
        QVERIFY(frames->currentItem()->text(0).contains("main"));
        const auto source=frames->currentItem()->data(0,Qt::UserRole+1).toString();
        QVERIFY2(source.endsWith("input.cpp"),qPrintable(source)); QVERIFY(QFileInfo(source).isFile());
        QVERIFY(frames->currentItem()->data(0,Qt::UserRole+2).toInt()>0);
        QTRY_VERIFY_WITH_TIMEOUT(variables->topLevelItemCount()>0,10000);
        QVERIFY(button(panel,"Step Over")->isEnabled());
        bool highlighted=false; for(const auto& location:locations) if(location.at(0).toString()==source && location.at(1).toInt()>0) highlighted=true;
        QVERIFY(highlighted);
        panel.perform("next"); QTRY_VERIFY_WITH_TIMEOUT(frames->currentItem()!=nullptr,10000);
        QVERIFY(frames->currentItem()->data(0,Qt::UserRole+1).toString().endsWith("input.cpp"));
        panel.perform("start");
        auto* io=terminal.findChild<TerminalWidget*>("debugProgramTerminal");
        QTRY_VERIFY_WITH_TIMEOUT(io->screenText().contains("Number:"),10000); emit io->input("21\n");
        QTRY_VERIFY_WITH_TIMEOUT(io->screenText().contains("Input result: 42"),10000); QTRY_VERIFY_WITH_TIMEOUT(!panel.active(),10000);
    }
    void realPython() {
        const auto python=qEnvironmentVariable("TURTLE_TEST_DEBUG_PYTHON"); if(python.isEmpty()) QSKIP("Set TURTLE_TEST_DEBUG_PYTHON to an interpreter with debugpy"); exercise(python);
    }
    void realCpp() {
        const auto adapter=qEnvironmentVariable("TURTLE_TEST_LLDB_DAP"); if(adapter.isEmpty()) QSKIP("Set TURTLE_TEST_LLDB_DAP and TURTLE_TEST_CPP_PROGRAM"); exercise("python3",adapter);
    }
};
QTEST_MAIN(DebugTests)
#include "DebugTests.moc"
