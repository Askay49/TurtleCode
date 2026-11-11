#include "search/ProjectSearchService.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>

namespace {
void writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    Q_ASSERT(file.open(QIODevice::WriteOnly));
    Q_ASSERT(file.write(contents) == contents.size());
}

ProjectSearchResult search(ProjectSearchOptions options) {
    ProjectSearchService service;
    ProjectSearchResult result;
    QEventLoop loop;
    QObject::connect(&service, &ProjectSearchService::searchFinished, &loop,
                     [&](const ProjectSearchResult& value) {
        result = value;
        loop.quit();
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    service.search(std::move(options));
    loop.exec();
    return result;
}

ProjectReplaceResult replace(ProjectReplaceRequest request) {
    ProjectReplaceService service;
    ProjectReplaceResult result;
    QEventLoop loop;
    bool finished = false;
    QObject::connect(&service, &ProjectReplaceService::replaceFinished, &loop,
                     [&](const ProjectReplaceResult& value) {
        result = value;
        finished = true;
        loop.quit();
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    service.replace(std::move(request));
    loop.exec();
    Q_ASSERT(finished);
    return result;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    Q_ASSERT(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    Q_ASSERT(directory.isValid());
    QDir root(directory.path());
    Q_ASSERT(root.mkpath("src"));
    Q_ASSERT(root.mkpath("build"));
    Q_ASSERT(root.mkpath(".private"));
    writeFile(root.filePath("src/main.cpp"), "Blink blink\nBLINK\n");
    writeFile(root.filePath("src/other.txt"), "blinking blink\n");
    writeFile(root.filePath("build/generated.cpp"), "blink\n");
    writeFile(root.filePath(".secret.txt"), "blink\n");
    writeFile(root.filePath(".private/note.txt"), "blink\n");
    writeFile(root.filePath(".gitignore"), "build/\n");

    ProjectSearchOptions options;
    options.roots = {directory.path()};
    options.query = "blink";
    auto result = search(options);
    Q_ASSERT(result.error.isEmpty());
    Q_ASSERT(result.matches.size() == 6);
    Q_ASSERT(std::none_of(result.matches.cbegin(), result.matches.cend(), [](const auto& match) {
        return match.relativePath.startsWith('.') && match.relativePath != ".gitignore";
    }));

    options.matchCase = true;
    result = search(options);
    Q_ASSERT(result.matches.size() == 4);

    options.matchCase = false;
    options.wholeWord = true;
    result = search(options);
    Q_ASSERT(result.matches.size() == 5);

    options.wholeWord = false;
    options.filesToInclude = "**/*.cpp";
    result = search(options);
    Q_ASSERT(result.matches.size() == 4);

    options.filesToInclude.clear();
    options.regularExpression = true;
    options.query = "blink(?:ing)?";
    result = search(options);
    Q_ASSERT(result.matches.size() == 6);

    options.regularExpression = false;
    options.query = "blink";
    options.showHidden = true;
    result = search(options);
    Q_ASSERT(result.matches.size() == 8);

    const QString mainPath = root.filePath("src/main.cpp");
    ProjectReplaceRequest selected;
    selected.fileHashes = result.fileHashes;
    selected.replacement = "replaced";
    for (const auto& match : result.matches)
        if (match.path == mainPath && match.line == 1) selected.matches.push_back(match);
    Q_ASSERT(selected.matches.size() == 2);
    auto replaced = replace(selected);
    Q_ASSERT(replaced.matchesReplaced == 2 && replaced.filesChanged == 1);
    Q_ASSERT(readFile(mainPath) == "replaced replaced\nBLINK\n");
    Q_ASSERT(readFile(root.filePath("src/other.txt")) == "blinking blink\n");

    auto staleSearch = search(options);
    ProjectReplaceRequest stale;
    stale.fileHashes = staleSearch.fileHashes;
    stale.replacement = "x";
    for (const auto& match : staleSearch.matches)
        if (match.path == mainPath) stale.matches.push_back(match);
    writeFile(mainPath, "changed externally\n");
    replaced = replace(stale);
    Q_ASSERT(replaced.matchesReplaced == 0 && replaced.skippedChanged.contains(mainPath));
    Q_ASSERT(readFile(mainPath) == "changed externally\n");

    const QString otherPath = root.filePath("src/other.txt");
    auto protectedSearch = search(options);
    ProjectReplaceRequest protectedRequest;
    protectedRequest.fileHashes = protectedSearch.fileHashes;
    protectedRequest.replacement = "x";
    protectedRequest.protectedPaths = {otherPath};
    for (const auto& match : protectedSearch.matches)
        if (match.path == otherPath) protectedRequest.matches.push_back(match);
    replaced = replace(protectedRequest);
    Q_ASSERT(replaced.matchesReplaced == 0 && replaced.skippedProtected.contains(otherPath));
    Q_ASSERT(readFile(otherPath) == "blinking blink\n");

    const QString unicodePath = root.filePath("src/unicode.txt");
    writeFile(unicodePath, QString::fromUtf8("привет item-12\nitem-34\n").toUtf8());
    ProjectSearchOptions regexOptions;
    regexOptions.roots = {directory.path()};
    regexOptions.query = "item-(\\d+)";
    regexOptions.regularExpression = true;
    auto regexResult = search(regexOptions);
    ProjectReplaceRequest regexRequest;
    regexRequest.fileHashes = regexResult.fileHashes;
    regexRequest.replacement = "number:$1:$$";
    regexRequest.regularExpression = true;
    for (const auto& match : regexResult.matches)
        if (match.path == unicodePath) regexRequest.matches.push_back(match);
    Q_ASSERT(regexRequest.matches.size() == 2);
    replaced = replace(regexRequest);
    Q_ASSERT(replaced.matchesReplaced == 2);
    Q_ASSERT(readFile(unicodePath) == QString::fromUtf8("привет number:12:$\nnumber:34:$\n").toUtf8());

    const QString manyPath = root.filePath("src/many.txt");
    QByteArray many;
    for (int index = 0; index < 20'000; ++index) many += "foo ";
    writeFile(manyPath, many);
    ProjectSearchOptions manyOptions;
    manyOptions.roots = {directory.path()};
    manyOptions.query = "foo";
    auto manyResult = search(manyOptions);
    Q_ASSERT(manyResult.matches.size() == 20'000);
    ProjectReplaceRequest manyRequest;
    manyRequest.matches = manyResult.matches;
    manyRequest.fileHashes = manyResult.fileHashes;
    manyRequest.replacement = "bar";
    manyRequest.selected.resize(manyResult.matches.size());
    manyRequest.selected.fill(true);
    for (int index = 1; index < manyRequest.selected.size(); index += 2)
        manyRequest.selected.clearBit(index);
    replaced = replace(manyRequest);
    Q_ASSERT(replaced.matchesReplaced == 10'000 && replaced.filesChanged == 1);
    QByteArray expectedMany;
    for (int index = 0; index < 20'000; ++index)
        expectedMany += index % 2 == 0 ? "bar " : "foo ";
    Q_ASSERT(readFile(manyPath) == expectedMany);

    const QString largePath = root.filePath("src/large.txt");
    QByteArray large(17 * 1024 * 1024, 'x');
    large += "needle\n";
    writeFile(largePath, large);
    ProjectSearchOptions largeOptions;
    largeOptions.roots = {directory.path()};
    largeOptions.query = "needle";
    auto largeResult = search(largeOptions);
    Q_ASSERT(largeResult.matches.size() == 1);
    ProjectReplaceRequest largeRequest;
    largeRequest.matches = largeResult.matches;
    largeRequest.fileHashes = largeResult.fileHashes;
    largeRequest.replacement = "found";
    replaced = replace(largeRequest);
    Q_ASSERT(replaced.matchesReplaced == 1);
    Q_ASSERT(readFile(largePath).endsWith("found\n"));
    return 0;
}
