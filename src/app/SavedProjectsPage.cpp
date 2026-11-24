#include "SavedProjectsPage.hpp"

#include "Codicons.hpp"
#include "I18n.hpp"
#include "IconDialogs.hpp"

#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLockFile>
#include <QMap>
#include <QMimeData>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVariant>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr auto catalogKey = "projects/saved";
constexpr auto pathRole = Qt::UserRole;
constexpr auto groupRole = Qt::UserRole + 1;
constexpr auto projectMimeType = "application/x-turtle-code-saved-project";

struct Catalog {
    QStringList groups;
    QVariantMap folders;
    QVariantMap parents;

    QString parentOf(const QString& group) const { return parents.value(group).toString(); }

    void setParent(const QString& group, const QString& parent) {
        if (parent.isEmpty()) parents.remove(group);
        else parents.insert(group, parent);
    }

    bool canPlaceGroup(const QString& group, const QString& parent) const {
        if (!groups.contains(group) || (!parent.isEmpty() && !groups.contains(parent))) return false;
        QSet<QString> visited;
        for (auto ancestor = parent; !ancestor.isEmpty(); ancestor = parentOf(ancestor)) {
            if (ancestor == group || visited.contains(ancestor)) return false;
            visited.insert(ancestor);
        }
        return true;
    }
};

Catalog readCatalog(const QSettings& settings) {
    const auto data = settings.value(catalogKey).toMap();
    Catalog catalog{data.value("groups").toStringList(), data.value("folders").toMap(), data.value("parents").toMap()};
    for (const auto& group : catalog.parents.keys()) {
        const auto parent = catalog.parentOf(group);
        if (parent.isEmpty() || !catalog.canPlaceGroup(group, parent)) catalog.parents.remove(group);
    }
    return catalog;
}

bool changeCatalog(QWidget* parent, const auto& edit) {
    QSettings settings;
    // Each edit reads the latest catalog while holding the same lock in every window.
    QLockFile lock(settings.fileName() + ".saved-projects.lock");
    if (QDir().mkpath(QFileInfo(settings.fileName()).absolutePath()) && lock.tryLock()) {
        settings.sync();
        if (settings.status() == QSettings::NoError) {
            auto data = readCatalog(settings);
            if (edit(data)) {
                settings.setValue(catalogKey, QVariantMap{{"groups", data.groups}, {"folders", data.folders},
                                                          {"parents", data.parents}});
                settings.sync();
            }
            if (settings.status() == QSettings::NoError) return true;
        }
    }
    if (lock.isLocked()) lock.unlock();
    IconDialogs::MessageBox::warning(parent, I18n::text("Projects"),
                                     I18n::text("Could not save the project list. Try again."));
    return false;
}

QString groupOf(const QTreeWidgetItem* item) {
    return item ? item->data(0, groupRole).toString() : QString{};
}

QString pathOf(const QTreeWidgetItem* item) {
    return item ? item->data(0, pathRole).toString() : QString{};
}

class ProjectDragData final : public QMimeData {
public:
    ProjectDragData(const QTreeWidget* source, const QTreeWidgetItem* item)
        : source(source), path(pathOf(item)), group(groupOf(item)), parent(groupOf(item->parent())) {
        setData(projectMimeType, {});
    }

    const QTreeWidget* const source;
    const QString path;
    const QString group;
    const QString parent;
};

class ProjectsTree final : public QTreeWidget {
public:
    ProjectsTree(QWidget* parent, std::function<bool(const ProjectDragData&, const QString&)> moveItem)
        : QTreeWidget(parent), moveItem_(std::move(moveItem)) {
        setDragDropMode(QAbstractItemView::DragDrop);
        setDragDropOverwriteMode(true);
    }

protected:
    QStringList mimeTypes() const override { return {projectMimeType}; }
    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override {
        if (items.size() != 1 || !(items.front()->flags() & Qt::ItemIsDragEnabled)) return nullptr;
        return new ProjectDragData(this, items.front());
    }

