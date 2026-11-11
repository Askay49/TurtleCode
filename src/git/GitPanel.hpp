#pragma once
#include "GitService.hpp"
#include <QWidget>
#include <QMap>
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class GitPanel final : public QWidget {
    Q_OBJECT
public:
    explicit GitPanel(QWidget* parent = nullptr);
    ~GitPanel() override;
    void pullRepository(const QString& repository);
    void setRoots(const QStringList& roots);
    void refresh();
    void applyFontSettings();
    GitService* service() { return &service_; }
    quint64 operationGeneration() const { return operationGeneration_; }
    void cancelPreparation();
    void switchBranch(const QString& repository, const QString& branch);
    void stagePaths(const QString& repository, const QStringList& paths);
signals:
    void discardRequested(const QString& repository, const GitEntry& entry);
    void pullRequested(const QString& repository);
    void branchSwitchRequested(const QString& repository, const QString& branch);
    void revertRequested(const QString& repository, const GitEntry& entry);
    void stageRequested(const QString& repository, const QStringList& paths);
    void openFileRequested(const QString& path);
    void diffReady(const QString& absolutePath, bool staged, const QString& text);
    void countChanged(int count);
private:
    struct Section {
        QString repository;
        QWidget *root{}, *body{};
        QToolButton *heading{}, *refresh{}, *more{}, *commitMore{};
        QComboBox* branches{};
        QLabel *state{}, *error{};
        QPlainTextEdit* message{};
        QPushButton* commit{};
        QTreeWidget* files{};
        GitStatus rendered;
        bool initialized{false}, commitAfterStage{false};
    };
    Section* addRepository(const QString& repository);
    bool select(Section* section);
    void rebuild();
    void rebuildFiles(Section* section);
    void updateActions();
    void activate(Section* section, QTreeWidgetItem* item, int column);
    void changeIndex(Section* section, QTreeWidgetItem* item);
    void requestCommit(Section* section, bool stageAll = false);
    void showFileMenu(Section* section, const QPoint& point);
    void showRepositoryMenu(Section* section, QWidget* anchor);
    quint64 operationGeneration_{0};
    bool preparing_{false}, pendingPull_{false};
    QStringList roots_, pendingStage_;
    QString pendingStageRepository_, pendingBranch_;
    GitService service_;
    QMap<QString, Section*> sections_;
    QVBoxLayout* repositoriesLayout_{};
    QLabel* empty_{};
    QToolButton* refresh_{};
    QTimer* poll_{};
};
