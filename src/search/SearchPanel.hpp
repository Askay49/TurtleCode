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
    void setIncludeIgnored(bool include) { includeIgnored_=include; if(!queryTextEmpty()) scheduleSearch(); }
    void focusSearch();
    void startReplace(ProjectReplaceRequest request);

signals:
    void fileActivated(const QString& path, int line, int column, int length);
    void replaceRequested(const ProjectReplaceRequest& request);
    void replaceFinished(const ProjectReplaceResult& result);

private:
    bool queryTextEmpty() const;
    void scheduleSearch();
    void startSearch();
    void displayResult(const ProjectSearchResult& result);
    void deliverResults();
    void changePage(qsizetype first);
    qsizetype pageFirst_{0};
    QVector<qsizetype> previousPages_;
    QTimer* deliveryTimer_{nullptr};
    qsizetype renderedMatches_{0};
    bool searching_{false};
    void loadMore(const QString& path, int count);
    void updateReplacePreview();
    void updateReplaceButton();
    void requestReplace();
    ProjectSearchOptions options() const;

    QStringList roots_;
    bool showHidden_{false},includeIgnored_{false};
    ProjectSearchService service_;
    ProjectReplaceService replaceService_;
    ProjectSearchResult currentResult_;
    struct Range {
        qsizetype first{},count{};
        qsizetype size() const{return count;} qsizetype front() const{return first;}
        qsizetype operator[](qsizetype i)const{return first+i;}
        struct Iterator {qsizetype value;qsizetype operator*()const{return value;}Iterator& operator++(){++value;return *this;}bool operator!=(const Iterator& other)const{return value!=other.value;}};
        Iterator begin()const{return {first};}Iterator end()const{return {first+count};}
    };
    QHash<QString, Range> fileMatches_;
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
