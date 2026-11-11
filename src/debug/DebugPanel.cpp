#include "app/IconDialogs.hpp"
#include "DebugPanel.hpp"
#include "DapClient.hpp"
#include "DebugViews.hpp"
#include "app/FontSettings.hpp"
#include <QHeaderView>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QPersistentModelIndex>
#include <QShortcut>
#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextDocument>
#include <QTreeWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QSettings>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStringDecoder>
#include <QRegularExpression>
#include <QDir>
#include <QTimer>
#include <QSignalBlocker>
#include <QCryptographicHash>
#include <QInputDialog>
#include <QPushButton>
#include <QFileSystemWatcher>
#include <QFile>
#include <memory>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <unistd.h>
#include <signal.h>
#endif

namespace { QString t(const char* s) { return I18n::text(s); } }
using namespace DebugViews;
DebugPanel::DebugPanel(QWidget* parent):QWidget(parent),client_(new DapClient(this)) {
    setObjectName("debugPanel");
    auto* layout=new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    auto* headingWidget=new QWidget(this); headingWidget->setObjectName("debugHeading");
    auto* heading=new QHBoxLayout(headingWidget); heading->setContentsMargins(12,3,5,3); heading->setSpacing(2);
    auto* title=new DebugViews::ElidedLabel(t("RUN AND DEBUG"),this); title->setObjectName("debugHeadingTitle");
    title->setToolTip(title->text()); title->setMaximumWidth(66); title->setMinimumWidth(0);
    title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); heading->addWidget(title,1);
    auto button=[&](QWidget* parent,const char* icon,const char* text) {
        auto* b=new QToolButton(parent); b->setIcon(Codicons::icon(icon)); b->setToolTip(t(text));
        b->setAccessibleName(t(text)); b->setAutoRaise(true); b->setIconSize({16,16}); b->setFixedSize(24,24); return b;
    };
    start_=button(headingWidget,"debug-start","Start Debugging"); start_->setObjectName("debugStart"); heading->addWidget(start_);
    const auto updateIcons=[this] {
        const auto& theme=ThemeManager::instance();
        start_->setIcon(Codicons::icon("debug-start",theme.color("syntax.string")));
        resume_->setIcon(Codicons::icon("debug-continue",theme.color("syntax.string")));
        pause_->setIcon(Codicons::icon("debug-pause",theme.color("diagnostic.warning")));
        next_->setIcon(Codicons::icon("debug-step-over",theme.color("accent")));
        stepIn_->setIcon(Codicons::icon("debug-step-into",theme.color("accent")));
        stepOut_->setIcon(Codicons::icon("debug-step-out",theme.color("accent")));
        stop_->setIcon(Codicons::icon("debug-stop",theme.color("diagnostic.error")));
        rebuildBreakpoints(); updateFormatButtons();
    };
    connect(&ThemeManager::instance(),&ThemeManager::themeChanged,this,updateIcons);
    configurations_=new DebugViews::ConfigurationCombo(this); configurations_->setObjectName("debugConfigurations");
    configurations_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); configurations_->setMinimumWidth(40);
    configurations_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    configurations_->setMinimumContentsLength(4); configurations_->setToolTip(t("Debug configuration")); heading->addWidget(configurations_,3);
    connect(configurations_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index) {
        if(index>=0 && !workspace_.isEmpty()) QSettings().setValue(settingsKey()+"/selectedConfiguration",configurations_->currentData());
    });
    settingsButton_=button(headingWidget,"gear","Open debug configuration"); settingsButton_->setObjectName("debugOpenConfiguration"); heading->addWidget(settingsButton_);
    auto* more=button(headingWidget,"ellipsis","More actions"); more->setObjectName("debugMoreActions");
    auto* menu=new QMenu(more); more->setMenu(menu); more->setPopupMode(QToolButton::InstantPopup); heading->addWidget(more);
    auto* configureAction=menu->addAction(Codicons::icon("gear"),t("Open debug configuration"));
    connect(configureAction,&QAction::triggered,this,&DebugPanel::editConfiguration);
    layout->addWidget(headingWidget);
    debugToolbar_=new QWidget(this); debugToolbar_->setObjectName("debugToolbar");
    auto* bar=new QHBoxLayout(debugToolbar_); bar->setContentsMargins(8,0,8,0); bar->setSpacing(2);
    auto control=[&](const char* icon,const char* text) {auto* b=button(debugToolbar_,icon,text); bar->addWidget(b); return b;};
    resume_=control("debug-continue","Continue"); pause_=control("debug-pause","Pause"); next_=control("debug-step-over","Step Over");
    stepIn_=control("debug-step-into","Step Into"); stepOut_=control("debug-step-out","Step Out"); stop_=control("debug-stop","Stop Debugging");
    bar->addStretch(); layout->addWidget(debugToolbar_);
    configureButton_=new QPushButton(t("Configure"),this); configureButton_->setObjectName("debugConfigure");
    configureButton_->setIcon(Codicons::icon("gear")); layout->addWidget(configureButton_);
    connect(configureButton_,&QPushButton::clicked,this,&DebugPanel::editConfiguration);
    connect(settingsButton_,&QToolButton::clicked,this,&DebugPanel::editConfiguration);
    configurationWatcher_=new QFileSystemWatcher(this);
    connect(configurationWatcher_,&QFileSystemWatcher::fileChanged,this,[this]{refreshConfiguration();});
    connect(configurationWatcher_,&QFileSystemWatcher::directoryChanged,this,[this]{refreshConfiguration();});
    status_=new QLabel(this); status_->setObjectName("debugStatus"); status_->setWordWrap(true); layout->addWidget(status_);
    auto* sections=new QSplitter(Qt::Vertical,this); sections->setObjectName("debugSections");
    sections->setChildrenCollapsible(false); sections->setHandleWidth(1); layout->addWidget(sections,1);
    auto tree=[&](const char* id,const char* name,const QStringList& headers,bool values) {
        auto* w=new QTreeWidget; w->setObjectName(id); w->setHeaderLabels(headers); w->setHeaderHidden(true);
        w->setFrameShape(QFrame::NoFrame); w->setIndentation(16); w->setUniformRowHeights(true); w->setRootIsDecorated(true);
        w->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); w->setMinimumSize(0,0);
        if(values) {
            w->setFont(FontSettings::font(FontSettings::Area::Editor)); w->setItemDelegate(new DebugViews::ValueDelegate(w));
            w->setColumnHidden(1,true); w->header()->setSectionResizeMode(0,QHeaderView::Stretch);
        }
        w->header()->setStretchLastSection(true);
        auto* section=new DebugViews::Section(id,t(name),w,sections); return std::make_pair(w,section);
    };
    auto variablesView=tree("debugVariables","VARIABLES",{t("Name"),t("Value")},true); variables_=variablesView.first;
    auto watches=tree("debugWatches","WATCH",{t("Expression"),t("Value")},true); watches_=watches.first;
    auto frames=tree("debugStack","CALL STACK",{t("Frame")},false); frames_=frames.first;
    auto breakpoints=tree("debugBreakpoints","BREAKPOINTS",{t("Name")},false); breakpoints_=breakpoints.first;
    sections->setSizes({280,400,180,80});
    const auto saved=QSettings().value("debug/ui/sectionSizes").toByteArray(); if(!saved.isEmpty()) sections->restoreState(saved);
    connect(sections,&QSplitter::splitterMoved,this,[sections]{QSettings().setValue("debug/ui/sectionSizes",sections->saveState());});
    auto* add=button(breakpoints.second,"add","Toggle breakpoint at cursor"); add->setObjectName("debugAddBreakpoint"); breakpoints.second->actions->addWidget(add);
    auto* remove=button(breakpoints.second,"trash","Remove selected breakpoint"); breakpoints.second->actions->addWidget(remove);
    for(auto* section:{variablesView.second,watches.second}) {
        auto* hex=button(section,"symbol-number","Hexadecimal values");
        hex->setObjectName(section==variablesView.second?"debugVariablesHex":"debugWatchesHex");
        hex->setCheckable(true); section->actions->addWidget(hex); formatButtons_.append(hex);
        connect(hex,&QToolButton::clicked,this,[this]{setHexValues(!hexValues_);});
    }
    auto* watchAdd=button(watches.second,"add","Add watch"); watchAdd->setObjectName("debugAddWatch"); watches.second->actions->addWidget(watchAdd);
    auto* watchRemove=button(watches.second,"trash","Remove watch"); watchRemove->setObjectName("debugRemoveWatchButton"); watches.second->actions->addWidget(watchRemove);
    connect(watchAdd,&QToolButton::clicked,this,[this]{editWatch();});
    connect(watchRemove,&QToolButton::clicked,this,[this]{ auto* item=watches_->currentItem(); if(!item) return; if(item->parent()) return; state_.watches.removeAll(item->text(0)); saveState(); refreshWatches(); });
    console_=new QWidget(this); console_->setObjectName("debugConsole"); auto* consoleLayout=new QVBoxLayout(console_); consoleLayout->setContentsMargins(4,4,4,4);
    output_=new QPlainTextEdit; output_->setObjectName("debugOutput"); output_->setReadOnly(true); output_->setMaximumBlockCount(1500); consoleLayout->addWidget(output_);
    evaluate_=new QLineEdit; evaluate_->setObjectName("debugEvaluate"); evaluate_->setPlaceholderText(t("Evaluate expression in selected frame")); consoleLayout->addWidget(evaluate_);
    connect(evaluate_,&QLineEdit::returnPressed,this,[this]{ const auto expression=evaluate_->text().trimmed(); if(expression.isEmpty() || !session_.stopped) return; evaluate_->clear(); evaluate(expression); });
    console_->hide();
    loadState();
    connect(breakpoints_,&QTreeWidget::itemChanged,this,[this](QTreeWidgetItem* item,int){
        const auto path=item->data(0,Qt::UserRole).toString(); const int line=item->data(0,Qt::UserRole+1).toInt();
        if(item->checkState(0)==Qt::Checked) state_.disabled[path].remove(line); else state_.disabled[path].insert(line);
        session_.verified[path].remove(line); saveState(); emit breakpointsChanged(path);
        if(active() && !session_.configuring && !session_.ending) sendBreakpoints(path);
    });
    connect(add,&QToolButton::clicked,this,&DebugPanel::breakpointRequested);
    connect(remove,&QToolButton::clicked,this,[this]{ auto* item=breakpoints_->currentItem(); if(item) toggleBreakpoint(item->data(0,Qt::UserRole).toString(),item->data(0,Qt::UserRole+1).toInt()); });
    connect(start_,&QToolButton::clicked,this,&DebugPanel::startRequested);
    connect(stop_,&QToolButton::clicked,this,[this]{ session_.ending=true; clearLocation(); setState(t("Stopping…")); if(preparing_) {
        if(preparation_->state()!=QProcess::NotRunning) stopPreparation();
        else finishPreparation();
    } else client_->stop(); });
    connect(pause_,&QToolButton::clicked,this,[this]{
        if(session_.thread>0) client_->request("pause",{{"threadId",session_.thread}});
        else client_->request("threads",{},[this](const QJsonObject& r){
            const auto threads=r.value("body").toObject().value("threads").toArray();
            if(!threads.isEmpty() && !session_.stopped && !session_.ending) { session_.thread=threads[0].toObject().value("id").toInt(); client_->request("pause",{{"threadId",session_.thread}}); }
        });
    });
    auto run=[this](const QString& command){ if(!session_.stopped) return; clearLocation(); setState(t("Running")); const auto epoch=session_.epoch;
        client_->request(command,{{"threadId",session_.thread}},[this,epoch](const QJsonObject& r){
            if(!r.value("success").toBool() && epoch==session_.epoch && !session_.ending) { setState(t("Paused"),true); stack(); }
        }); };
    connect(resume_,&QToolButton::clicked,this,[run]{run("continue");});
    connect(next_,&QToolButton::clicked,this,[run]{run("next");});
    connect(stepIn_,&QToolButton::clicked,this,[run]{run("stepIn");});
    connect(stepOut_,&QToolButton::clicked,this,[run]{run("stepOut");});
    connect(client_,&DapClient::ready,this,[this]{
        setStatus(t("Initializing debugger…"));
        client_->request("initialize",{{"clientID","turtle-code"},{"adapterID",configuration_.type=="python"?"debugpy":configuration_.type=="dap"?configuration_.adapterID:"lldb"},{"linesStartAt1",true},{"columnsStartAt1",true},{"pathFormat","path"},{"supportsRunInTerminalRequest",terminalAvailable_},{"supportsVariableType",true}},[this](const QJsonObject& response){
            if(!response.value("success").toBool()) { client_->stop(); return; }
            session_.capabilities=response.value("body").toObject(); launch();
        });
    });
    connect(client_,&DapClient::reverseRequest,this,[this](const QJsonObject& request){
        if(terminalAvailable_ && !session_.ending) {
            setStatus(t("Waiting for debugger to attach…"));
            auto terminalRequest=request; auto arguments=request.value("arguments").toObject(); auto command=arguments.value("args").toArray();
            // LLDB relaunches itself as a terminal helper. Keep the configured
            // launcher (which may establish shared-library paths) for that helper.
            bool helper=false; for(const auto& value:command) if(value.toString()=="--launch-target") helper=true;
            if(configuration_.type=="cpp" && helper && !command.isEmpty() && QFileInfo(command.first().toString()).fileName()=="lldb-dap") {
                command[0]=configuration_.adapter; arguments["args"]=command; terminalRequest["arguments"]=arguments;
            }
            emit terminalRequested(terminalRequest);
        }
        else client_->respond(request,false,{},"Integrated terminal is unavailable");
    });
    connect(client_,&DapClient::eventReceived,this,&DebugPanel::event);
    connect(client_,&DapClient::fatalError,this,[this](const QString& message){
        if(session_.lastError.isEmpty()) session_.lastError=message;
        emit consoleRequested();
    });
    connect(client_,&DapClient::error,this,[this](const QString& message){ append(message+"\n"); setStatus(message); });
    connect(client_,&DapClient::finished,this,[this]{ clearLocation(); emit terminalStopRequested(); serverConsole_->close(); for(auto* socket:serverConsole_->findChildren<QTcpSocket*>()) socket->deleteLater(); session_.ending=false; session_.configuring=false; setState(session_.lastError.isEmpty()?t("Session ended"):session_.lastError); if(workspacePending_) {workspacePending_=false; setWorkspace(pendingWorkspace_);} });
    connect(frames_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item){
        if(!item || !session_.stopped) return;
        const auto path=item->data(0,Qt::UserRole+1).toString(); const int line=item->data(0,Qt::UserRole+2).toInt();
        if(line>0 && QFileInfo(path).isFile()) {setStatus(t("Paused")+": "+QFileInfo(path).fileName()+":"+QString::number(line)); emit locationChanged(path,line);}
        else {setStatus(t("Paused outside available source code; choose a frame or continue")); emit locationChanged({},0);}
        scopes(item->data(0,Qt::UserRole).toInt());
    });
    for(auto* tree:{variables_,watches_}) {
        connect(tree,&QTreeWidget::itemCollapsed,this,[this](QTreeWidgetItem* item){valueExpansion_[valuePath(item)]=false;});
        connect(tree,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item){valueExpansion_[valuePath(item)]=true;});
    }
    connect(variables_,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item){
        const int ref=item->data(0,Qt::UserRole).toInt(); if(ref<=0 || item->data(0,Qt::UserRole+1).toBool()) return;
        item->setData(0,Qt::UserRole+1,true); variables(item,ref);
    });
    connect(watches_,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item){
        const int ref=item->data(0,Qt::UserRole).toInt(); if(ref<=0 || item->data(0,Qt::UserRole+1).toBool()) return;
        item->setData(0,Qt::UserRole+1,true); variables(item,ref);
    });
    connect(breakpoints_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* item){ emit sourceActivated(item->data(0,Qt::UserRole).toString(),item->data(0,Qt::UserRole+1).toInt()); });
    preparation_=new QProcess(this);
    preparation_->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    preparation_->setChildProcessModifier([]{::setpgid(0,0);});
