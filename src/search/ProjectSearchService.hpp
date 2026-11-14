#pragma once

#include <QObject>
#include "SearchMatches.hpp"
#include <QStringList>
#include <QList>
#include <QHash>
#include <QByteArray>
#include <QBitArray>

#include <atomic>
#include <memory>

struct ProjectSearchOptions {
    QStringList roots;
    QString query;
    QString filesToInclude;
    QString filesToExclude;
    bool matchCase{false};
    bool wholeWord{false};
    bool regularExpression{false};
    bool showHidden{false};
    bool includeIgnored{false};
};


struct ProjectSearchResult {
    SearchMatches matches;
    int filesSearched{0};
    int skippedFiles{0};
    QString error;
    QStringList warnings;
    SearchHashes fileHashes;
};

Q_DECLARE_METATYPE(ProjectSearchResult)

class ProjectSearchService final : public QObject {
    Q_OBJECT
public:
    explicit ProjectSearchService(QObject* parent = nullptr);
    ~ProjectSearchService() override;

    void search(ProjectSearchOptions options);
    void cancel();

signals:
    void searchStarted();
    void searchProgress(const ProjectSearchResult& result);
    void searchFinished(const ProjectSearchResult& result);

private:
    std::shared_ptr<std::atomic_bool> cancellation_;
    quint64 generation_{0};
};

struct ProjectReplaceRequest {
    SearchMatches matches;
    QBitArray selected; // Empty means all matches are selected.
    SearchHashes fileHashes;
    QString replacement;
    QStringList selectedPaths;
    QStringList protectedPaths;
    bool regularExpression{false};
};

struct ProjectReplaceResult {
    int matchesReplaced{0};
    int filesChanged{0};
    QStringList changedPaths;
    QStringList skippedProtected;
    QStringList skippedChanged;
    QStringList errors;
    bool cancelled{false};
};

class ProjectReplaceService final : public QObject {
    Q_OBJECT
public:
    explicit ProjectReplaceService(QObject* parent = nullptr);
    ~ProjectReplaceService() override;
    void replace(ProjectReplaceRequest request);
    void cancel();
    [[nodiscard]] static QString previewReplacement(const ProjectSearchMatch& match,
                                                    const QString& replacement,
                                                    bool regularExpression);

signals:
    void replaceFinished(const ProjectReplaceResult& result);

private:
    std::shared_ptr<std::atomic_bool> cancellation_;
};