    void startDrag(Qt::DropActions) override {
        const auto items = selectedItems();
        auto* data = mimeData(items);
        if (!data) return;
        auto* drag = new QDrag(this);
        drag->setMimeData(data);
        drag->setPixmap(items.front()->icon(0).pixmap(iconSize()));
        // The catalog owns the move. Qt must not remove a row after the drop.
        drag->exec(Qt::MoveAction);
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (draggedItem(*event)) QTreeWidget::dragEnterEvent(event);
        else event->ignore();
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        const bool valid = dropGroup(event->position().toPoint(), draggedItem(*event));
        setDropIndicatorShown(valid);
        QTreeWidget::dragMoveEvent(event);
        if (!valid) event->ignore();
    }

    void dropEvent(QDropEvent* event) override {
        const auto* item = draggedItem(*event);
        const auto* group = dropGroup(event->position().toPoint(), item);
        QDragLeaveEvent leave;
        QTreeWidget::dragLeaveEvent(&leave);
        if (!group || !moveItem_(*item, groupOf(group))) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

private:
    const ProjectDragData* draggedItem(const QDropEvent& event) const {
        const auto* data = dynamic_cast<const ProjectDragData*>(event.mimeData());
        return data && data->source == this && event.possibleActions().testFlag(Qt::MoveAction) ? data : nullptr;
    }

    const QTreeWidgetItem* dropGroup(const QPoint& position, const ProjectDragData* drag) const {
        const auto* item = itemAt(position);
        if (!drag || !item || !pathOf(item).isEmpty() || groupOf(item) == drag->parent) return nullptr;
        if (drag->path.isEmpty()) {
            for (auto* ancestor = item; ancestor; ancestor = ancestor->parent())
                if (groupOf(ancestor) == drag->group) return nullptr;
        }
        return item;
    }

    std::function<bool(const ProjectDragData&, const QString&)> moveItem_;
};

}

SavedProjectsPage::SavedProjectsPage(QWidget* parent) : QWidget(parent) {
    setObjectName("savedProjectsPage");
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(32, 24, 32, 24);
    auto* panel = new QWidget(this);
    panel->setMaximumWidth(900);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    outer->addStretch(1);
    outer->addWidget(panel, 10);
    outer->addStretch(1);

    auto* title = new QLabel(I18n::text("Projects"), panel);
    auto font = title->font();
    font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() + 7 : 18);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);

    auto* toolbar = new QHBoxLayout;
    auto* add = new QPushButton(Codicons::icon("folder-opened"), I18n::text("Add Folder…"), panel);
    add->setObjectName("savedProjectsAddFolder");
    auto* group = new QPushButton(Codicons::icon("new-folder"), I18n::text("Create Group…"), panel);
    group->setObjectName("savedProjectsCreateGroup");
    auto* file = new QPushButton(Codicons::icon("go-to-file"), I18n::text("Open File…"), panel);
    file->setObjectName("savedProjectsOpenFile");
    back_ = new QPushButton(Codicons::icon("arrow-left"), I18n::text("Back to Editor"), panel);
    back_->setObjectName("savedProjectsBack");
    back_->hide();
    toolbar->addWidget(add);
    toolbar->addWidget(group);
    toolbar->addStretch();
    toolbar->addWidget(back_);
    toolbar->addWidget(file);
    layout->addLayout(toolbar);

    projects_ = new ProjectsTree(panel, [this](const ProjectDragData& item, const QString& group) {
        return item.path.isEmpty() ? moveGroup(item.group, group) : moveProject(item.path, group);
    });
    projects_->setObjectName("savedProjectsList");
    projects_->setAccessibleName(I18n::text("Projects"));
    projects_->setHeaderLabels({I18n::text("Project"), I18n::text("Folder Path")});
    projects_->header()->resizeSection(0, 240);
    projects_->setUniformRowHeights(true);
    projects_->setAllColumnsShowFocus(true);
    projects_->setIconSize(QSize(18, 18));
    layout->addWidget(projects_, 1);

    auto* actions = new QHBoxLayout;
    remove_ = new QPushButton(Codicons::icon("trash"), {}, panel);
    remove_->setObjectName("savedProjectsRemove");
    open_ = new QPushButton(Codicons::icon("folder-opened"), I18n::text("Open Project"), panel);
    open_->setObjectName("savedProjectsOpen");
    actions->addWidget(remove_);
    actions->addStretch();
    actions->addWidget(open_);
    layout->addLayout(actions);
    setFocusProxy(projects_);

    connect(add, &QPushButton::clicked, this, &SavedProjectsPage::addFolder);
    connect(group, &QPushButton::clicked, this, &SavedProjectsPage::createGroup);
    connect(file, &QPushButton::clicked, this, &SavedProjectsPage::fileRequested);
    connect(back_, &QPushButton::clicked, this, &SavedProjectsPage::editorRequested);
    connect(remove_, &QPushButton::clicked, this, &SavedProjectsPage::removeSelected);
    connect(open_, &QPushButton::clicked, this, &SavedProjectsPage::openSelected);
    connect(projects_, &QTreeWidget::currentItemChanged, this, &SavedProjectsPage::updateActions);
    connect(projects_, &QTreeWidget::itemActivated, this, &SavedProjectsPage::openSelected);
}