#endif
    connect(preparation_,&QProcess::readyReadStandardOutput,this,[this]{append(QString::fromUtf8(preparation_->readAllStandardOutput()));});
    connect(preparation_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart) finishPreparation(t("Build failed")+": "+preparation_->errorString());
    });
    connect(preparation_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus status) {
        append(QString::fromUtf8(preparation_->readAllStandardOutput()));
        if(session_.ending) finishPreparation();
        else if(code!=0 || status!=QProcess::NormalExit) finishPreparation(t("Build failed")+" ("+QString::number(code)+")");
        else prepareNext();
    });
    serverConsole_=new QTcpServer(this);
    connect(serverConsole_,&QTcpServer::newConnection,this,[this] {
        while(serverConsole_->hasPendingConnections()) {
            auto* socket=serverConsole_->nextPendingConnection();
            const auto decoder=std::make_shared<QStringDecoder>(QStringDecoder::Utf8);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket,decoder]{
                QString text=(*decoder)(socket->readAll());
                static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-?]*[ -/]*[@-~]"));
                text.remove(ansi); append(text);
            });
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        }
    });
    hexValues_=QSettings().value("debug/ui/hexValues",false).toBool();
    initContextMenus();
    updateIcons();
    setState(t("Ready to debug"));
}
void DebugPanel::perform(const QString& command) {
    if(command=="start") { if(session_.stopped) resume_->click(); else if(!active()) emit startRequested(); }
    else if(command=="stop") stop_->click();
    else if(command=="next") next_->click();
    else if(command=="stepIn") stepIn_->click();
    else if(command=="stepOut") stepOut_->click();
}
bool DebugPanel::active() const { return preparing_ || client_->active(); }
void DebugPanel::useFile(const QString& path) {
    if(active() || path.isEmpty())return;
    for(const auto& name:{"launch.json","tasks.json","settings.json","debug.json"})if(path==DebugProject::path(workspace_,name))return;
    currentFile_=path;
}
void DebugPanel::append(const QString& text) {
    const auto chunk=text.left(65536);
    const int excess=output_->document()->characterCount()+int(chunk.size())-256*1024;
    auto cursor=output_->textCursor();
    if(excess>0) { cursor.movePosition(QTextCursor::Start); cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor,excess); cursor.removeSelectedText(); }
    cursor.movePosition(QTextCursor::End); cursor.insertText(chunk); output_->setTextCursor(cursor);
}
void DebugPanel::setStatus(const QString& text) {
    status_->setText(text); status_->setVisible(text!=t("Ready to debug") && text!=t("Session ended"));
    setToolTip(text);
}
void DebugPanel::setState(const QString& text,bool stopped) {
    session_.stopped=stopped; evaluate_->setEnabled(stopped); setStatus(text);
    const bool running=active(); debugToolbar_->setVisible(running); start_->setEnabled(!running); stop_->setEnabled(running && !session_.ending);
    resume_->setEnabled(stopped); next_->setEnabled(stopped); stepIn_->setEnabled(stopped); stepOut_->setEnabled(stopped);
    pause_->setEnabled(running && !stopped && !session_.configuring && !session_.ending);
    refreshConfiguration(); updateFormatButtons();
}
void DebugPanel::clearLocation() { ++session_.epoch; session_.frame=-1; session_.stopped=false; refreshWatches(); frames_->clear(); variables_->clear(); updateFormatButtons(); emit locationChanged({},0); }
void DebugPanel::start() {
    if(active()) return;
    ++formatRequestGeneration_;
    emit currentFileRequested();
    if(workspace_.isEmpty()) {setStatus(t("Open a project folder to configure debugging")); return;}
    if(!QFileInfo(configurationPath()).isFile()) {refreshConfiguration(); setStatus(t("Click Configure to create .turtle/launch.json")); return;}
    QString error;
    if(!DebugConfiguration::read(workspace_,currentFile_,configuration_,error,configurations_->currentData().toString())) {setStatus(t("Invalid debug configuration")+": "+error); emit consoleRequested(); append(t("Invalid debug configuration")+": "+error+"\n"); return;}
    session_.lastError.clear();
    emit consoleRequested();
    clearLocation(); session_.editedSources.clear(); output_->clear(); session_.verified.clear(); session_.serverPoints.clear(); rebuildBreakpoints(); session_.configuring=true; session_.ending=false; session_.thread=0;
    preparing_=true; preparationIndex_=0;
    prepareNext();
}

