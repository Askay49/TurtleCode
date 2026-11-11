#pragma once

#include <QWidget>
#include <QStringList>
#include <QBitArray>
#include <QHash>
#include <QVector>

#include "ProjectSearchService.hpp"

class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class SearchPanel final : public QWidget {
    Q_OBJECT
public:
    explicit SearchPanel(QWidget* parent = nullptr);
    void applyFontSettings();
    void setRoots(const QStringList& roots);
    void setShowHidden(bool show);
    void focusSearch();
    void startReplace(ProjectReplaceRequest request);

signals:
    void fileActivated(const QString& path, int line, int column, int length);
    void replaceRequested(const ProjectReplaceRequest& request);
    void replaceFinished(const ProjectReplaceResult& result);

private:
    void scheduleSearch();
    void startSearch();
    void displayResult(const ProjectSearchResult& result);
    void loadMore(const QString& path, int count);
    void updateReplacePreview();
    void updateReplaceButton();
    void requestReplace();
    ProjectSearchOptions options() const;

    QStringList roots_;
    bool showHidden_{false};
    ProjectSearchService service_;
    ProjectReplaceService replaceService_;
    ProjectSearchResult currentResult_;
    QHash<QString, QVector<qsizetype>> fileMatches_;
    QHash<QString, QTreeWidgetItem*> fileItems_;
    QHash<QString, int> loadedPerFile_;
    QHash<QString, qsizetype> selectedPerFile_;
    QBitArray selected_;
    qsizetype selectedCount_{0};
    ProjectSearchOptions searchedOptions_;
    bool replacing_{false};
    QString replaceSummary_;
    QTimer* debounce_{nullptr};
    QLineEdit* query_{nullptr};
    QLineEdit* replacement_{nullptr};
    QLineEdit* include_{nullptr};
    QLineEdit* exclude_{nullptr};
    QToolButton* replaceToggle_{nullptr};
    QToolButton* replaceButton_{nullptr};
    QToolButton* matchCase_{nullptr};
    QToolButton* wholeWord_{nullptr};
    QToolButton* regex_{nullptr};
    QWidget* replaceRow_{nullptr};
    QWidget* details_{nullptr};
    QTreeWidget* results_{nullptr};
    QLabel* status_{nullptr};
};