void SavedProjectsPage::refresh(bool canReturnToEditor) {
    back_->setVisible(canReturnToEditor);
    const auto* item = projects_->currentItem();
    reload(groupOf(item), pathOf(item));
}

void SavedProjectsPage::reload(const QString& selectedGroup, const QString& selectedPath) {
    auto data = readCatalog(QSettings{});
    std::sort(data.groups.begin(), data.groups.end(), [](const QString& left, const QString& right) {
        return left.localeAwareCompare(right) < 0;
    });

    QSet<QString> collapsed;
    for (QTreeWidgetItemIterator it(projects_); *it; ++it) {
        const auto* item = *it;
        if (pathOf(item).isEmpty() && !item->isExpanded()) collapsed.insert(groupOf(item));
    }
    projects_->clear();

    QMap<QString, QTreeWidgetItem*> sections;
    const auto groups = data.groups + QStringList{QString{}};
    for (const auto& group : groups) {
        auto* section = new QTreeWidgetItem(QStringList{group.isEmpty() ? I18n::text("Ungrouped") : group});
        section->setData(0, groupRole, group);
        if (group.isEmpty()) section->setFlags(section->flags() & ~Qt::ItemIsDragEnabled);
        section->setIcon(0, Codicons::icon("folder-library"));
        sections.insert(group, section);
    }
    for (const auto& group : groups) {
        auto* section = sections.value(group);
        const auto parent = data.parentOf(group);
        if (parent.isEmpty()) projects_->addTopLevelItem(section);
        else sections.value(parent)->addChild(section);
    }
    auto* selected = sections.value(selectedGroup, sections.value(QString{}));
    for (auto it = data.folders.cbegin(); it != data.folders.cend(); ++it) {
        const QFileInfo folder(it.key());
        auto* section = sections.value(it.value().toString(), sections.value(QString{}));
        auto* project = new QTreeWidgetItem(section, {folder.fileName().isEmpty() ? it.key() : folder.fileName(),
                                                      QDir::toNativeSeparators(it.key())});
        project->setData(0, pathRole, it.key());
        project->setData(0, groupRole, groupOf(section));
        project->setFlags(project->flags() & ~Qt::ItemIsDropEnabled);
        project->setToolTip(1, project->text(1));
        const bool available = folder.isDir();
        project->setIcon(0, Codicons::icon(available ? "folder" : "warning"));
        if (!available) project->setToolTip(0, I18n::text("Project folder is unavailable."));
        if (it.key() == selectedPath) selected = project;
    }
    for (auto* section : sections) {
        section->setFirstColumnSpanned(true);
        section->sortChildren(0, Qt::AscendingOrder);
        section->setExpanded(!collapsed.contains(groupOf(section)));
    }
    for (auto* ancestor = selected->parent(); ancestor; ancestor = ancestor->parent()) ancestor->setExpanded(true);
    projects_->setCurrentItem(selected);
    updateActions();
}

void SavedProjectsPage::updateActions() {
    const auto* item = projects_->currentItem();
    const bool isProject = !pathOf(item).isEmpty();
    const bool isGroup = !isProject && !groupOf(item).isEmpty();
    open_->setEnabled(isProject);
    remove_->setText(I18n::text(isGroup ? "Delete Group" : "Remove from List"));
    remove_->setEnabled(isProject || isGroup);
}