void DebugPanel::stopPreparation() {
#ifdef Q_OS_UNIX
    const auto pid=preparation_->processId();
    if(pid>0) ::kill(-pid,SIGKILL);
#endif
    preparation_->kill();
}

void DebugPanel::finishPreparation(const QString& error) {
    preparing_=false; session_.ending=false; session_.configuring=false;
    serverConsole_->close();
    if(!error.isEmpty()) { session_.lastError=error; append(error+"\n"); }
    setState(error.isEmpty()?t("Session ended"):error);
    if(workspacePending_) {workspacePending_=false; setWorkspace(pendingWorkspace_);}
}

void DebugPanel::prepareNext() {
    if(!preparing_) return;
    const int count=configuration_.buildSteps.isEmpty()?configuration_.preLaunch.size():configuration_.buildSteps.size();
    if(preparationIndex_>=count) {
        if(!QFileInfo(configuration_.program).isFile()) {finishPreparation(t("Program does not exist:")+" "+configuration_.program); return;}
        preparing_=false; startAdapter(); return;
    }
    QStringList command;QString cwd=configuration_.cwd;QJsonObject env;
    if(configuration_.buildSteps.isEmpty()) for(const auto& value:configuration_.preLaunch[preparationIndex_].toArray())command.append(value.toString());
    else {const auto& step=configuration_.buildSteps[preparationIndex_];command=step.command;cwd=step.cwd;env=step.env;}
    ++preparationIndex_;
    const auto executable=command.takeFirst();
    append("> "+executable+" "+command.join(' ')+"\n");
    setState(t("Building before debugging…"));
    preparation_->setWorkingDirectory(cwd);preparation_->setProcessEnvironment(DebugProject::environment(env));
    preparation_->start(executable,command);
}

