#include "TerminalPanel.hpp"
#include "TerminalSession.hpp"
#include "TerminalWidget.hpp"
#include "app/I18n.hpp"
#include "app/Codicons.hpp"
#include <QHBoxLayout>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <QSignalBlocker>
#include <QJsonArray>
#include <QDir>
#include <QLabel>
#include <QFileInfo>
#include <algorithm>
#include <utility>

TerminalPanel::TerminalPanel(QWidget* parent) : QDockWidget(I18n::text("Terminal"), parent) {
    setObjectName("terminalPanel");
    setAllowedAreas(Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable);
    setMinimumHeight(100);
    auto* header = new QWidget(this);
    header->setObjectName("terminalActions");
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(6, 0, 6, 0); layout->setSpacing(2);
    shellIcon_ = new QLabel(header);
    shellIcon_->setPixmap(Codicons::icon("terminal").pixmap(16, 16));
    layout->addWidget(shellIcon_);
    shellName_ = new QLabel(header);
    shellName_->setObjectName("terminalShellName");
    layout->addWidget(shellName_);
    layout->addSpacing(6);
    const auto addButton = [&](const char* name, const char* icon, const char* label) {
        auto* button = new QToolButton(header);
        button->setObjectName(name);
        button->setIcon(Codicons::icon(icon));
        button->setToolTip(I18n::text(label));
        button->setAccessibleName(button->toolTip());
        button->setAutoRaise(true);
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(28, 28);
        layout->addWidget(button);
        return button;
    };
    restart_ = addButton("terminalRestart", "refresh", "Restart terminal");
    closeSession_ = addButton("terminalCloseSession", "trash", "Close terminal session");
    auto* hidePanel = addButton("terminalClose", "close", "Hide terminal");
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("terminalTabs");
    auto* tabBar = tabs_->tabBar();
    tabBar->setAccessibleName(I18n::text("Terminal"));
    auto tabFont = tabBar->font();
    tabFont.setPointSizeF(10);
    tabBar->setFont(tabFont);
    tabBar->setDrawBase(false);
    tabBar->setExpanding(false);
    tabBar->setElideMode(Qt::ElideRight);
    tabBar->setUsesScrollButtons(true);
    tabs_->setCornerWidget(header, Qt::TopRightCorner);

    debugTerminal_ = new TerminalWidget(tabs_);
    debugTerminal_->setObjectName("debugProgramTerminal");
    debugSession_ = new TerminalSession(this);
    const int outputIndex = tabs_->addTab(debugTerminal_, I18n::text("OUTPUT"));
    tabs_->setTabToolTip(outputIndex, I18n::text("Program input/output"));
    terminal_ = addShell(I18n::text("TERMINAL"), {});
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
    // The tab strip is the dock header.
    auto* titleSpacer = new QWidget(this);
    titleSpacer->setFixedHeight(0);
    setTitleBarWidget(titleSpacer);
    setWidget(tabs_);
    connect(hidePanel, &QToolButton::clicked, this, &QWidget::hide);
    connect(restart_, &QToolButton::clicked, this, &TerminalPanel::restartSession);
    connect(closeSession_, &QToolButton::clicked, this, [this] {
        closeShell(qobject_cast<TerminalWidget*>(tabs_->currentWidget()));
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        auto* page = tabs_->widget(index);
        selectPage(page);
        if (page && page == terminal_ && isVisible()) startSession(terminal_);
    });
    selectPage(terminal_);
    hide();
}

TerminalPanel::~TerminalPanel() {
    // A session can emit finished() during QDockWidget's child teardown.
    disconnect(tabs_, nullptr, this, nullptr);
    for (auto* session : findChildren<TerminalSession*>(QString(), Qt::FindDirectChildrenOnly))
        disconnect(session, nullptr, this, nullptr);
}

TerminalWidget* TerminalPanel::addShell(const QString& title, const QString& directory) {
    auto* terminal = new TerminalWidget(tabs_);
    auto* session = new TerminalSession(this);
    shells_.insert(terminal, {session, directory});
    const int index = tabs_->addTab(terminal, title);
    tabs_->setTabToolTip(index, QDir::toNativeSeparators(directory));

    connect(terminal, &TerminalWidget::input, session, &TerminalSession::write);
    connect(terminal, &TerminalWidget::terminalSizeChanged, session, &TerminalSession::resize);
    connect(session, &TerminalSession::output, terminal, &TerminalWidget::feedOutput);
    connect(session, &TerminalSession::started, this, &TerminalPanel::updateShellActions);
    connect(session, &TerminalSession::error, terminal, [terminal](const QString& message) {
        terminal->feedOutput(("\r\n" + I18n::text("Terminal error: %1").arg(message) + "\r\n").toUtf8());
    });
    connect(session, &TerminalSession::finished, this, [this, terminal](int code) {
        auto shell = shells_.find(terminal);
        if (shuttingDown_ || shell == shells_.end()) return;
        if (shell->restartPending) {
            // Starting again must wait until QProcess has delivered finished().
            QTimer::singleShot(0, terminal, [this, terminal] { startSession(terminal); });
        } else if (code == 0) {
            closeShell(terminal);
        } else {
            terminal->feedOutput(("\r\n" + I18n::text("Process exited (%1). Use Restart terminal to start a new session.").arg(code) + "\r\n").toUtf8());
        }
    });
    return terminal;
}

