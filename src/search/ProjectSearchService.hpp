#pragma once

#include <QObject>
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
};

struct ProjectSearchMatch {
    QString path;
    QString relativePath;
    int line{0};
    int column{0};
    int length{0};
    QString preview;
    qint64 byteOffset{0};
    qint64 byteLength{0};
    QString matchedText;
    QStringList captures;
};

struct ProjectSearchResult {
    QList<ProjectSearchMatch> matches;
    int filesSearched{0};
    QString error;
    QHash<QString, QByteArray> fileHashes;
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
    void searchFinished(const ProjectSearchResult& result);

private:
    std::shared_ptr<std::atomic_bool> cancellation_;
    quint64 generation_{0};
};

struct ProjectReplaceRequest {
    QList<ProjectSearchMatch> matches;
    QBitArray selected; // Empty means all matches are selected.
    QHash<QString, QByteArray> fileHashes;
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