void DebugPanel::startAdapter() {
    const auto environment=DebugProject::environment(configuration_.env);
    session_.launchArguments={{"program",configuration_.program},{"cwd",configuration_.cwd},{"args",configuration_.arguments},{"stopOnEntry",configuration_.stopOnEntry}};
    if(configuration_.type=="python") {
        session_.launchArguments["python"]=QJsonArray{configuration_.python}; session_.launchArguments["console"]=terminalAvailable_?"integratedTerminal":"internalConsole";
        session_.launchArguments["redirectOutput"]=!terminalAvailable_; session_.launchArguments["justMyCode"]=true;
        client_->start(configuration_.python,{"-m","debugpy.adapter"},configuration_.cwd,environment);
    } else if(configuration_.type=="dap") {
        session_.launchArguments=configuration_.launchArguments;
        if(!session_.launchArguments.contains("cwd")) session_.launchArguments["cwd"]=configuration_.cwd;
        if(!session_.launchArguments.contains("program")) session_.launchArguments["program"]=configuration_.program;
        // Some adapters stream their GDB-server log to a frontend TCP console.
        // Bind only on loopback, using a fresh OS-assigned port for this session.
        const bool console=QJsonDocument(session_.launchArguments).toJson().contains("${serverConsolePort}");
        if(console && !serverConsole_->listen(QHostAddress::LocalHost)) {finishPreparation(serverConsole_->errorString()); return;}
        std::function<QJsonValue(QJsonValue)> ports=[&](QJsonValue value)->QJsonValue {
            if(value.isString()) {
                auto text=value.toString();
                if(text=="${serverConsolePort}") return int(serverConsole_->serverPort());
                text.replace("${serverConsolePort}",QString::number(serverConsole_->serverPort())); return text;
            }
            if(value.isArray()) {QJsonArray result; for(const auto& item:value.toArray()) result.append(ports(item)); return result;}
            if(value.isObject()) {auto result=value.toObject(); for(auto it=result.begin();it!=result.end();++it) it.value()=ports(it.value()); return result;}
            return value;
        };
        session_.launchArguments=ports(session_.launchArguments).toObject();
        QStringList arguments; for(const auto& item:configuration_.adapterArguments) arguments.append(item.toString());
        if(configuration_.adapterID=="debugpy" && !terminalAvailable_ && session_.launchArguments.value("console").toString()=="integratedTerminal") {
            session_.launchArguments["console"]="internalConsole";session_.launchArguments["redirectOutput"]=true;
        }
        client_->start(configuration_.adapter,arguments,configuration_.cwd,environment);
    } else {
        session_.launchArguments["runInTerminal"]=terminalAvailable_; session_.launchArguments["disableASLR"]=false;
        // External symbol lookup can block launch on debuginfod before initialized.
        // Keep ordinary source debugging local, without changing the process environment.
        if(!configuration_.externalSymbols) session_.launchArguments["initCommands"]=QJsonArray{"settings set symbols.enable-external-lookup false"};
        // LLDB's stopOnEntry stops in the ELF loader, without a usable source.
        // Our C/C++ source-level entry is a one-shot breakpoint at main instead.
        if(configuration_.stopOnEntry) {
            session_.launchArguments["stopOnEntry"]=false;
            session_.launchArguments["preRunCommands"]=QJsonArray{"breakpoint set --name main --one-shot true"};
        }
        client_->start(configuration_.adapter,{},configuration_.cwd,environment);
    }
    setState(t("Starting debugger…"));
}
void DebugPanel::launch() {
    if(cortexValueFormat()) {
        if(QSettings().contains("debug/ui/hexValues")) session_.launchArguments["variableUseNaturalFormat"]=!hexValues_;
        else hexValues_=!session_.launchArguments.value("variableUseNaturalFormat").toBool(true);
    }
    setStatus(t("Launching program…")); client_->request(configuration_.request,session_.launchArguments,[this](const QJsonObject& response){ if(!response.value("success").toBool()) { session_.ending=true; client_->stop(); } }); }