TerminalSession* TerminalPanel::session() const {
    const auto shell = shells_.constFind(terminal_);
    return shell == shells_.cend() ? nullptr : shell->session;
}

void TerminalPanel::selectPage(QWidget* page) {
    if (!page) return;
    const QSignalBlocker block(tabs_);
    tabs_->setCurrentWidget(page);
    setFocusProxy(page);
    updateShellActions();
}

void TerminalPanel::updateShellActions() {
    const auto shell = shells_.constFind(tabs_->currentWidget());
    const bool hasShell = shell != shells_.cend();
    restart_->setEnabled(hasShell);
    closeSession_->setEnabled(hasShell);
    shellIcon_->setVisible(hasShell);
    shellName_->setVisible(hasShell);
    if (hasShell) shellName_->setText(QFileInfo(shell->session->program()).fileName());
}

void TerminalPanel::open(const QString& directory) {
    if (shells_.isEmpty()) terminal_ = addShell(I18n::text("TERMINAL"), directory);
    if (terminal_) shells_[terminal_].directory = directory;
    auto* selected = tabs_->currentWidget();
    if (!shells_.contains(selected))
        selected = terminal_ ? terminal_ : tabs_->widget(tabs_->count() - shells_.size());
    auto* terminal = static_cast<TerminalWidget*>(selected);
    showShell(terminal);
    if (terminal == terminal_) startSession(terminal);
}

void TerminalPanel::openNewSession(const QString& directory) {
    const QDir folder(directory);
    const auto path = folder.absolutePath();
    auto title = folder.dirName();
    if (title.isEmpty()) title = path;
    title.replace('&', QStringLiteral("&&"));
    auto* terminal = addShell(title, path);
    showShell(terminal);
    startSession(terminal);
}

void TerminalPanel::showShell(TerminalWidget* terminal) {
    selectPage(terminal);
    show();
    raise();
    terminal->applyFontSettings();
    focusCurrentTab();
}

void TerminalPanel::focusCurrentTab() {
    if (auto* current = tabs_->currentWidget()) current->setFocus(Qt::ShortcutFocusReason);
}

void TerminalPanel::setDirectory(const QString& directory) {
    if (!terminal_) return;
    const auto normalized = directory.isEmpty() ? QDir::homePath() : QDir(directory).absolutePath();
    auto& shell = shells_[terminal_];
    if (shell.directory == normalized) return;
    shell.directory = normalized;
    // Replace the shell instead of typing a command into a possibly running program.
    // Hidden terminals follow the project without opening the panel or taking focus.
    if (shell.session->isRunning()) {
        shell.restartPending = true;
        shell.session->stop();
    }
}

void TerminalPanel::startSession(TerminalWidget* terminal) {
    auto shell = shells_.find(terminal);
    if (shuttingDown_ || shell == shells_.end() || shell->session->isRunning()) return;
    shell->restartPending = false;
    terminal->reset();
    shell->session->resize(terminal->rows(), terminal->columns());
    shell->session->start(shell->directory);
}

void TerminalPanel::restartSession() {
    auto* terminal = qobject_cast<TerminalWidget*>(tabs_->currentWidget());
    auto shell = shells_.find(terminal);
    if (shuttingDown_ || shell == shells_.end()) return;
    terminal->setFocus(Qt::OtherFocusReason);
    if (shell->session->isRunning()) {
        shell->restartPending = true;
        shell->session->stop();
    } else {
        startSession(terminal);
    }
}

void TerminalPanel::closeShell(TerminalWidget* terminal) {
    auto shell = shells_.find(terminal);
    if (shell == shells_.end()) return;

    auto* session = shell->session;
    const bool selected = tabs_->currentWidget() == terminal;
    const int row = tabs_->indexOf(terminal);
    if (terminal == terminal_) terminal_ = nullptr;
    shells_.erase(shell);
    {
        const QSignalBlocker block(tabs_);
        tabs_->removeTab(row);
    }
    session->stop();
    session->deleteLater();
    terminal->deleteLater();
    if (selected && shells_.isEmpty()) hide();
    auto* next = tabs_->currentWidget();
    if (selected && !shells_.isEmpty()) {
        const int firstShell = tabs_->count() - shells_.size();
        next = tabs_->widget(std::clamp(row - 1, firstShell, tabs_->count() - 1));
    }
    selectPage(next);
    if (selected && isVisible()) {
        if (next == terminal_) startSession(terminal_);
        focusCurrentTab();
    }
}

void TerminalPanel::shutdown() {
    shuttingDown_ = true;
    stopDebugProgram();
    for (const auto& shell : std::as_const(shells_)) shell.session->stop();
}

void TerminalPanel::setDebugConsole(QWidget* console) {
    if (debugConsole_ == console) return;
    debugConsole_ = console;
    tabs_->insertTab(1, console, I18n::text("DEBUG CONSOLE"));
}
void TerminalPanel::showDebugConsole() {
    if(!debugConsole_) return;
    selectPage(debugConsole_);
    show(); raise();
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
    selectPage(debugTerminal_);
    show(); raise(); debugTerminal_->setFocus();
    debugSession_->resize(debugTerminal_->rows(),debugTerminal_->columns()); pendingRequest_=request;
    debugSession_->start(args.value("cwd").toString(),program,arguments,args.value("env").toObject());
}
void TerminalPanel::stopDebugProgram() { pendingRequest_={}; debugSession_->stop(); }
