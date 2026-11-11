#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QByteArray>
#include <QProcess>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QString>

class QNetworkReply;

class GrammarManager final : public QObject {
    Q_OBJECT
public:
    explicit GrammarManager(QObject* parent = nullptr);
    ~GrammarManager() override;
    [[nodiscard]] static QString grammarDirectory(const QString& id);
    [[nodiscard]] static QString libraryPath(const QString& id);
    [[nodiscard]] static QString queryPath(const QString& id);
    [[nodiscard]] static bool isInstalled(const QString& id);
    void ensureCppInstalled();
    void install(const QString& id);

signals:
    void statusChanged(const QString& id, const QString& message);
    void installationFinished(const QString& id, bool success);

private:
    void downloadCandidate(const QString& id, const QString& ref, bool tag = false);
    void tryOlderVersion(const QString& id, const QString& reason);
    void fetchTags(const QString& id, int page = 1);
    void extractAndBuild(const QString& id, const QByteArray& archive);
    void finish(const QString& id, bool success, const QString& message);
    QNetworkAccessManager network_;
    QHash<QString, QTemporaryDir*> temporaryDirectories_;
    QHash<QString, QProcess*> processes_;
    QSet<QString> installing_;
    QHash<QString, QStringList> candidates_;
    QHash<QString, QString> selectedRefs_;
    QHash<QString, QString> candidateErrors_;
    QSet<QString> tagsLoaded_;
};