void DebugPanel::configure() {
    if(!session_.stopped) setStatus(t("Applying breakpoints…"));
    // All setBreakpoints responses precede configurationDone, including errors.
    auto remaining=std::make_shared<int>(state_.points.size());
    auto done=[this,remaining]{ if(--*remaining>0) return; client_->request("configurationDone",{},[this](const QJsonObject& response){ session_.configuring=false; if(!response.value("success").toBool()) client_->stop(); else if(!session_.stopped) setState(t("Running")); }); };
    if(state_.points.isEmpty()) { *remaining=1; done(); }
    else for(auto it=state_.points.cbegin();it!=state_.points.cend();++it) sendBreakpoints(it.key(),done);
}
void DebugPanel::sendBreakpoints(const QString& path,std::function<void()> done) {
    if(session_.editedSources.contains(path)) {if(done) done(); return;}
    QList<int> lines=(state_.points.value(path)-state_.disabled.value(path)).values(); std::sort(lines.begin(),lines.end()); QJsonArray list;
    for(int line:lines) list.append(QJsonObject{{"line",line}});
    const auto version=++session_.pointVersions[path];
    client_->request("setBreakpoints",{{"source",QJsonObject{{"path",path}}},{"breakpoints",list}},[this,path,lines,done,version](const QJsonObject& response){
        if(version!=session_.pointVersions.value(path)) { if(done) done(); return; }
        session_.verified[path].clear(); session_.pointMessages[path].clear();
        if(!response.value("success").toBool()) for(int line:lines) session_.pointMessages[path][line]=response.value("message").toString();
        const auto bps=response.value("body").toObject().value("breakpoints").toArray();
        for(auto it=session_.serverPoints.begin();it!=session_.serverPoints.end();) { if(it->first==path) it=session_.serverPoints.erase(it); else ++it; }
        for(int i=0;i<bps.size() && i<lines.size();++i) applyBreakpoint(path,lines[i],bps[i].toObject());
        rebuildBreakpoints(); if(done) done();
    });
}
void DebugPanel::toggleBreakpoint(const QString& path,int line) {
    if(path.isEmpty() || line<1) return;
    if(state_.points[path].contains(line)) state_.points[path].remove(line); else state_.points[path].insert(line);
    state_.disabled[path].remove(line); session_.verified[path].remove(line); saveState(); rebuildBreakpoints(); emit breakpointsChanged(path);
    if(active() && !session_.configuring && !session_.ending) sendBreakpoints(path);
}
void DebugPanel::applyBreakpoint(const QString& path,int requested,const QJsonObject& bp) {
    // Preserve the requested source anchor; adapter locations are session data.
    if(!state_.points.value(path).contains(requested)) return;
    const int actual=bp.value("line").toInt(requested);
    session_.verified[path].remove(requested);
    QString message=bp.value("message").toString();
    if(message.isEmpty() && !bp.value("verified").toBool()) message=t("Breakpoint not verified");
    if(actual>0 && actual!=requested) message+=QString("\n%1: %2").arg(t("Adapter line")).arg(actual);
    session_.pointMessages[path][requested]=message.trimmed();
    if(bp.value("verified").toBool()) session_.verified[path].insert(requested);
    if(bp.contains("id")) session_.serverPoints.insert(bp.value("id").toInt(),{path,requested});
    emit breakpointsChanged(path);
}
void DebugPanel::rebuildBreakpoints() {
    const QSignalBlocker blocker(breakpoints_);
    breakpoints_->clear();
    for(auto it=state_.points.cbegin();it!=state_.points.cend();++it) {
        auto lines=it.value().values(); std::sort(lines.begin(),lines.end());
        for(int line:lines) { auto* item=new QTreeWidgetItem(breakpoints_,{QFileInfo(it.key()).fileName()+":"+QString::number(line)});
            item->setIcon(0,Codicons::icon(session_.verified.value(it.key()).contains(line)?"circle-filled":"circle-outline",ThemeManager::instance().color("diagnostic.error")));
            item->setCheckState(0,state_.disabled.value(it.key()).contains(line)?Qt::Unchecked:Qt::Checked);
            item->setToolTip(0,it.key()+"\n"+session_.pointMessages.value(it.key()).value(line)); item->setData(0,Qt::UserRole,it.key()); item->setData(0,Qt::UserRole+1,line); }
    }
}
void DebugPanel::event(const QString& name,const QJsonObject& body) {
    if(name=="output" && body.value("category").toString()!="telemetry") append(body.value("output").toString());
    else if(name=="breakpoint") {
        const auto bp=body.value("breakpoint").toObject(); const auto point=session_.serverPoints.value(bp.value("id").toInt(),{});
        if(!point.first.isEmpty()) {
            if(body.value("reason").toString()=="removed") session_.verified[point.first].remove(point.second);
            else applyBreakpoint(point.first,point.second,bp);
            rebuildBreakpoints();
        }
    }
    else if(name=="initialized") configure();
    else if(name=="stopped" && !session_.ending) {
        clearLocation(); session_.thread=body.value("threadId").toInt(); setState(t("Paused")+": "+body.value("reason").toString(),true);
        if(session_.thread>0) stack();
        else { const auto epoch=session_.epoch; client_->request("threads",{},[this,epoch](const QJsonObject& r){ if(epoch!=session_.epoch || !session_.stopped) return; const auto ts=r.value("body").toObject().value("threads").toArray(); if(!ts.isEmpty()) { session_.thread=ts[0].toObject().value("id").toInt(); stack(); } }); }
    } else if(name=="continued") { clearLocation(); setState(t("Running")); }
    else if(name=="terminated") { session_.ending=true; clearLocation(); setState(t("Stopping…")); client_->stop(); }
    else if(name=="exited") append(t("Exit code")+": "+QString::number(body.value("exitCode").toInt())+"\n");
}
void DebugPanel::stack() {
    const auto epoch=session_.epoch;
    client_->request("stackTrace",{{"threadId",session_.thread},{"startFrame",0},{"levels",100}},[this,epoch](const QJsonObject& r){
        if(epoch!=session_.epoch || !session_.stopped) return;
        const auto frames=r.value("body").toObject().value("stackFrames").toArray();
        for(const auto& value:frames) { const auto f=value.toObject(); const auto path=f.value("source").toObject().value("path").toString(); const int line=f.value("line").toInt();
            auto* item=new QTreeWidgetItem(frames_,{f.value("name").toString()+"  "+QFileInfo(path).fileName()+":"+QString::number(line)});
            item->setData(0,Qt::UserRole,f.value("id").toInt()); item->setData(0,Qt::UserRole+1,path); item->setData(0,Qt::UserRole+2,line);
        }
        QTreeWidgetItem* selected=nullptr;
        for(int i=0;i<frames_->topLevelItemCount();++i) {
            auto* item=frames_->topLevelItem(i);
            if(item->data(0,Qt::UserRole+2).toInt()>0 && QFileInfo(item->data(0,Qt::UserRole+1).toString()).isFile()) {selected=item; break;}
        }
        if(!selected && frames_->topLevelItemCount()) selected=frames_->topLevelItem(0);
        if(selected) frames_->setCurrentItem(selected);
        else setStatus(t("Paused; the debugger returned no stack frames"));
    });
}
void DebugPanel::scopes(int frame) {
    variables_->clear(); session_.frame=frame; updateFormatButtons(); const auto epoch=++session_.epoch; refreshWatches();
    client_->request("scopes",{{"frameId",frame}},[this,epoch](const QJsonObject& r){
        if(epoch!=session_.epoch || !session_.stopped) return;
        for(const auto& value:r.value("body").toObject().value("scopes").toArray()) { const auto s=value.toObject(); auto* item=new QTreeWidgetItem(variables_,{s.value("name").toString()});
            item->setFirstColumnSpanned(true); item->setData(0,Qt::UserRole,s.value("variablesReference").toInt()); item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); restoreValueExpansion(item,true);
        }
    });
}
void DebugPanel::variables(QTreeWidgetItem* parent,int reference) {
    const auto epoch=session_.epoch,generation=session_.watchGeneration; const bool watch=parent->treeWidget()==watches_;
    client_->request("variables",formatArguments({{"variablesReference",reference}}),[this,epoch,generation,parent,watch,reference](const QJsonObject& r){
        if(epoch!=session_.epoch || (watch && generation!=session_.watchGeneration) || !session_.stopped) return; // Parent survives only within this stop/frame generation.
        int count=0;
        for(const auto& value:r.value("body").toObject().value("variables").toArray()) { if(++count>1000) { new QTreeWidgetItem(parent,{t("More than 1000 variables; list truncated")}); break; }
            const auto v=value.toObject(); auto* item=new QTreeWidgetItem(parent,{v.value("name").toString(),v.value("value").toString()}); item->setToolTip(1,v.value("type").toString());
            item->setFirstColumnSpanned(true); item->setToolTip(0,v.value("name").toString()+": "+v.value("value").toString()+"\n"+v.value("type").toString());
            item->setData(0,ExpressionRole,v.value("evaluateName").toString());
            item->setData(0,ParentReferenceRole,reference); item->setData(0,HasValueRole,true);
            item->setData(0,ReadOnlyRole,v.value("presentationHint").toObject().value("attributes").toArray().contains("readOnly"));
            const int ref=v.value("variablesReference").toInt(); item->setData(0,Qt::UserRole,ref); if(ref>0) {item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); restoreValueExpansion(item);}
        }
    });
}

