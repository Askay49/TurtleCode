#include "TerminalPanel.hpp"
#include "TerminalSession.hpp"
#include "TerminalWidget.hpp"
#include "app/I18n.hpp"
#include "app/Codicons.hpp"
#include <QHBoxLayout>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QTabWidget>
#include <QJsonArray>
#include <QDir>
#include <QLabel>
#include <QFileInfo>

TerminalPanel::TerminalPanel(QWidget* parent) : QDockWidget(I18n::text("Terminal"), parent),
    terminal_(new TerminalWidget(this)), session_(new TerminalSession(this)) {
    setObjectName("terminalPanel");
    setAllowedAreas(Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable);
    setMinimumHeight(100);
    auto* header = new QWidget(this);
    header->setObjectName("terminalActions");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(6, 0, 6, 0); layout->setSpacing(2);
    auto* shellIcon = new QLabel(header);
    shellIcon->setPixmap(Codicons::icon("terminal").pixmap(16, 16));
    layout->addWidget(shellIcon);
    auto* shellName = new QLabel(header); shellName->setObjectName("terminalShellName");
    layout->addWidget(shellName); layout->addSpacing(6);
    connect(session_, &TerminalSession::started, this, [this, shellName] {
        shellName->setText(QFileInfo(session_->program()).fileName());
    });
    auto* restart = new QToolButton(header); restart->setObjectName("terminalRestart");
    restart->setIcon(Codicons::icon("refresh")); restart->setToolTip(I18n::text("Restart terminal"));
    restart->setAccessibleName(restart->toolTip()); layout->addWidget(restart);
    auto* close = new QToolButton(header); close->setObjectName("terminalClose");
    close->setIcon(Codicons::icon("close")); close->setToolTip(I18n::text("Hide terminal"));
    close->setAccessibleName(close->toolTip()); layout->addWidget(close);
    for (auto* button : {restart, close}) {
        button->setAutoRaise(true);
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(28, 28);
    }
    tabs_=new QTabWidget(this); tabs_->setObjectName("terminalTabs");
    auto tabFont = tabs_->tabBar()->font();
    tabFont.setPointSizeF(10);
    tabs_->tabBar()->setFont(tabFont);
    tabs_->tabBar()->setDrawBase(false);
    tabs_->tabBar()->setExpanding(false);
    tabs_->tabBar()->setElideMode(Qt::ElideRight);
    tabs_->setCornerWidget(header, Qt::TopRightCorner);
    tabs_->addTab(terminal_,I18n::text("TERMINAL"));
    debugTerminal_=new TerminalWidget(this); debugTerminal_->setObjectName("debugProgramTerminal"); debugSession_=new TerminalSession(this);
    tabs_->insertTab(0,debugTerminal_,I18n::text("OUTPUT"));
    tabs_->setTabToolTip(0, I18n::text("Program input/output"));
    tabs_->setCurrentWidget(terminal_);
    connect(debugTerminal_,&TerminalWidget::input,debugSession_,&TerminalSession::write);
    connect(debugTerminal_,&TerminalWidget::terminalSizeChanged,debugSession_,&TerminalSession::resize);
    connect(debugSession_,&TerminalSession::output,debugTerminal_,&TerminalWidget::feedOutput);
    connect(debugSession_,&TerminalSession::started,this,[this]{
        if(pendingRequest_.isEmpty()) return;
        const auto request=pendingRequest_; pendingRequest_={};
        emit debugTerminalReply(request,true,{{"processId",debugSession_->processId()}},{});
    });
    connect(debugSession_,&TerminalSession::error,this,[this](const QString& message){
        if(!pendingRequest_.isEmpty()) {const auto request=pendingRequest_; pendingRequest_={}; emit debugTerminalReply(request,false,{},message);}
        debugTerminal_->feedOutput(("\r\n"+message+"\r\n").toUtf8());
    });
    // The tab strip is the single panel header; keep dock ownership and shortcuts.
    auto* titleSpacer = new QWidget(this);
    titleSpacer->setFixedHeight(0);
    setTitleBarWidget(titleSpacer); setWidget(tabs_); setFocusProxy(terminal_);
    connect(close, &QToolButton::clicked, this, &QWidget::hide);
    connect(restart, &QToolButton::clicked, this, [this] {
        terminal_->setFocus(Qt::OtherFocusReason);
        if (session_->isRunning()) { restartPending_ = true; session_->stop(); }
        else startSession();
    });
    connect(terminal_, &TerminalWidget::input, session_, &TerminalSession::write);
    connect(terminal_, &TerminalWidget::terminalSizeChanged, session_, &TerminalSession::resize);
    connect(session_, &TerminalSession::output, terminal_, &TerminalWidget::feedOutput);
    connect(session_, &TerminalSession::error, this, [this](const QString& message) {
        terminal_->feedOutput(("\r\n" + I18n::text("Terminal error: %1").arg(message) + "\r\n").toUtf8());
    });
    connect(session_, &TerminalSession::finished, this, [this](int code) {
        if (shuttingDown_) return;
        if (restartPending_) {
            restartPending_ = false; QTimer::singleShot(0, this, &TerminalPanel::startSession);
        } else terminal_->feedOutput(("\r\n" + I18n::text("Process exited (%1). Use Restart terminal to start a new session.").arg(code) + "\r\n").toUtf8());
    });
    connect(tabs_,&QTabWidget::currentChanged,this,[this,restart,shellIcon,shellName]{
        const bool shell=tabs_->currentWidget()==terminal_; restart->setEnabled(shell);
        shellIcon->setVisible(shell); shellName->setVisible(shell);
        setFocusProxy(tabs_->currentWidget());
        if(shell && isVisible() && !session_->isRunning()) startSession();
    });
    hide();
}
void TerminalPanel::open(const QString& directory) {
    directory_ = directory;
    show(); raise();
    tabs_->setCurrentWidget(terminal_);
    terminal_->applyFontSettings();
    if (!session_->isRunning()) startSession();
    terminal_->setFocus(Qt::ShortcutFocusReason);
}
void TerminalPanel::setDirectory(const QString& directory) {
    const auto normalized = directory.isEmpty() ? QDir::homePath() : QDir(directory).absolutePath();
    if (directory_ == normalized) return;
    directory_ = normalized;
    // Replace the shell instead of typing a command into a possibly running program.
    // Hidden terminals follow the project without opening the panel or taking focus.
    if (session_->isRunning()) {
        restartPending_ = true;
        session_->stop();
    }
}
void TerminalPanel::startSession() {
    if (shuttingDown_ || session_->isRunning()) return;
    restartPending_ = false;
    terminal_->reset();
    session_->resize(terminal_->rows(), terminal_->columns());
    session_->start(directory_);
}
void TerminalPanel::shutdown() { stopDebugProgram(); shuttingDown_ = true; restartPending_ = false; session_->stop(); }

