#pragma once

#include <QWidget>

class QPushButton;
class QTreeWidget;

class SavedProjectsPage final : public QWidget {
    Q_OBJECT

public:
    explicit SavedProjectsPage(QWidget* parent = nullptr);
    void refresh(bool canReturnToEditor);

signals:
    void projectRequested(const QString& path);
    void fileRequested();
    void editorRequested();

private:
    void reload(const QString& selectedGroup = {}, const QString& selectedPath = {});
    void updateActions();
    void addFolder();
    void createGroup();
    void removeSelected();
    bool moveProject(const QString& path, const QString& group);
    bool moveGroup(const QString& group, const QString& parent);
    void openSelected();

    QTreeWidget* projects_;
    QPushButton* back_;
    QPushButton* open_;
    QPushButton* remove_;
};
