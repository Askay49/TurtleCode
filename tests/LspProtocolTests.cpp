#include "language/LspClient.hpp"
#include "language/SemanticHighlighting.hpp"
#include "language/LspAdaptivePolicy.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QThread>
#include <cassert>
static bool waitFor(const std::function<bool()>& predicate, int timeout=5000) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed()<timeout) { QCoreApplication::processEvents(); QThread::msleep(5); }
    return predicate();
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    {
        const QJsonObject legend{{"tokenTypes",QJsonArray{"parameter","type"}}, {"tokenModifiers",QJsonArray{"declaration","deprecated"}}};
        const QString text=QString::fromUtf8("имя 😀\r\nnext\n");
        const auto result=SemanticHighlighting::decode(QJsonArray{0,0,3,0,1, 0,4,2,1,2, 1,0,4,0,0},legend,text);
        assert(result.valid && result.lines.size()==2);
        assert(result.lines[0][1].start==4 && result.lines[0][1].length==2);
        assert(result.lines[0][1].modifiers==QStringList{"deprecated"});
        auto invalid=[&](QJsonArray data) { assert(!SemanticHighlighting::decode(data,legend,text).valid); };
        invalid({0,0,1}); // Truncated token tuple.
        invalid({0,-1,1,0,0}); invalid({0,0.5,1,0,0});
        invalid({0,0,0,0,0}); invalid({0,0,1,2,0}); invalid({0,0,1,0,4});
        invalid({3,0,1,0,0}); invalid({0,6,1,0,0}); // CRLF is outside the line text.
        invalid({0,4,1,0,0}); invalid({0,5,1,0,0}); // Neither half of an emoji is a valid boundary.
        invalid({0,0,3,0,0, 0,2,1,1,0}); // Unnegotiated overlap.
        auto shifted=result.lines; SemanticHighlighting::applyEdit(shifted,0,0,2);
        assert(shifted.size()==1 && shifted[3][0].line==3);
        SemanticHighlighting::applyEdit(shifted,1,2,0); assert(shifted.isEmpty());
        assert(SemanticHighlighting::decode({},legend,text).valid);
    }
    {
        LspAdaptivePolicy policy; using Mode=LspAdaptivePolicy::Mode;
        assert(policy.mode(1000,true,true,0)==Mode::Full);
        assert(policy.mode(3*LspAdaptivePolicy::MiB,true,true,0)==Mode::Range);
        assert(policy.mode(3*LspAdaptivePolicy::MiB,false,true,0)==Mode::None);
        assert(policy.mode(1000,true,false,0)==Mode::Range);
        assert(LspAdaptivePolicy::transportFits(9*LspAdaptivePolicy::MiB));
        assert(!LspAdaptivePolicy::automaticDocument(17*LspAdaptivePolicy::MiB));
        policy.observe(800,100,true,1000);
        assert(policy.delay(1000)>=2000 && policy.mode(1000,true,true,1001)==Mode::Range);
        policy.observe(900,100,true,2000); policy.observe(800,100,true,3000);
        assert(policy.mode(1000,true,true,3001)==Mode::None);
        assert(policy.mode(1000,true,true,34000)==Mode::Range);
        for(int i=0;i<10;++i) policy.observe(20,10,true,34000+i);
        assert(policy.automaticFeatures(1000,35000));
        policy.reset(); policy.observe(10,200000,true,0);
        assert(policy.mode(1000,true,true,1)==Mode::None);
    }
    QTemporaryDir directory; assert(directory.isValid());
    LspServerConfig config; config.name="Fake server"; config.command=QStandardPaths::findExecutable("python3");
    assert(!config.command.isEmpty()); config.arguments={LSP_FAKE_SERVER}; config.languageIds={{"xyz", "custom"}};
    LspClient client(config); client.setRoot(directory.path());
    assert(client.supports("test.xyz")); assert(!client.supports("test.cpp"));
    QSignalSpy errors(&client, &LspClient::error), diagnostics(&client, &LspClient::diagnostics);
    const auto path=directory.filePath("unicode.xyz");
    client.open(path, QString::fromUtf8("первая 😀\nвторая\n"));
    client.change(path, QString::fromUtf8("первая 😁\nизменено\n"));
    assert(waitFor([&] { return client.ready(); }));
    auto checkText = [&](const QString& expected, bool saved=false) {
        bool done=false; QJsonObject result;
        client.hover(path, 0, 0, [&](QJsonValue value) { result=value.toObject(); done=true; });
        assert(waitFor([&] { return done; }));
        assert(result.value("contents").toObject().value("value").toString()==expected);
        if (saved) assert(result.value("configured").toBool());
        if (saved) assert(result.value("saved").toBool());
    };
    checkText(QString::fromUtf8("первая 😁\nизменено\n"));
    for (const auto& method : {QString("declaration"), QString("typeDefinition"), QString("references")}) {
        bool received=false;
        auto callback=[&](QJsonValue value) {
            if (method=="declaration") assert(value.toArray().first().toObject().contains("targetSelectionRange"));
            else if (method=="references") assert(value.toArray().size()==2);
            else assert(value.toObject().value("range").toObject().value("end").toObject().value("character").toInt()==3);
            received=true;
        };
        if (method=="declaration") client.declaration(path,0,5,callback);
        else if (method=="typeDefinition") client.typeDefinition(path,0,5,callback);
        else client.references(path,0,5,callback);
        assert(waitFor([&]{return received;}));
    }

    assert(client.canFormat());
    bool formatted=false;
    client.formatDocument(path,4,true,[&](QJsonValue value){assert(value.isArray());formatted=true;});
    assert(waitFor([&]{return formatted;}));
    int staleFormat=0;
    client.formatDocument(path,4,true,[&](QJsonValue){++staleFormat;});
    client.change(path,QString::fromUtf8("первая 😁\nизменено\n "));
    QThread::msleep(180);QCoreApplication::processEvents();assert(staleFormat==0);
    client.change(path,QString::fromUtf8("первая 😁\nизменено\n"));
    assert(client.semanticTokensAvailable());
    bool highlighted=false;
    client.semanticTokens(path,[&](QJsonValue value) {
        const auto object=value.toObject();
        const auto result=SemanticHighlighting::decode(object.value("data").toArray(),object.value("legend").toObject(),QString::fromUtf8("первая 😁\nизменено\n"));
        assert(result.valid && result.lines.size()==2);
        assert(result.lines[0].first().type=="parameter");
        highlighted=true;
    });
    assert(waitFor([&] { return highlighted; }));
    const QString changed=QString::fromUtf8("первая 😎!\nещё одна строка\n");
    client.change(path, changed); client.saved(path); checkText(changed, true);
    int staleCallbacks=0;
    client.completion(path, 0, 0, [&](QJsonValue) { ++staleCallbacks; });
    client.semanticTokens(path,[&](QJsonValue) { ++staleCallbacks; });
    client.change(path, changed+"x");
    QThread::msleep(180); QCoreApplication::processEvents(); assert(staleCallbacks==0);
    bool done=false; client.completion(path, 0, 0, [&](QJsonValue value) { assert(value.toArray().first().toObject().value("label").toString()==changed+"x"); done=true; });
    assert(waitFor([&] { return done; }));
    const auto version=client.documentVersion(path); client.open(path, changed+"x"); assert(client.documentVersion(path)==version);
    diagnostics.clear(); checkText(changed+"x");
    assert(waitFor([&] { return !diagnostics.isEmpty(); }));
    for (const auto& signal:diagnostics) for (const auto& value:signal[1].toJsonArray()) assert(value.toObject().value("message").toString()!="stale");
    assert(errors.isEmpty());
    client.restart(); assert(waitFor([&] { return client.ready(); })); checkText(changed+"x");
    client.close(path); assert(client.documentVersion(path)==0);
    {
        auto rangedConfig=config; rangedConfig.arguments={LSP_FAKE_SERVER,"--range-only"};
        LspClient ranged(rangedConfig); ranged.open(path,QString::fromUtf8("first\nимя 😀\nthird\n"));
        assert(waitFor([&]{return ranged.ready();}));
        assert(ranged.semanticTokensRangeAvailable() && !ranged.semanticTokensFullAvailable());
        bool rangeDone=false;
        const QJsonObject range{{"start",QJsonObject{{"line",1},{"character",0}}},{"end",QJsonObject{{"line",1},{"character",6}}}};
        ranged.semanticTokens(path,[&](QJsonValue value) {
            const auto object=value.toObject();
            const auto result=SemanticHighlighting::decode(object.value("data").toArray(),object.value("legend").toObject(),QString::fromUtf8("имя 😀"),1,20);
            assert(result.valid && result.lines.size()==1 && result.lines.contains(1)); rangeDone=true;
        },range);
        assert(waitFor([&]{return rangeDone;}));
        assert(!SemanticHighlighting::decode(QJsonArray{1,0,3,0,0},QJsonObject{{"tokenTypes",QJsonArray{"parameter"}}},QString::fromUtf8("имя"),1,0).valid);
        ranged.close(path);
    }
    {
        auto unsupportedConfig=config;unsupportedConfig.arguments={LSP_FAKE_SERVER,"--no-format"};
        LspClient unsupported(unsupportedConfig);unsupported.open(path,"int value;");
        assert(waitFor([&]{return unsupported.ready();}));assert(!unsupported.canFormat());
        bool rejected=false;unsupported.formatDocument(path,4,true,[&](QJsonValue value){assert(value.isUndefined());rejected=true;});
        assert(rejected);
    }
    config.arguments={LSP_FAKE_SERVER, "--init-error"};
    LspClient failed(config); failed.open(path, "text");
    assert(waitFor([&] { return failed.status()=="Server initialization failed"; })); assert(!failed.ready());
}