void SavedProjectsPage::addFolder() {
    const auto selectedGroup = groupOf(projects_->currentItem());
    const auto path = IconDialogs::FileDialog::getExistingDirectory(this, I18n::text("Add Folder…"), QDir::homePath());
    if (path.isEmpty()) return;
    const auto folder = QFileInfo(path).canonicalFilePath();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) {
        IconDialogs::MessageBox::warning(this, I18n::text("Projects"), I18n::text("Project folder is unavailable."));
        return;
    }

    const auto data = readCatalog(QSettings{});
    QString group;
    if (!data.folders.contains(folder) && !data.groups.isEmpty()) {
        const auto ungrouped = I18n::text("Ungrouped");
        IconDialogs::InputDialog dialog(this);
        dialog.setWindowTitle(I18n::text("Add Folder…"));
        dialog.setLabelText(I18n::text("Project group:"));
        dialog.setComboBoxItems(QStringList{ungrouped} + data.groups);
        dialog.setTextValue(data.groups.contains(selectedGroup) ? selectedGroup : ungrouped);
        if (dialog.exec() != QDialog::Accepted) return;
        if (dialog.textValue() != ungrouped) group = dialog.textValue();
    }
    if (changeCatalog(this, [&](Catalog& latest) {
        if (latest.folders.contains(folder)) return false;
        latest.folders.insert(folder, latest.groups.contains(group) ? group : QString{});
        return true;
    })) reload({}, folder);
}

void SavedProjectsPage::createGroup() {
    auto name = IconDialogs::InputDialog::getText(this, I18n::text("Create Group…"),
                                                  I18n::text("Group name:")).trimmed();
    if (name.isEmpty()) return;
    if (name.compare(I18n::text("Ungrouped"), Qt::CaseInsensitive) == 0 ||
        name.compare(QStringLiteral("Ungrouped"), Qt::CaseInsensitive) == 0 ||
        name.compare(QStringLiteral("Без группы"), Qt::CaseInsensitive) == 0) {
        IconDialogs::MessageBox::warning(this, I18n::text("Projects"), I18n::text("That group name is reserved."));
        return;
    }

    if (changeCatalog(this, [&](Catalog& data) {
        for (const auto& group : data.groups) {
            if (group.compare(name, Qt::CaseInsensitive) != 0) continue;
            name = group;
            return false;
        }
        data.groups.append(name);
        return true;
    })) reload(name);
}

void SavedProjectsPage::removeSelected() {
    const auto* item = projects_->currentItem();
    if (!item) return;
    const auto path = pathOf(item);
    const auto group = groupOf(item);
    if (path.isEmpty() && group.isEmpty()) return;

    auto selectedGroup = group;
    if (changeCatalog(this, [&](Catalog& data) {
        if (!path.isEmpty()) return data.folders.remove(path) > 0;
        if (!data.groups.removeOne(group)) return false;
        selectedGroup = data.parents.take(group).toString();
        for (const auto& child : data.groups)
            if (data.parentOf(child) == group) data.setParent(child, selectedGroup);
        for (auto& assigned : data.folders)
            if (assigned.toString() == group) assigned = selectedGroup;
        return true;
    })) reload(selectedGroup);
}

bool SavedProjectsPage::moveProject(const QString& path, const QString& group) {
    bool moved = false;
    if (!changeCatalog(this, [&](Catalog& data) {
        auto project = data.folders.find(path);
        if (project == data.folders.end() || (!group.isEmpty() && !data.groups.contains(group)) ||
            project.value().toString() == group) return false;
        project.value() = group;
        moved = true;
        return true;
    })) return false;
    reload(group, path);
    return moved;
}

bool SavedProjectsPage::moveGroup(const QString& group, const QString& parent) {
    bool moved = false;
    if (!changeCatalog(this, [&](Catalog& data) {
        if (!data.canPlaceGroup(group, parent) || data.parentOf(group) == parent) return false;
        data.setParent(group, parent);
        moved = true;
        return true;
    })) return false;
    reload(group);
    return moved;
}

void SavedProjectsPage::openSelected() {
    const auto path = pathOf(projects_->currentItem());
    if (path.isEmpty()) return;
    if (!QFileInfo(path).isDir()) {
        IconDialogs::MessageBox::warning(this, I18n::text("Projects"), I18n::text("Project folder is unavailable."));
        reload({}, path);
        return;
    }
    emit projectRequested(path);
}
