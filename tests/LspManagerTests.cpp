#include "language/LspManager.hpp"
#include "language/LanguageRegistry.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QSettings>
#include <QThread>
#include <QFile>
#include <QDir>
#include <cassert>

static bool waitFor(const std::function<bool()>& predicate,int timeout=8000) {
    QElapsedTimer timer;timer.start();
    while(!predicate() && timer.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(5);}return predicate();
}
int main(int argc,char** argv) {
    QTemporaryDir directory;assert(directory.isValid());qputenv("XDG_CONFIG_HOME",directory.path().toUtf8());
    QCoreApplication app(argc,argv);app.setOrganizationName("TurtleCodeTests");app.setApplicationName("LspManagerTests");
    const auto python=QStandardPaths::findExecutable("python3");assert(!python.isEmpty());
    auto config=LspConfiguration::load();assert(config.bindings.value("c")=="clangd" && config.bindings.value("cpp")=="clangd");
    config.profiles[0].server.command=python;config.profiles[0].server.arguments={LSP_FAKE_SERVER,"--any-language"};
    auto other=config.profiles[0];other.id="other";other.server.name="Other";
    other.server.initializationOptions={{"example",42}};
    other.server.settings={{"custom",QJsonObject{{"nested",QJsonObject{{"value",7}}}}}};
    config.profiles.append(other);
    for(const auto& language:LanguageRegistry::languages())if(language.id!="c" && language.id!="cpp")config.bindings[language.id]="other";
    config.save();assert(LspConfiguration::load().json()==config.json());
    LspManager manager;manager.setRoot(directory.path());manager.configure(config);
    const auto cpp=directory.filePath("main.cpp"),py=directory.filePath("main.py");
    manager.open(cpp,"int a;");manager.open(py,"first 😀");
    assert(waitFor([&]{return manager.ready(cpp)&&manager.ready(py);}));
    auto* c=manager.clientFor(cpp);auto* p=manager.clientFor(py);assert(c!=p);
    const auto cHeader=directory.filePath("other.h");manager.open(cHeader,"int a;");assert(manager.clientFor(cHeader)==c);
    auto inspect=[&](const QString& path,const QString& text,const QString& language) {
        manager.open(path,text);assert(waitFor([&]{return manager.ready(path);}));
        QJsonObject result; QElapsedTimer deadline;deadline.start();
        do {
            bool done=false;
            manager.hover(path,0,0,[&](QJsonValue value){result=value.toObject();done=true;});
            assert(waitFor([&]{return done;}));
            assert(deadline.elapsed()<8000);
        } while(!result.value("configured").toBool());
        assert(result.value("contents").toObject().value("value").toString()==text);
        assert(result.value("languageId").toString()==language);
        if(manager.profileFor(path)=="other") {
            assert(result.value("configuration").toArray().first().toObject().value("nested").toObject().value("value").toInt()==7);
            assert(result.value("configuration").toArray().at(1).toObject().value("value").toInt()==7);
            assert(result.value("configuration").toArray().at(2).toObject()==other.server.settings);
            assert(result.value("configuration").toArray().at(3).toObject().isEmpty());
            assert(result.value("initialization").toObject().value("initializationOptions").toObject().value("example").toInt()==42);
        }
    };
    inspect(py,"first 😀","python");inspect(cpp,"int a;","cpp");
    for(const auto& language:LanguageRegistry::languages()) {
        QString path=directory.filePath("example."+language.extensions.value(0));
        if(language.id=="cmake")path=directory.filePath("CMakeLists.txt");
        if(language.id=="make")path=directory.filePath("Makefile");
        if(language.id=="meson")path=directory.filePath("meson.build");
        if(language.id=="dockerfile")path=directory.filePath("Dockerfile");
        if(language.id=="nginx")path=directory.filePath("nginx.conf");
        assert(manager.supports(path));
    }
    inspect(directory.filePath("CMakeLists.txt"),"project(a)","cmake");
    inspect(directory.filePath("Makefile"),"all:","make");
    inspect(directory.filePath("Dockerfile"),"FROM example","dockerfile");
    inspect(directory.filePath(".editorconfig"),"root=true","editorconfig");
    inspect(directory.filePath("view.jsx"),"const x=1","javascriptreact");
    inspect(directory.filePath("view.tsx"),"const x=1","typescriptreact");
    assert(manager.documentVersionForServer(cpp,py)==0);
    manager.change(py,"second");assert(manager.documentVersion(py)==2 && manager.documentVersion(cpp)==1);
    manager.restart("other");assert(waitFor([&]{return manager.ready(py);}));assert(manager.ready(cpp));inspect(py,"second","python");
    config.profiles[1].server.logLevel=2;manager.configure(config);
    assert(manager.clientFor(py)==p && manager.ready(py)); // Logging changes preserve the process and versions.
    config.profiles[1].server.logLevel=1;manager.configure(config);
    QTemporaryDir second;assert(second.isValid());manager.setRoot(second.path());
    const auto another=second.filePath("main.py");manager.open(another,"another root");assert(waitFor([&]{return manager.ready(another);}));
    assert(manager.clientFor(another)!=p);assert(manager.clientFor(py)==p && manager.ready(py));
    manager.close(py);assert(!manager.ready(py)&&manager.documentVersion(py)==0);assert(manager.ready(cpp));
    config.profiles[1].enabled=false;manager.configure(config);assert(!manager.supports(py)&&manager.supports(cpp));
    config.profiles[1].enabled=true;config.languageIds["python"]="custom-python";manager.configure(config);assert(manager.protocolLanguage(py)=="custom-python");
    {
        LspServerConfig server;server.command=python;server.arguments={LSP_FAKE_SERVER,"--log-flood"};server.languageIds={{"xyz","custom"}};
        LspClient client(server);client.open(directory.filePath("logs.xyz"),"SECRET_DOCUMENT_TEXT");assert(waitFor([&]{return client.ready();}));
        bool done=false;client.hover(directory.filePath("logs.xyz"),0,0,[&](QJsonValue){done=true;});
        assert(waitFor([&]{return done&&client.log().contains("stderr-tail");}));
        assert(client.log().toUtf8().size()<=LspClient::NormalLogBytes);
        assert(client.log().contains("PRESERVED_TEST_ERROR"));assert(!client.log().contains("SECRET_DOCUMENT_TEXT"));
        client.setLogLevel(2);done=false;client.hover(directory.filePath("logs.xyz"),0,0,[&](QJsonValue){done=true;});
        assert(waitFor([&]{return done&&client.log().contains("SECRET_DOCUMENT_TEXT");}));
        assert(client.log().toUtf8().size()<=LspClient::DetailedLogBytes);
        client.setLogLevel(1);assert(!client.log().contains("SECRET_DOCUMENT_TEXT"));
        client.clearLog();assert(client.log().isEmpty());client.setLogLevel(0);client.restart();assert(waitFor([&]{return client.ready();}));assert(client.log().isEmpty());
    }
    {
        auto server=other.server;server.arguments={LSP_FAKE_SERVER,"--any-language"};
        LspClient probe(server);probe.setRoot(directory.path());probe.checkConnection();assert(waitFor([&]{return probe.ready();}));assert(probe.canFormat());
    }
}