DebugPanel::~DebugPanel() {
    if(preparation_) {preparation_->disconnect(this); if(preparation_->state()!=QProcess::NotRunning) {stopPreparation(); preparation_->waitForFinished(1000);}}
    if(serverConsole_) {serverConsole_->disconnect(this); for(auto* socket:serverConsole_->findChildren<QTcpSocket*>()) socket->disconnect(this);}
    saveState();
}
QString DebugPanel::settingsKey() const {
    if(workspace_.isEmpty()) return "debug/standalone";
    return "debug/workspaces/"+QString::fromLatin1(QCryptographicHash::hash(workspace_.toUtf8(),QCryptographicHash::Sha256).toHex());
}
void DebugPanel::saveState() {
    auto json=state_.json();
    if(state_.configuration.isEmpty()) json.remove("configuration");
    QSettings().setValue(settingsKey(),QJsonDocument(json).toJson(QJsonDocument::Compact));
}
void DebugPanel::loadState() {
    state_.restore(QJsonDocument::fromJson(QSettings().value(settingsKey()).toByteArray()).object());
    if(QFileInfo(configurationPath()).isFile() && !state_.configuration.isEmpty()) {state_.configuration={}; saveState();}
    rebuildBreakpoints(); refreshWatches(); refreshConfiguration();
}

void DebugPanel::setWorkspace(const QString& root) {
    const auto path=root.isEmpty()?QString{}:QDir(root).absolutePath();
    if(path==workspace_) return;
    if(active()) {pendingWorkspace_=path; workspacePending_=true; return;}
    saveState(); valueExpansion_.clear(); const auto previous=state_.points.keys(); workspace_=path; session_.verified.clear(); session_.pointMessages.clear(); loadState();
    for(const auto& file:previous) emit breakpointsChanged(file);
    for(const auto& file:state_.points.keys()) emit breakpointsChanged(file);
}
void DebugPanel::moveBreakpoints(const QString& path,int row,int column,int removed,int inserted) {
    if(!state_.points.contains(path) || (removed==0 && inserted==0)) return;
    if(active()) {session_.editedSources.insert(path); setStatus(t("Source changed; breakpoints will apply on the next launch"));}
    state_.moveLines(path,row,column,removed,inserted); ++session_.pointVersions[path]; session_.verified[path].clear(); session_.pointMessages[path].clear();
    saveState(); rebuildBreakpoints(); emit breakpointsChanged(path);
    // Edited source no longer matches the running program. Apply anchors at the next launch.
}
void DebugPanel::terminalReply(const QJsonObject& request,bool success,const QJsonObject& body,const QString& message) { client_->respond(request,success,body,message); }
void DebugPanel::refreshWatches() {
    ++session_.watchGeneration;
    watches_->clear(); const auto epoch=session_.epoch,generation=session_.watchGeneration;
    for(const auto& expression:state_.watches) {
        auto* item=new QTreeWidgetItem(watches_,{expression,t("No data")});
        item->setFirstColumnSpanned(true); item->setData(0,UnavailableRole,true); item->setData(0,ExpressionRole,expression); item->setToolTip(0,expression);
        if(!session_.stopped || session_.frame<0) continue;
        client_->request("evaluate",formatArguments({{"expression",expression},{"frameId",session_.frame},{"context","watch"}}),[this,epoch,generation,item](const QJsonObject& r){
            if(epoch!=session_.epoch || generation!=session_.watchGeneration || !session_.stopped) return;
            const int reference=r.value("body").toObject().value("variablesReference").toInt();
            item->setData(0,Qt::UserRole,reference); if(reference>0) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            item->setData(0,UnavailableRole,!r.value("success").toBool());
            item->setData(0,HasValueRole,r.value("success").toBool());
            item->setData(0,ReadOnlyRole,r.value("body").toObject().value("presentationHint").toObject().value("attributes").toArray().contains("readOnly"));
            item->setText(1,r.value("success").toBool()?r.value("body").toObject().value("result").toString():r.value("message").toString());
            item->setToolTip(0,item->text(0)+": "+item->text(1));
            if(reference>0) restoreValueExpansion(item);
        });
    }
}
void DebugPanel::evaluate(const QString& expression) {
    if(!session_.stopped || session_.frame<0) return;
    append("> "+expression+"\n"); const auto epoch=session_.epoch;
    client_->request("evaluate",formatArguments({{"expression",expression},{"frameId",session_.frame},{"context","repl"}}),[this,epoch](const QJsonObject& r){
        if(epoch!=session_.epoch || !session_.stopped) return;
        append((r.value("success").toBool()?r.value("body").toObject().value("result").toString():r.value("message").toString())+"\n");
    });
}

void DebugPanel::renameSource(const QString& oldPath,const QString& newPath) {
    const auto files=state_.points.keys();
    for(const auto& path:files) if(path==oldPath || path.startsWith(oldPath+QDir::separator())) {
        const auto updated=newPath+path.mid(oldPath.size());
        state_.points[updated].unite(state_.points.take(path)); state_.disabled[updated].unite(state_.disabled.take(path));
        ++session_.pointVersions[path]; session_.verified.remove(path); session_.pointMessages.remove(path);
        emit breakpointsChanged(path); emit breakpointsChanged(updated);
    }
    rebuildBreakpoints(); saveState();
}

void DebugPanel::remapBreakpoints(const QString& path,const QMap<int,int>& anchors) {
    QSet<int> points,disabled;
    for(int line:state_.points.value(path)) {
        const int mapped=anchors.value(line,line); points.insert(mapped);
        if(state_.disabled.value(path).contains(line)) disabled.insert(mapped);
    }
    if(points==state_.points.value(path) && disabled==state_.disabled.value(path)) return;
    state_.points[path]=points; state_.disabled[path]=disabled;
    if(active()) {session_.editedSources.insert(path); setStatus(t("Source changed; breakpoints will apply on the next launch"));}
    ++session_.pointVersions[path]; session_.verified[path].clear(); session_.pointMessages[path].clear();
    saveState(); rebuildBreakpoints(); emit breakpointsChanged(path);
}

