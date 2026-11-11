#include "language/LspClient.hpp"
#include "language/SemanticHighlighting.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdio>

static bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return predicate();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (QStandardPaths::findExecutable(QStringLiteral("clangd")).isEmpty()) return 0;
    QTemporaryDir directory;
    Q_ASSERT(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("sample.cpp"));
    const QString source = QStringLiteral(
        "int turtle_code_symbol = 1;\n"
        "int main() { return turtle_code_symbol; }\n"
        "void local() { int turtle_code_symbol = 2; (void)turtle_code_symbol; }\n"
        "void pin_set(int gpio, int state) { (void)gpio; (void)state; }\n");
    QFile file(path);
    Q_ASSERT(file.open(QIODevice::WriteOnly));
    Q_ASSERT(file.write(source.toUtf8()) == source.toUtf8().size());
    file.close();

    LspClient client;
    client.setRoot(directory.path());
    QSignalSpy diagnostics(&client, &LspClient::diagnostics);
    client.open(path, source);
    Q_ASSERT(waitUntil([&] { return !diagnostics.isEmpty(); }));
    Q_ASSERT(diagnostics.first().at(0).toString() == path);

    Q_ASSERT(client.documentSymbolsAvailable());
    bool outlined=false;
    client.documentSymbols(path,[&](QJsonValue value) {
        Q_ASSERT(value.isArray());
        bool found=false;
        for(const auto& item:value.toArray()) {
            const auto symbol=item.toObject();
            if(symbol.value("name").toString()=="pin_set") {
                Q_ASSERT(symbol.value("kind").toInt()==12);
                Q_ASSERT(symbol.value("selectionRange").toObject().value("start").toObject().value("line").toInt()==3);
                found=true;
            }
        }
        Q_ASSERT(found); outlined=true;
    });
    Q_ASSERT(waitUntil([&]{return outlined;}));

    bool defined = false;
    QString definitionPath;
    int definitionLine = -1;
    client.definition(path, 1, 24, [&](QJsonValue value) {
        if (value.isArray()) {
            const auto array = value.toArray();
            value = array.isEmpty() ? QJsonValue{} : array.at(0);
        }
        const auto location = value.toObject();
        definitionPath = QUrl(location.value(QStringLiteral("uri")).toString()).toLocalFile();
        definitionLine = location.value(QStringLiteral("range")).toObject()
            .value(QStringLiteral("start")).toObject().value(QStringLiteral("line")).toInt(-1);
        defined = true;
    });
    Q_ASSERT(waitUntil([&] { return defined; }));
    Q_ASSERT(definitionPath == path && definitionLine == 0);

    bool completed = false;
    int itemCount = 0;
    client.completion(path, 1, 25, [&](QJsonValue value) {
        itemCount = value.isArray() ? value.toArray().size()
            : value.toObject().value(QStringLiteral("items")).toArray().size();
        completed = true;
    });
    Q_ASSERT(waitUntil([&] { return completed; }));
    Q_ASSERT(itemCount > 0);
    Q_ASSERT(client.canRename());
    bool prepared = false;
    client.prepareRename(path, 1, 24, [&](QJsonValue value) { Q_ASSERT(value.isObject()); prepared = true; });
    Q_ASSERT(waitUntil([&] { return prepared; }));
    bool renamed = false;
    client.renameSymbol(path, 1, 24, "renamed_symbol", [&](QJsonValue value) {
        Q_ASSERT(value.isObject());
        const auto workspace = value.toObject();
        int count = 0;
        if (workspace.contains("changes")) {
            const auto changes = workspace.value("changes").toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it) count += it.value().toArray().size();
        } else for (const auto& document : workspace.value("documentChanges").toArray()) count += document.toObject().value("edits").toArray().size();
        Q_ASSERT(count == 2); renamed = true;
    });
    Q_ASSERT(waitUntil([&] { return renamed; }));
    Q_ASSERT(client.semanticTokensAvailable());
    bool semanticDone=false;
    client.semanticTokens(path,[&](QJsonValue value) {
        const auto object=value.toObject();
        const auto result=SemanticHighlighting::decode(object.value("data").toArray(),object.value("legend").toObject(),source);
        Q_ASSERT(result.valid && !result.lines.isEmpty());
        int parameters=0;
        for(const auto& row:result.lines) for(const auto& token:row) {
            Q_ASSERT(!token.type.isEmpty());
            if(token.line==3 && token.type=="parameter") ++parameters;
        }
        Q_ASSERT(parameters==4); // Both declarations and uses are resolved by clangd.
        semanticDone=true;
    });
    Q_ASSERT(waitUntil([&] { return semanticDone; }));
    bool referencesDone=false;
    client.references(path,1,24,[&](QJsonValue value) {
        // Same spelling in the local scope is a different symbol.
        Q_ASSERT(value.toArray().size()==2); referencesDone=true;
    });
    Q_ASSERT(waitUntil([&]{return referencesDone;}));
    const QString navigationSource="struct Kind {};\nextern Kind item;\nKind item;\nKind use() { return item; }\n";
    client.change(path,navigationSource);
    bool declarationDone=false;
    client.declaration(path,3,21,[&](QJsonValue value) {
        if(value.isArray()) value=value.toArray().first();
        const auto object=value.toObject();
        const auto range=object.contains("targetUri")?object.value("targetSelectionRange"):object.value("range");
        Q_ASSERT(range.toObject().value("start").toObject().value("line").toInt(-1)==1);
        declarationDone=true;
    });
    Q_ASSERT(waitUntil([&]{return declarationDone;}));
    bool typeDone=false;
    client.typeDefinition(path,3,21,[&](QJsonValue value) {
        if(value.isArray()) value=value.toArray().first();
        const auto object=value.toObject();
        const auto range=object.contains("targetUri")?object.value("targetSelectionRange"):object.value("range");
        Q_ASSERT(range.toObject().value("start").toObject().value("line").toInt(-1)==0);
        typeDone=true;
    });
    Q_ASSERT(waitUntil([&]{return typeDone;}));
    // Exercise the actual installed clangd formatter, not only the fake peer.
    client.change(path,"int main(){return 0;}\n");
    Q_ASSERT(client.canFormat());
    bool formatted=false;
    client.formatDocument(path,4,true,[&](QJsonValue result){
        Q_ASSERT(result.isArray() && !result.toArray().isEmpty());formatted=true;
    });
    Q_ASSERT(waitUntil([&]{return formatted;}));
    client.close(path);
    return 0;
}