void TerminalPanel::setDebugConsole(QWidget* console) {
    debugConsole_=console; tabs_->insertTab(1,console,I18n::text("DEBUG CONSOLE")); console->show();
}
void TerminalPanel::showDebugConsole() {
    if(!debugConsole_) return;
    show(); raise(); tabs_->setCurrentWidget(debugConsole_);
}
void TerminalPanel::runDebugProgram(const QJsonObject& request) {
    const auto args=request.value("arguments").toObject();
    const auto command=args.value("args").toArray();
    if(debugSession_->isRunning() || !pendingRequest_.isEmpty() || command.isEmpty() || args.value("argsCanBeInterpretedByShell").toBool()) {
        emit debugTerminalReply(request,false,{},I18n::text("Cannot start debug program terminal")); return;
    }
    QStringList arguments;
    for(const auto& value:command) {if(!value.isString()) {emit debugTerminalReply(request,false,{},"Invalid terminal arguments"); return;} arguments.append(value.toString());}
    const auto program=arguments.takeFirst();
    if(program.isEmpty()) {emit debugTerminalReply(request,false,{},"Empty terminal command"); return;}
    debugTerminal_->reset(); debugTerminal_->applyFontSettings();
    show(); raise(); tabs_->setCurrentWidget(debugTerminal_); debugTerminal_->setFocus();
    debugSession_->resize(debugTerminal_->rows(),debugTerminal_->columns()); pendingRequest_=request;
    debugSession_->start(args.value("cwd").toString(),program,arguments,args.value("env").toObject());
}
void TerminalPanel::stopDebugProgram() { pendingRequest_={}; debugSession_->stop(); }
