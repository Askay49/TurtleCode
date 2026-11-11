#include "GitPanel.hpp"
#include "app/IconDialogs.hpp"
#include "app/Codicons.hpp"
#include "app/FileIcons.hpp"
#include "app/FontSettings.hpp"
#include "app/I18n.hpp"
#include "app/ThemeManager.hpp"
#include <QApplication>
#include <QComboBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QMouseEvent>
#include <QAbstractItemView>

namespace {
constexpr int EntryRole = Qt::UserRole, StagedRole = Qt::UserRole + 1, GroupRole = Qt::UserRole + 2;
QToolButton* tool(QWidget* parent, const char* icon, const QString& tooltip) {
    auto* button = new QToolButton(parent);
    button->setIcon(Codicons::icon(icon)); button->setToolTip(tooltip);
    button->setAccessibleName(tooltip); button->setAutoRaise(true);
    button->setIconSize(QSize(14,14)); button->setFixedSize(22,22);
    return button;
}
class BranchCombo final : public QComboBox {
public:
    explicit BranchCombo(QWidget* parent) : QComboBox(parent) {
        setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override {
        return {qBound(64,fontMetrics().horizontalAdvance(currentText())+28,140),qMax(22,fontMetrics().height()+6)};
    }
    QSize minimumSizeHint() const override { return {48,sizeHint().height()}; }
    void showPopup() override {
        int longest=180;
        for(int i=0;i<count();++i) longest=qMax(longest,fontMetrics().horizontalAdvance(itemText(i))+40);
        view()->setMinimumWidth(qMin(longest,600));
        QComboBox::showPopup();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if(underMouse() || hasFocus()) {
            painter.setPen(Qt::NoPen); painter.setBrush(ThemeManager::instance().color("editor.selection"));
            painter.drawRoundedRect(rect().adjusted(0,1,0,-1),3,3);
        }
        const auto color=palette().color(isEnabled()?QPalette::Active:QPalette::Disabled,QPalette::WindowText);
        Codicons::icon("git-branch",color).paint(&painter,QRect(4,(height()-14)/2,14,14));
        painter.setPen(color); painter.setFont(font());
        const auto textRect=rect().adjusted(23,0,-4,0);
        painter.drawText(textRect,Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(currentText(),Qt::ElideMiddle,textRect.width()));
    }
};
class RepositoryLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setFont(font()); painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(),Qt::AlignLeft|Qt::AlignVCenter,fontMetrics().elidedText(text(),Qt::ElideRight,contentsRect().width()));
    }
};
class FileTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    QPersistentModelIndex hovered;
protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto index = indexAt(event->pos()).siblingAtColumn(0);
        if (index != hovered) { hovered = index; viewport()->update(); }
        QTreeWidget::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent* event) override { hovered = QPersistentModelIndex(); viewport()->update(); QTreeWidget::leaveEvent(event); }
};
class FileDelegate final : public QStyledItemDelegate {
public:
    explicit FileDelegate(FileTree* tree) : QStyledItemDelegate(tree), tree_(tree) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem view(option); initStyleOption(&view,index);
        const auto name = index.siblingAtColumn(0);
        const bool file = name.data(EntryRole).isValid();
        const bool hover = tree_->hovered == name || (option.state & QStyle::State_Selected);
        if ((index.column() == 1 || index.column() == 2) && file && !hover) view.icon = {};
        const QString path = view.text;
        if (file && index.column()==0) view.text = QFileInfo(path).fileName();
        if (!file) view.font.setBold(true);
        tree_->style()->drawControl(QStyle::CE_ItemViewItem,&view,painter,tree_);
        if (file && index.column()==0) {
            auto rect=tree_->style()->subElementRect(QStyle::SE_ItemViewItemText,&view,tree_);
            const int width=QFontMetrics(view.font).horizontalAdvance(view.text);
            const QString directory=QFileInfo(path).path();
            if (directory!="." && width+20<rect.width()) {
                rect.setLeft(rect.left()+width+8);
                painter->save(); painter->setClipRect(rect); painter->setFont(view.font);
                painter->setPen(ThemeManager::instance().color("text.muted"));
                painter->drawText(rect,Qt::AlignVCenter|Qt::AlignLeft,QFontMetrics(view.font).elidedText(directory,Qt::ElideLeft,rect.width()));
                painter->restore();
            }
        }
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size=QStyledItemDelegate::sizeHint(option,index);
        size.setHeight(qMax(22,option.fontMetrics.height()+4)); return size;
    }