void DebugPanel::refreshConfiguration() {
    const auto watched=configurationWatcher_->files()+configurationWatcher_->directories();
    if(!watched.isEmpty()) configurationWatcher_->removePaths(watched);
    if(!workspace_.isEmpty()) {
        const QSet<QString> paths{workspace_,QDir(workspace_).filePath(".turtle"),configurationPath(),DebugProject::path(workspace_,"launch.json"),DebugProject::path(workspace_,"tasks.json"),DebugProject::path(workspace_,"settings.json")};
        for(const auto& path:paths)
            if(QFileInfo::exists(path)) configurationWatcher_->addPath(path);
    }
    const QSignalBlocker blocker(configurations_);
    const auto previous=QSettings().value(settingsKey()+"/selectedConfiguration").toString();
    configurations_->clear();
    const bool modern=configurationPath().endsWith("/launch.json");
    if(modern && QFileInfo::exists(configurationPath())) {
        QString error;const auto names=DebugProject::names(workspace_,error);
        for(const auto& name:names)configurations_->addItem(name,name);
        const auto selected=configurations_->findData(previous);if(selected>=0)configurations_->setCurrentIndex(selected);
        configurations_->setToolTip(error.isEmpty()?t("Debug configuration"):error);
    }
    configurations_->setVisible(modern && configurations_->count()>0);configurations_->setEnabled(!active());
    const bool exists=QFileInfo(configurationPath()).isFile();
    configureButton_->setVisible(!exists); configureButton_->setEnabled(!workspace_.isEmpty() && !active());
    settingsButton_->setVisible(exists); settingsButton_->setEnabled(!workspace_.isEmpty());
    start_->setEnabled(exists && !active());
    configureButton_->setToolTip(workspace_.isEmpty()?t("Open a project folder to configure debugging"):configurationPath());
    settingsButton_->setToolTip(configurationPath());
}
QJsonObject DebugPanel::configurationTemplate() const {
    auto result=DebugConfiguration::defaults(currentFile_.endsWith(".py",Qt::CaseInsensitive));
    const auto legacy=state_.configuration;
    if(legacy.value("program").toString().isEmpty()) return result;
    const bool python=legacy.value("kind").toInt()==0;
    result=DebugConfiguration::defaults(python);
    for(const auto& key:{"program","cwd","python","adapter"}) {
        if((QString(key)=="python" && !python) || (QString(key)=="adapter" && python)) continue;
        const auto value=legacy.value(key).toString();
        if(!value.isEmpty()) result[key]=DebugConfiguration::portable(value,workspace_);
    }
    if(legacy.value("cwd").toString().isEmpty()) result["cwd"]=DebugConfiguration::portable(QFileInfo(QDir(workspace_).absoluteFilePath(legacy.value("program").toString())).absolutePath(),workspace_);
    result["args"]=QJsonArray::fromStringList(QProcess::splitCommand(legacy.value("args").toString()));
    result["stopOnEntry"]=legacy.value("entry").toBool(false);
    return result;
}
void DebugPanel::editConfiguration() {
    emit currentFileRequested();
    if(workspace_.isEmpty()) {setStatus(t("Open a project folder to configure debugging")); return;}
    const auto path=state_.configuration.isEmpty()?configurationPath():DebugConfiguration::legacyPath(workspace_);
    if(!QFileInfo::exists(path)) {
        if(!QDir().mkpath(QFileInfo(path).absolutePath())) {setStatus(t("Cannot create debug configuration")+": "+path); return;}
        QFile file(path);
        if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {setStatus(file.errorString()+": "+path); refreshConfiguration(); return;}
        auto config=configurationTemplate();
        QJsonObject document=config;
        if(path.endsWith("/launch.json")) {
        config.remove("version");config["name"]=t("Launch");config["request"]="launch";
        config["type"]=config.value("type").toString()=="python"?"debugpy":"lldb";
        config.remove("adapter");config.remove("externalSymbols");
        document=QJsonObject{{"version","0.2.0"},{"configurations",QJsonArray{config}}};
        }
        const auto data=QJsonDocument(document).toJson(QJsonDocument::Indented);
        if(file.write(data)!=data.size() || !file.flush()) {const auto error=file.errorString(); file.close(); file.remove(); setStatus(error+": "+path); return;}
        file.close(); state_.configuration={}; saveState();
    }
    if(!QFileInfo(path).isFile()) {setStatus(t("Cannot open debug configuration")+": "+path); return;}
    refreshConfiguration(); emit configurationRequested(path);
}

QJsonObject DebugPanel::formatArguments(QJsonObject arguments) const {
    if(session_.capabilities.value("supportsValueFormattingOptions").toBool())
        arguments["format"]=QJsonObject{{"hex",hexValues_}};
    return arguments;
}

void DebugPanel::editWatch(const QString& expression) {
    const auto workspace=workspace_;
    bool ok=false;
    const auto text=IconDialogs::InputDialog::getText(this,t(expression.isEmpty()?"Add watch":"Edit watch"),
                                        t("Expression"),QLineEdit::Normal,expression,&ok).trimmed();
    if(!ok || text.isEmpty() || workspace!=workspace_) return;
    const int position=state_.watches.indexOf(expression);
    if(!expression.isEmpty() && position<0) return;
    if(text!=expression && state_.watches.contains(text)) {setStatus(t("Expression is already in Watch")); return;}
    if(expression.isEmpty()) state_.watches.append(text); else state_.watches[position]=text;
    saveState(); refreshWatches();
}

bool DebugPanel::canSetValue(const QPersistentModelIndex& index) const {
    if(!index.isValid() || !session_.stopped || session_.ending || session_.frame<0
       || !index.data(HasValueRole).toBool() || index.data(ReadOnlyRole).toBool()) return false;
    return (!index.data(ExpressionRole).toString().isEmpty() && session_.capabilities.value("supportsSetExpression").toBool())
        || (index.data(ParentReferenceRole).toInt()>0 && session_.capabilities.value("supportsSetVariable").toBool());
}

void DebugPanel::setValue(QTreeWidget*,const QPersistentModelIndex& index) {
    if(!canSetValue(index)) return;
    const auto epoch=session_.epoch;
    bool ok=false;
    const auto value=IconDialogs::InputDialog::getText(this,t("Set value"),index.data().toString(),QLineEdit::Normal,
                                         index.sibling(index.row(),1).data().toString(),&ok);
    // The debuggee can continue or the user can change frames while a dialog is open.
    if(!ok || epoch!=session_.epoch || !canSetValue(index)) return;
    const auto expression=index.data(ExpressionRole).toString();
    const bool byExpression=!expression.isEmpty() && session_.capabilities.value("supportsSetExpression").toBool();
    QJsonObject args{{"value",value}};
    if(byExpression) {args["expression"]=expression; args["frameId"]=session_.frame;}
    else {args["variablesReference"]=index.data(ParentReferenceRole).toInt(); args["name"]=index.data().toString();}
    client_->request(byExpression?"setExpression":"setVariable",formatArguments(args),[this,epoch](const QJsonObject& response) {
        if(epoch!=session_.epoch || !session_.stopped || session_.ending) return;
        if(!response.value("success").toBool()) {
            const auto message=t("Cannot set value")+": "+response.value("message").toString();
            setStatus(message); append(message+"\n"); return;
        }
        // Assignment may change other variables or invalidate a container reference.
        scopes(session_.frame);
    });
}

void DebugPanel::valueMenu(QTreeWidget* tree,const QPoint& point) {
    auto* item=point.x()<0?tree->currentItem():tree->itemAt(point);
    if(item) tree->setCurrentItem(item);
    const QPersistentModelIndex index(item?tree->indexFromItem(item):QModelIndex{});
    const bool root=tree==watches_ && item && !item->parent();
    const auto epoch=session_.epoch;
    const auto valid=[this,index,epoch] {return index.isValid() && epoch==session_.epoch;};
    auto* menu=new QMenu(tree); menu->setObjectName("debugValuesMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto action=[&](const char* id,const char* icon,const char* label,bool enabled,auto callback) {
        auto* a=menu->addAction(Codicons::icon(icon),t(label)); a->setObjectName(id); a->setEnabled(enabled);
        connect(a,&QAction::triggered,this,callback); return a;
    };
    if(item) {
        action("debugCopyValue","copy","Copy value",index.data(HasValueRole).toBool(),[index,valid] {
            if(valid()) QApplication::clipboard()->setText(index.sibling(index.row(),1).data().toString());
        });
        action("debugCopyName","symbol-variable","Copy name",true,[index,valid] {if(valid()) QApplication::clipboard()->setText(index.data().toString());});
        const auto expression=index.data(ExpressionRole).toString();
        action("debugCopyExpression","code","Copy expression",!expression.isEmpty(),[expression,valid] {if(valid()) QApplication::clipboard()->setText(expression);});
        menu->addSeparator();
        action("debugSetValue","edit","Set value",canSetValue(index),[this,tree,index,valid] {if(valid()) setValue(tree,index);});
        if(root) {
            action("debugEditWatch","edit","Edit watch",true,[this,expression,valid] {if(valid()) editWatch(expression);});
        }
    }
    if(menu->isEmpty()) {menu->deleteLater(); return;}
    const auto position=point.x()<0?tree->viewport()->rect().center():point;
    menu->popup(tree->viewport()->mapToGlobal(position));
}