private:
    FileTree* tree_;
};
}
GitPanel::GitPanel(QWidget* parent) : QWidget(parent), service_(this) {
    setObjectName("gitPanel");
    auto* layout=new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    auto* heading=new QWidget(this); heading->setObjectName("gitHeading");
    auto* header=new QHBoxLayout(heading); header->setContentsMargins(12,5,6,5); header->setSpacing(2);
    auto* title=new QLabel(I18n::text("SOURCE CONTROL"),heading); title->setObjectName("gitHeadingTitle");
    title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); header->addWidget(title,1);
    auto* collapse=tool(heading,"collapse-all",I18n::text("Collapse all")); header->addWidget(collapse);
    refresh_=tool(heading,"refresh",I18n::text("Refresh Git")); refresh_->setObjectName("gitRefresh"); header->addWidget(refresh_);
    auto* more=tool(heading,"ellipsis",I18n::text("More actions")); header->addWidget(more); layout->addWidget(heading);
    auto* scroll=new QScrollArea(this); scroll->setObjectName("gitRepositoriesScroll"); scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content=new QWidget(scroll); repositoriesLayout_=new QVBoxLayout(content);
    repositoriesLayout_->setContentsMargins(0,0,0,0); repositoriesLayout_->setSpacing(0);
    empty_=new QLabel(I18n::text("No Git repository in workspace."),content); empty_->setWordWrap(true);
    empty_->setContentsMargins(12,8,12,8); repositoriesLayout_->addWidget(empty_); repositoriesLayout_->addStretch();
    scroll->setWidget(content); layout->addWidget(scroll,1);
    connect(collapse,&QToolButton::clicked,this,[this] {
        for(auto* section:sections_) { section->heading->setChecked(false); section->body->hide(); section->heading->setIcon(Codicons::icon("chevron-right")); }
    });
    connect(refresh_,&QToolButton::clicked,&service_,&GitService::rescan);
    connect(more,&QToolButton::clicked,this,[this,more] {
        auto* menu=new QMenu(more); menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* refresh=menu->addAction(Codicons::icon("refresh"),I18n::text("Refresh Git"));
        connect(refresh,&QAction::triggered,&service_,&GitService::rescan);
        menu->popup(more->mapToGlobal(QPoint(0,more->height())));
    });
    connect(&service_,&GitService::changed,this,&GitPanel::rebuild);
    connect(&service_,&GitService::busyChanged,this,[this](bool busy) {
        updateActions();
        if(busy) return;
        if(pendingPull_) {
            pendingPull_=false; const auto repository=pendingStageRepository_;
            QTimer::singleShot(0,this,[this,repository] { pullRepository(repository); }); return;
        }
        if(!pendingBranch_.isEmpty()) {
            const auto branch=pendingBranch_, repository=pendingStageRepository_; pendingBranch_.clear();
            QTimer::singleShot(0,this,[this,repository,branch] { switchBranch(repository,branch); }); return;
        }
        if(!pendingStage_.isEmpty()) {
            const auto paths=pendingStage_; const auto repository=pendingStageRepository_; pendingStage_.clear();
            QTimer::singleShot(0,this,[this,repository,paths] { stagePaths(repository,paths); }); return;
        }
        for(auto* section:sections_) if(section->commitAfterStage) {
            const auto repository=section->repository;
            QTimer::singleShot(0,this,[this,repository] {
                auto* section=sections_.value(repository);
                if(section && section->commitAfterStage && !service_.busy() && select(section)) {
                    section->commitAfterStage=false;
                    requestCommit(section);
                }
            });
        }
    });
    connect(&service_,&GitService::error,this,[this](const QString& message) {
        if(auto* section=sections_.value(service_.repository())) {
            section->commitAfterStage=false;
            section->error->setText(I18n::text("Git error: %1").arg(message.left(4000))); section->error->show();
        }
    });
    connect(&service_,&GitService::commitFailed,this,[this](const QString& message) {
        auto* dialog=new QDialog(window()); dialog->setObjectName("gitCommitErrorDialog");
        dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setWindowTitle(I18n::text("Commit failed"));
        dialog->setWindowModality(Qt::WindowModal); dialog->resize(720,420);
        auto* layout=new QVBoxLayout(dialog); auto* text=new QPlainTextEdit(dialog);
        text->setObjectName("gitCommitErrorText"); text->setReadOnly(true); text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setFont(FontSettings::font(FontSettings::Area::Editor)); text->setAccessibleName(I18n::text("Commit error details"));
        text->setPlainText(message); layout->addWidget(text,1);
        auto* buttons=new IconDialogs::ButtonBox(QDialogButtonBox::Close,dialog);
        auto* copy=buttons->addButton(I18n::text("Copy"),QDialogButtonBox::ActionRole); copy->setIcon(Codicons::icon("copy")); copy->setObjectName("gitCommitErrorCopy");
        connect(copy,&QPushButton::clicked,dialog,[message] { QApplication::clipboard()->setText(message); });
        connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::reject); layout->addWidget(buttons); dialog->open();
    });
    connect(&service_,&GitService::committed,this,[this] {
        if(auto* section=sections_.value(service_.repository())) { section->message->clear(); section->error->hide(); }
    });
    connect(&service_,&GitService::diffReady,this,[this](const QString& path,bool staged,const QString& text) {
        emit diffReady(service_.repository()+"/"+path,staged,text);
    });
    connect(&ThemeManager::instance(),&ThemeManager::themeChanged,this,[this] {
        for(auto* section:sections_) section->initialized=false;
        rebuild();
    });
    poll_=new QTimer(this); poll_->setInterval(2500);
    connect(poll_,&QTimer::timeout,this,[this] { if(!preparing_ && isVisible() && QApplication::applicationState()==Qt::ApplicationActive) refresh(); }); poll_->start();
    connect(qApp,&QGuiApplication::applicationStateChanged,this,[this](Qt::ApplicationState state) { if(state==Qt::ApplicationActive) refresh(); });
    applyFontSettings(); rebuild();
}
GitPanel::~GitPanel() { qDeleteAll(sections_); }
void GitPanel::pullRepository(const QString& repository) {
    if(repository!=service_.repository()) { cancelPreparation(); emit service_.branchSwitchFinished(repository,false); return; }
    if(service_.busy()) { pendingStageRepository_=repository; pendingPull_=true; preparing_=true; updateActions(); return; }
    cancelPreparation(); service_.pull();
}

GitPanel::Section* GitPanel::addRepository(const QString& repository) {
    auto* section=new Section; section->repository=repository;
    auto* root=new QWidget; root->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum); root->setObjectName("gitRepository"); root->setProperty("repository",repository); section->root=root;
    auto* layout=new QVBoxLayout(root); layout->setContentsMargins(0,0,0,0); layout->setSpacing(0); layout->setAlignment(Qt::AlignTop);
    auto* header=new QWidget(root); header->setObjectName("gitRepositoryHeader"); auto* row=new QHBoxLayout(header);
    row->setContentsMargins(6,2,6,2); row->setSpacing(2);
    section->heading=tool(header,"chevron-down",repository); section->heading->setFixedWidth(18);
    section->heading->setCheckable(true); section->heading->setChecked(true); row->addWidget(section->heading);
    auto* name=new RepositoryLabel(QFileInfo(repository).fileName(),header); name->setObjectName("gitRepositoryName");
    name->setToolTip(repository); name->setMinimumWidth(24); name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); row->addWidget(name,1);
    section->branches=new BranchCombo(header); section->branches->setObjectName("gitBranches");
    section->branches->setMinimumWidth(48); section->branches->setMaximumWidth(140);
    section->branches->setAccessibleName(I18n::text("Branch")); row->addWidget(section->branches);
    auto* quickCommit=tool(header,"check",I18n::text("Commit staged changes")); row->addWidget(quickCommit);
    section->refresh=tool(header,"refresh",I18n::text("Refresh Git")); row->addWidget(section->refresh);
    section->more=tool(header,"ellipsis",I18n::text("More actions")); row->addWidget(section->more); layout->addWidget(header);
    section->body=new QWidget(root); section->body->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum); auto* body=new QVBoxLayout(section->body); body->setAlignment(Qt::AlignTop); body->setContentsMargins(20,4,8,6); body->setSpacing(6);
    section->message=new QPlainTextEdit(section->body); section->message->setObjectName("gitCommitMessage");
    section->message->setPlaceholderText(I18n::text("Message (Ctrl+Enter to commit)")); section->message->setAccessibleName(I18n::text("Commit message"));
    section->message->setTabChangesFocus(true); body->addWidget(section->message);
    auto* commitRow=new QWidget(section->body); commitRow->setObjectName("gitCommitRow"); auto* commitLayout=new QHBoxLayout(commitRow);
    commitLayout->setContentsMargins(0,0,0,0); commitLayout->setSpacing(0);
    section->commit=new QPushButton(I18n::text("Commit"),commitRow); section->commit->setIcon(Codicons::icon("check")); section->commit->setObjectName("gitCommit"); commitLayout->addWidget(section->commit,1);
    section->commitMore=tool(commitRow,"chevron-down",I18n::text("Commit actions")); section->commitMore->setObjectName("gitCommitMore"); section->commitMore->setFixedHeight(28); commitLayout->addWidget(section->commitMore);
    body->addWidget(commitRow);
    section->state=new QLabel(section->body); section->state->setWordWrap(true); section->state->setTextFormat(Qt::PlainText); body->addWidget(section->state);
    section->error=new QLabel(section->body); section->error->setObjectName("gitError"); section->error->setWordWrap(true); section->error->setTextFormat(Qt::PlainText);
    section->error->setTextInteractionFlags(Qt::TextSelectableByMouse); section->error->hide(); body->addWidget(section->error);
    auto* files=new FileTree(section->body); section->files=files; files->setObjectName("gitFiles"); files->setColumnCount(4); files->setHeaderHidden(true);
    files->setItemDelegate(new FileDelegate(files)); files->setUniformRowHeights(true); files->setIndentation(12); files->setMouseTracking(true);
    files->setIconSize(QSize(16,16)); files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    files->header()->setStretchLastSection(false); files->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    for(int column=1;column<4;++column) { files->header()->setSectionResizeMode(column,QHeaderView::Fixed); files->setColumnWidth(column,column==3?26:22); }
    body->addWidget(files); layout->addWidget(section->body);
    repositoriesLayout_->insertWidget(repositoriesLayout_->count()-1,root);
    sections_.insert(repository,section);
    connect(section->heading,&QToolButton::toggled,this,[section](bool expanded) {
        section->body->setVisible(expanded); section->heading->setIcon(Codicons::icon(expanded?"chevron-down":"chevron-right"));
    });
    connect(section->message,&QPlainTextEdit::textChanged,this,&GitPanel::updateActions);
    connect(section->commit,&QPushButton::clicked,this,[this,section] { requestCommit(section); });
    connect(quickCommit,&QToolButton::clicked,this,[this,section] { requestCommit(section); });
    auto* shortcut=new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_Return),section->message); shortcut->setContext(Qt::WidgetShortcut);
    connect(shortcut,&QShortcut::activated,this,[this,section] { requestCommit(section); });
    auto* keypadShortcut=new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_Enter),section->message); keypadShortcut->setContext(Qt::WidgetShortcut);
    connect(keypadShortcut,&QShortcut::activated,this,[this,section] { requestCommit(section); });
    connect(section->commitMore,&QToolButton::clicked,this,[this,section] {
        auto* menu=new QMenu(section->commitMore); menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* staged=menu->addAction(Codicons::icon("check"),I18n::text("Commit staged changes"));
        connect(staged,&QAction::triggered,this,[this,section] { requestCommit(section); });
        auto* all=menu->addAction(Codicons::icon("check-all"),I18n::text("Stage all and commit"));
        connect(all,&QAction::triggered,this,[this,section] { requestCommit(section,true); });
        menu->popup(section->commitMore->mapToGlobal(QPoint(0,section->commitMore->height())));
    });
    connect(section->refresh,&QToolButton::clicked,this,[this] { refresh(); });
    connect(section->more,&QToolButton::clicked,this,[this,section] { showRepositoryMenu(section,section->more); });
    connect(section->branches,qOverload<int>(&QComboBox::activated),this,[this,section](int index) {
        const auto branch=section->branches->itemData(index).toString();
        section->branches->setCurrentIndex(section->branches->findData(service_.status(section->repository).currentBranch));
        if(!select(section) || branch.isEmpty() || branch==service_.status().currentBranch) return;
        ++operationGeneration_; preparing_=true; updateActions(); section->error->hide(); emit branchSwitchRequested(section->repository,branch);
    });
    files->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(files,&QTreeWidget::customContextMenuRequested,this,[this,section](const QPoint& point) { showFileMenu(section,point); });
    connect(files,&QTreeWidget::itemClicked,this,[this,section](QTreeWidgetItem* item,int column) { activate(section,item,column); });
    connect(files,&QTreeWidget::itemActivated,this,[this,section](QTreeWidgetItem* item,int) { activate(section,item,0); });
    return section;
}
bool GitPanel::select(Section* section) {
    return section && !preparing_ && !service_.writing() && service_.selectRepository(section->repository);
}
void GitPanel::applyFontSettings() {
    const auto size=FontSettings::font(FontSettings::Area::Tree).pointSizeF();
    auto widgets=findChildren<QWidget*>(); widgets.prepend(this);
    for(auto* widget:widgets) { auto font=widget->font(); font.setPointSizeF(size); widget->setFont(font); }
    for(auto* section:sections_) {
        section->message->setFixedHeight(qMax(32,section->message->fontMetrics().height()*2+10));
    }
}
void GitPanel::setRoots(const QStringList& roots) {
    if(roots_==roots) { refresh(); return; }
    ++operationGeneration_; roots_=roots; pendingStage_.clear(); pendingBranch_.clear(); pendingPull_=false; preparing_=false;
    for(auto* section:sections_) section->commitAfterStage=false;
    service_.setRoots(roots);
}
void GitPanel::refresh() { service_.refresh(); }
void GitPanel::cancelPreparation() {
    ++operationGeneration_; preparing_=false;
    if(auto* section=sections_.value(service_.repository())) section->commitAfterStage=false;
    updateActions();
}
void GitPanel::switchBranch(const QString& repository,const QString& branch) {
    if(repository!=service_.repository()) { cancelPreparation(); emit service_.branchSwitchFinished(repository,false); return; }
    if(service_.busy()) { pendingStageRepository_=repository; pendingBranch_=branch; preparing_=true; updateActions(); return; }
    cancelPreparation(); service_.switchBranch(branch);
}
void GitPanel::stagePaths(const QString& repository,const QStringList& paths) {
    // Keep a requested smart commit pending until the index write has finished.
    ++operationGeneration_; preparing_=false;
    if(repository!=service_.repository()) { cancelPreparation(); return; }
    if(service_.busy()) { pendingStageRepository_=repository; pendingStage_=paths; preparing_=true; updateActions(); return; }
    if(auto* section=sections_.value(repository)) section->error->hide();
    service_.stage(paths);
}
void GitPanel::requestCommit(Section* section,bool stageAll) {
    if(service_.busy() || !select(section) || section->message->toPlainText().trimmed().isEmpty()) return;
    bool staged=false,conflict=false; QStringList paths;
    for(const auto& entry:service_.status().entries) { staged|=entry.staged(); conflict|=entry.conflict(); if(entry.unstaged()) paths+=entry.paths(); }
    if(conflict) return;
    if((stageAll || !staged) && !paths.isEmpty()) {
        if(!stageAll && IconDialogs::MessageBox::question(this,I18n::text("Commit"),I18n::text("There are no staged changes. Stage all changes and commit?"),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes) return;
        paths.removeDuplicates(); section->commitAfterStage=true;
        ++operationGeneration_; preparing_=true; updateActions(); emit stageRequested(section->repository,paths); return;
    }
    if(staged) { section->error->hide(); service_.commit(section->message->toPlainText()); }
}
void GitPanel::updateActions() {
    const bool busy=service_.busy() || preparing_;
    refresh_->setEnabled(!busy);
    for(auto* section:sections_) {
        const auto& status=service_.status(section->repository);
        bool conflict=false; for(const auto& entry:status.entries) conflict|=entry.conflict();
        section->commit->setEnabled(!busy && status.valid && !status.entries.isEmpty() && !conflict && !section->message->toPlainText().trimmed().isEmpty());
        section->commitMore->setEnabled(!busy && status.valid);
        section->message->setEnabled(!service_.writing() && !preparing_);
        section->branches->setEnabled(!busy && status.valid); section->refresh->setEnabled(!busy); section->more->setEnabled(!busy && status.valid);
        section->files->setEnabled(!service_.writing() && !preparing_);
        const bool working=(service_.writing() || preparing_) && service_.repository()==section->repository;
        section->state->setText(working?I18n::text("Git: working…"):!status.valid?I18n::text("Git status unavailable. Refresh to retry."):conflict?I18n::text("Resolve conflicts before committing."):status.entries.isEmpty()?I18n::text("Working tree clean"):QString());
        section->state->setVisible(!section->state->text().isEmpty());
    }
}
void GitPanel::rebuild() {
    const auto repositories=service_.repositories();
    for(auto it=sections_.begin();it!=sections_.end();) {
        if(!repositories.contains(it.key())) { auto* section=it.value(); delete section->root; delete section; it=sections_.erase(it); }
        else ++it;
    }
    int total=0;
    for(const auto& repository:repositories) {
        auto* section=sections_.value(repository); if(!section) { section=addRepository(repository); applyFontSettings(); }
        const auto& status=service_.status(repository); total+=status.entries.size();
        if(section->initialized && section->rendered==status) continue;
        const QSignalBlocker blocker(section->branches); section->branches->clear();
        for(const auto& branch:status.branches) section->branches->addItem(Codicons::icon("git-branch"),branch,branch);
        auto current=section->branches->findData(status.currentBranch);
        if(current<0) { section->branches->insertItem(0,status.valid?I18n::text("Detached HEAD"):I18n::text("Branch"),QString()); current=0; }
        section->branches->setCurrentIndex(current); section->branches->updateGeometry(); section->branches->setToolTip(repository+"\n"+status.branch);
        rebuildFiles(section); section->rendered=status; section->initialized=true;
    }
    empty_->setVisible(repositories.isEmpty()); emit countChanged(total); updateActions();
}
void GitPanel::rebuildFiles(Section* section) {
    auto* files=section->files; const int scroll=files->verticalScrollBar()->value();
    QString selectedPath; int selectedGroup=-1; QVector<bool> expanded;
    for(int group=0;group<files->topLevelItemCount();++group) {
        auto* item=files->topLevelItem(group); expanded.append(item->isExpanded());
        if(files->currentItem()==item) selectedGroup=group;
        else if(files->currentItem() && files->currentItem()->parent()==item) { selectedGroup=group; selectedPath=files->currentItem()->text(0); }
    }
    const QSignalBlocker blocker(files); files->setUpdatesEnabled(false); files->clear();
    const QStringList labels{I18n::text("Staged changes"),I18n::text("Changes"),I18n::text("Untracked files"),I18n::text("Conflicts")};
    QVector<QTreeWidgetItem*> groups;
    for(int group=0;group<labels.size();++group) {
        auto* item=new QTreeWidgetItem(files,{labels[group]}); item->setData(0,GroupRole,group); item->setData(0,StagedRole,group==0);
        item->setIcon(2,Codicons::icon(group==0?"remove":"add")); item->setToolTip(2,I18n::text(group==0?"Unstage all":"Stage all")); groups.append(item);
    }
    int displayed=0;
    const auto& entries=service_.status(section->repository).entries;
    for(qsizetype index=0;index<entries.size();++index) {
        const auto& entry=entries[index];
        for(int group:{0,1,3}) {
            const bool include=group==0?entry.staged():group==1?entry.unstaged():entry.conflict();
            if(!include || displayed>=3000) continue;
            ++displayed; const char code=group==0?entry.index:entry.untracked()?'U':entry.worktree;
            auto* item=new QTreeWidgetItem(groups[group],{entry.path,{},{},QString(QChar::fromLatin1(code))});
            item->setIcon(0,FileIcons::icon(entry.path)); item->setData(0,EntryRole,int(index)); item->setData(0,StagedRole,group==0);
            item->setIcon(2,Codicons::icon(group==0?"remove":"add")); item->setToolTip(2,I18n::text(group==0?"Unstage":"Stage"));
            if(group==1 && !entry.untracked() && entry.index!='D') { item->setIcon(1,Codicons::icon("discard")); item->setToolTip(1,I18n::text("Revert")); }
            item->setToolTip(0,entry.originalPath.isEmpty()?entry.path:entry.originalPath+" → "+entry.path);
            const char* role=entry.conflict()?"diagnostic.error":entry.untracked()||code=='A'?"syntax.string":code=='D'?"diagnostic.error":"accent";
            item->setForeground(3,ThemeManager::instance().color(role)); item->setTextAlignment(3,Qt::AlignCenter);
        }
    }
    int visible=0;
    for(int i=0;i<groups.size();++i) {
        auto* group=groups[i]; group->setText(3,QString::number(group->childCount())); group->setTextAlignment(3,Qt::AlignCenter);
        group->setHidden(group->childCount()==0);
        if(i==selectedGroup) {
            if(selectedPath.isEmpty()) files->setCurrentItem(group);
            else for(int row=0;row<group->childCount();++row) if(group->child(row)->text(0)==selectedPath) files->setCurrentItem(group->child(row));
        }
        const bool open=i>=expanded.size() || expanded[i]; group->setExpanded(open);
        if(!group->isHidden()) visible+=1+(open?group->childCount():0);
    }
    if(displayed>=3000) { auto* limit=new QTreeWidgetItem(files,{I18n::text("File list limited to 3000 rows.")}); limit->setFlags(Qt::NoItemFlags); }
    files->setFixedHeight(qBound(32,(visible+1)*qMax(22,files->fontMetrics().height()+4),360));
    files->doItemsLayout(); files->verticalScrollBar()->setValue(scroll); files->setUpdatesEnabled(true);
}
void GitPanel::activate(Section* section,QTreeWidgetItem* item,int column) {
    if(!item || !select(section)) return;
    if(column==2) { changeIndex(section,item); return; }
    if(!item->data(0,EntryRole).isValid()) {
        if(column==0) item->setExpanded(!item->isExpanded());
        return;
    }
    const auto index=item->data(0,EntryRole).toInt();
    if(index<0 || index>=service_.status().entries.size()) return;
    const auto entry=service_.status().entries[index]; section->error->hide();
    if(column==1) {
        if(!item->icon(1).isNull()) emit revertRequested(section->repository,entry);
    } else service_.diff(entry,item->data(0,StagedRole).toBool());
}
void GitPanel::changeIndex(Section* section,QTreeWidgetItem* item) {
    if(!select(section)) return;
    const bool staged=item->data(0,StagedRole).toBool(); QStringList paths;
    const auto& entries=service_.status().entries;
    if(item->data(0,EntryRole).isValid()) {
        const int index=item->data(0,EntryRole).toInt(); if(index<0 || index>=entries.size()) return; paths=entries[index].paths();
    } else if(item->data(0,GroupRole).isValid()) {
        const int group=item->data(0,GroupRole).toInt();
        for(const auto& entry:entries) if((group==0 && entry.staged()) || (group==1 && entry.unstaged()) || (group==3 && entry.conflict())) paths+=entry.paths();
    }
    paths.removeDuplicates(); if(paths.isEmpty()) return; section->error->hide();
    if(staged) service_.unstage(paths);
    else { ++operationGeneration_; preparing_=true; updateActions(); emit stageRequested(section->repository,paths); }
}
void GitPanel::showFileMenu(Section* section,const QPoint& point) {
    auto* files=section->files; auto* item=files->itemAt(point);
    if(!item || !item->data(0,EntryRole).isValid() || !select(section)) return;
    const int index=item->data(0,EntryRole).toInt(); if(index<0 || index>=service_.status().entries.size()) return;
    const auto entry=service_.status().entries[index]; const auto repository=section->repository; const bool staged=item->data(0,StagedRole).toBool();
    files->setCurrentItem(item);
    auto* menu=new QMenu(files); menu->setObjectName("gitFileMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* diff=menu->addAction(Codicons::icon("diff"),I18n::text("Open changes"));
    connect(diff,&QAction::triggered,this,[this,section,entry,staged] { if(select(section)) service_.diff(entry,staged); });
    auto* open=menu->addAction(Codicons::icon("go-to-file"),I18n::text("Open file")); open->setEnabled(entry.worktree!='D' && entry.index!='D');
    connect(open,&QAction::triggered,this,[this,repository,entry] { emit openFileRequested(repository+"/"+entry.path); });
    menu->addSeparator();
    auto* stage=menu->addAction(Codicons::icon(staged?"remove":"add"),I18n::text(staged?"Unstage":"Stage"));
    connect(stage,&QAction::triggered,this,[this,section,entry,staged] {
        if(!select(section)) return;
        if(staged) service_.unstage(entry.paths());
        else { ++operationGeneration_; preparing_=true; updateActions(); emit stageRequested(section->repository,entry.paths()); }
    });
    auto* revert=menu->addAction(Codicons::icon("discard"),I18n::text("Revert")); revert->setObjectName("gitFileRevert");
    revert->setEnabled(!staged && !entry.untracked() && !entry.conflict() && entry.index!='D');
    connect(revert,&QAction::triggered,this,[this,section,entry] { if(select(section)) emit revertRequested(section->repository,entry); });
    auto* discard=menu->addAction(Codicons::icon("trash"),I18n::text("Discard changes…")); discard->setObjectName("gitFileDiscard");
    discard->setEnabled(!staged && !entry.untracked() && !entry.conflict() && entry.index!='D' && entry.worktree!='R');
    connect(discard,&QAction::triggered,this,[this,section,entry] {
        if(select(section) && !service_.busy()) emit discardRequested(section->repository,entry);
    });
    menu->addSeparator();
    auto* copy=menu->addAction(Codicons::icon("copy"),I18n::text("Copy path"));
    connect(copy,&QAction::triggered,this,[repository,entry] { QApplication::clipboard()->setText(repository+"/"+entry.path); });
    auto* relative=menu->addAction(Codicons::icon("copy"),I18n::text("Copy relative path"));
    connect(relative,&QAction::triggered,this,[entry] { QApplication::clipboard()->setText(entry.path); });
    menu->popup(files->viewport()->mapToGlobal(point));
}
void GitPanel::showRepositoryMenu(Section* section,QWidget* anchor) {
    if(!select(section)) return;
    auto* menu=new QMenu(anchor); menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* fetch=menu->addAction(Codicons::icon("cloud-download"),I18n::text("Fetch")); connect(fetch,&QAction::triggered,this,[this,section] { if(select(section)) service_.fetch(); });
    auto* pull=menu->addAction(Codicons::icon("arrow-down"),I18n::text("Pull (fast-forward only)"));
    connect(pull,&QAction::triggered,this,[this,section] {
        if(!select(section)) return;
        ++operationGeneration_; preparing_=true; updateActions(); emit pullRequested(section->repository);
    });
    auto* push=menu->addAction(Codicons::icon("arrow-up"),I18n::text("Push")); connect(push,&QAction::triggered,this,[this,section] { if(select(section)) service_.push(); });
    menu->addSeparator();
    auto* stage=menu->addAction(Codicons::icon("add"),I18n::text("Stage all"));
    connect(stage,&QAction::triggered,this,[this,section] { if(select(section)) changeIndex(section,section->files->topLevelItem(1)); });
    auto* unstage=menu->addAction(Codicons::icon("remove"),I18n::text("Unstage all"));
    connect(unstage,&QAction::triggered,this,[this,section] { if(select(section)) changeIndex(section,section->files->topLevelItem(0)); });
    auto* copy=menu->addAction(Codicons::icon("copy"),I18n::text("Copy repository path"));
    connect(copy,&QAction::triggered,this,[section] { QApplication::clipboard()->setText(section->repository); });
    menu->popup(anchor->mapToGlobal(QPoint(0,anchor->height())));
}