void DebugPanel::initContextMenus() {
    for(auto* tree:{variables_,watches_}) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree,&QWidget::customContextMenuRequested,this,[this,tree](const QPoint& point){valueMenu(tree,point);});
        auto* copy=new QShortcut(QKeySequence::Copy,tree); copy->setContext(Qt::WidgetWithChildrenShortcut);
        connect(copy,&QShortcut::activated,this,[tree] {
            auto* item=tree->currentItem(); if(item && item->data(0,HasValueRole).toBool()) QApplication::clipboard()->setText(item->text(1));
        });
    }
    auto* edit=new QShortcut(QKeySequence(Qt::Key_F2),watches_); edit->setContext(Qt::WidgetWithChildrenShortcut);
    connect(edit,&QShortcut::activated,this,[this]{auto* item=watches_->currentItem(); if(item && !item->parent()) editWatch(item->text(0));});
    auto* remove=new QShortcut(QKeySequence(Qt::Key_Delete),watches_); remove->setContext(Qt::WidgetWithChildrenShortcut);
    connect(remove,&QShortcut::activated,this,[this] {
        auto* item=watches_->currentItem(); if(!item || item->parent()) return;
        state_.watches.removeAll(item->text(0)); saveState(); refreshWatches();
    });
    connect(watches_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item) {if(item && !item->parent()) editWatch(item->text(0));});
    for(auto* tree:{frames_,breakpoints_}) {
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tree,&QWidget::customContextMenuRequested,this,[this,tree](const QPoint& point) {
            auto* item=point.x()<0?tree->currentItem():tree->itemAt(point);
            if(item) tree->setCurrentItem(item);
            const QPersistentModelIndex index(item?tree->indexFromItem(item):QModelIndex{});
            const bool bp=tree==breakpoints_;
            const auto workspace=workspace_;
            auto* menu=new QMenu(tree); menu->setObjectName("debugNavigationMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
            auto* go=menu->addAction(Codicons::icon("go-to-file"),t("Go to source")); go->setEnabled(index.isValid());
            connect(go,&QAction::triggered,this,[this,index,bp,workspace] {
                if(!index.isValid() || workspace!=workspace_) return;
                emit sourceActivated(index.data(bp?Qt::UserRole:Qt::UserRole+1).toString(),index.data(bp?Qt::UserRole+1:Qt::UserRole+2).toInt());
            });
            auto* copy=menu->addAction(Codicons::icon("copy"),t(bp?"Copy name":"Copy call stack")); copy->setEnabled(bp?index.isValid():tree->topLevelItemCount()>0);
            connect(copy,&QAction::triggered,this,[tree,index,bp] {
                QStringList text;
                if(bp) {if(index.isValid()) text.append(index.data(Qt::UserRole).toString()+":"+index.data(Qt::UserRole+1).toString());}
                else for(int i=0;i<tree->topLevelItemCount();++i) text.append(tree->topLevelItem(i)->text(0));
                QApplication::clipboard()->setText(text.join('\n'));
            });
            if(bp && index.isValid()) {
                auto* enabled=menu->addAction(Codicons::icon("check"),t("Enabled")); enabled->setCheckable(true); enabled->setChecked(item->checkState(0)==Qt::Checked);
                connect(enabled,&QAction::triggered,this,[tree,index,workspace,this](bool checked) {
                    if(index.isValid() && workspace==workspace_) tree->itemFromIndex(index)->setCheckState(0,checked?Qt::Checked:Qt::Unchecked);
                });
                auto* erase=menu->addAction(Codicons::icon("trash"),t("Remove selected breakpoint"));
                connect(erase,&QAction::triggered,this,[this,index,workspace] {
                    if(index.isValid() && workspace==workspace_) toggleBreakpoint(index.data(Qt::UserRole).toString(),index.data(Qt::UserRole+1).toInt());
                });
            }
            menu->popup(tree->viewport()->mapToGlobal(point.x()<0?tree->viewport()->rect().center():point));
        });
    }
}


QString DebugPanel::valuePath(QTreeWidgetItem* item) const {
    QJsonArray path;
    for(auto* node=item;node;node=node->parent()) {
        int occurrence=0;
        const int count=node->parent()?node->parent()->indexOfChild(node):node->treeWidget()->indexOfTopLevelItem(node);
        for(int i=0;i<count;++i) {
            auto* sibling=node->parent()?node->parent()->child(i):node->treeWidget()->topLevelItem(i);
            if(sibling->text(0)==node->text(0)) ++occurrence;
        }
        path.prepend(occurrence); path.prepend(node->text(0));
    }
    return item->treeWidget()->objectName()+QString::fromUtf8(QJsonDocument(path).toJson(QJsonDocument::Compact));
}

void DebugPanel::restoreValueExpansion(QTreeWidgetItem* item,bool initiallyExpanded) {
    item->setExpanded(valueExpansion_.value(valuePath(item),initiallyExpanded));
}

bool DebugPanel::cortexValueFormat() const {
    return configuration_.type=="dap" && configuration_.adapterID=="cortex-debug";
}

bool DebugPanel::canFormatValues() const {
    return cortexValueFormat() || session_.capabilities.value("supportsValueFormattingOptions").toBool();
}

void DebugPanel::setHexValues(bool hex) {
    if(!session_.stopped || session_.frame<0 || !canFormatValues()) return;
    const bool previous=hexValues_;
    hexValues_=hex; updateFormatButtons();
    const auto generation=++formatRequestGeneration_,epoch=session_.epoch;
    if(!cortexValueFormat()) {
        QSettings().setValue("debug/ui/hexValues",hexValues_); scopes(session_.frame); return;
    }
    // Cortex-Debug 1.12 uses its custom request rather than DAP ValueFormat.
    // It may also emit a new stopped event after changing GDB's output radix.
    client_->request("set-var-format",{{"hex",hex}},[this,generation,epoch,hex,previous](const QJsonObject& response) {
        if(generation!=formatRequestGeneration_) return;
        if(!response.value("success").toBool()) {
            hexValues_=previous; updateFormatButtons();
            setStatus(t("Cannot change value format")+": "+response.value("message").toString()); return;
        }
        QSettings().setValue("debug/ui/hexValues",hex);
        if(epoch==session_.epoch && session_.stopped && session_.frame>=0) scopes(session_.frame);
    });
}


void DebugPanel::updateFormatButtons() {
    for(auto* button:formatButtons_) {
        button->setChecked(hexValues_);
        button->setEnabled(session_.stopped && !session_.ending && session_.frame>=0 && canFormatValues());
        button->setIcon(Codicons::icon("symbol-number",ThemeManager::instance().color(hexValues_?"accent":"text.muted")));
    }
}
