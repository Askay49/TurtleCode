#pragma once
#include "DebugJson.hpp"
#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QVersionNumber>
#include <functional>
#include <algorithm>

struct DebugBuildStep { QStringList command; QString cwd; QJsonObject env; };

// Translate the supported VS Code configuration into the existing DAP runner.
// No .vscode files or VS Code user settings are read implicitly.
class DebugProject {
public:
    static QString path(const QString& root,const QString& file) {return QDir(root).filePath(".turtle/"+file);}
    static QStringList names(const QString& root,QString& error) {
        QJsonObject launch;if(!DebugJson::read(path(root,"launch.json"),launch,error))return {};
        if(launch.value("version").toString()!="0.2.0" || !launch.value("configurations").isArray()) {error=path(root,"launch.json")+": Expected version 0.2.0 and configurations array";return {};}
        QStringList names;
        for(const auto& item:launch.value("configurations").toArray()) {
            const auto name=item.toObject().value("name").toString();
            if(name.isEmpty() || names.contains(name)) {error=path(root,"launch.json")+": Configuration names must be non-empty and unique";return {};}
            names.append(name);
        }
        if(names.isEmpty())error=path(root,"launch.json")+": No debug configurations";
        return names;
    }
    // Resolve installed adapters at runtime; projects need no versioned extension path.
    static QString extension(const QString& prefix,const QString& relative) {
        QString best;QVersionNumber version;
        const auto home=QDir::homePath();
        for(const auto& base:{home+"/.vscode/extensions",home+"/.vscode-oss/extensions",home+"/.vscode-insiders/extensions"}) {
            for(const auto& entry:QDir(base).entryList({prefix+"*"},QDir::Dirs|QDir::NoDotAndDotDot)) {
                const auto folder=QDir(base).filePath(entry);
                if(!QFileInfo::exists(folder+"/"+relative))continue;
                const auto v=QVersionNumber::fromString(entry.mid(prefix.size()));
                if(best.isEmpty() || QVersionNumber::compare(v,version)>0) {best=folder;version=v;}
            }
        }
        return best;
    }
    static QProcessEnvironment environment(const QJsonObject& changes) {
        auto env=QProcessEnvironment::systemEnvironment();
        for(auto it=changes.begin();it!=changes.end();++it) {if(it.value().isNull())env.remove(it.key());else env.insert(it.key(),it.value().toString());}
        return env;
    }
    DebugProject(QString root,QString file):root_(QDir(root).absolutePath()),file_(std::move(file)){}
    bool load(const QString& selected,QJsonObject& normalized,QVector<DebugBuildStep>& steps,QString& error) {
        error_=&error;
        QJsonObject launch;
        if(!DebugJson::read(path(root_,"launch.json"),launch,error) || !DebugJson::read(path(root_,"settings.json"),settings_,error,true))return false;
        const auto available=names(root_,error);if(!error.isEmpty())return false;
        const auto choice=selected.isEmpty()?available.first():selected;
        if(!available.contains(choice))return fail("Debug configuration not found: "+choice);
        QJsonObject config;
        for(const auto& item:launch.value("configurations").toArray())if(item.toObject().value("name").toString()==choice)config=item.toObject();
        config=platform(config);
        config=expand(config).toObject();if(!error.isEmpty())return false;
        for(const auto& key:{"name","type","request","program","executable","cwd","preLaunchTask"})
            if(config.contains(key) && !config.value(key).isString())return fail(QString(key)+" must be a string");
        const auto kind=config.value("type").toString();
        const auto request=config.value("request").toString();
        if(request!="launch" && request!="attach")return fail("request must be launch or attach");
        for(const auto& key:{"postDebugTask","debugServer","pipeTransport","sourceFileMap","envFile"})
            if(config.contains(key))return fail(QString("Unsupported debug setting: ")+key);
        const auto task=config.value("preLaunchTask");
        if(!task.isUndefined()) {
            if(!task.isString())return fail("preLaunchTask must be a task label");
            if(!readTasks(task.toString(),steps))return false;
        }
        QJsonObject result{{"version",1},{"cwd",config.value("cwd").toString(root_)},{"args",config.value("args").toArray()}};
        if(config.contains("args") && !config.value("args").isArray())return fail("args must be an array");
        const auto program=config.value(kind=="cortex-debug"?"executable":"program").toString();
        if(program.isEmpty())return fail("A program/executable file is required for this adapter");
        result["program"]=program;
        QJsonArray commands;for(const auto& step:steps)commands.append(QJsonArray::fromStringList(step.command));
        if(!commands.isEmpty())result["preLaunch"]=commands;
        if(config.contains("env")) {if(!validEnv(config.value("env")))return false;result["env"]=config.value("env");}
        if(kind=="python" || kind=="debugpy") {
            if(request!="launch")return fail("Python attach is not supported yet");
            if(config.contains("module") || config.contains("pythonArgs"))return fail("Python module/pythonArgs launches are not supported yet");
            const auto python=expand(config.value("python").toString(settings_.value("python.defaultInterpreterPath").toString("python3"))).toString();
            if(config.contains("python") && !config.value("python").isString())return fail("python must be an interpreter path");
            result["type"]="dap";result["adapter"]=python;result["adapterID"]="debugpy";result["adapterArgs"]=QJsonArray{"-m","debugpy.adapter"};
            auto args=config;for(const auto& key:{"name","preLaunchTask","presentation","linux","windows","osx"})args.remove(key);
            args["python"]=QJsonArray{python};args["type"]="debugpy";
            if(!args.contains("console"))args["console"]="integratedTerminal";
            if(!args.contains("justMyCode"))args["justMyCode"]=true;
            result["launchArguments"]=args;
        } else {
            result["type"]="dap";result["adapterID"]=kind;result["request"]=request;
            QJsonObject args=config;
            for(const auto& key:{"name","preLaunchTask","presentation","internalConsoleOptions","env","linux","windows","osx"})args.remove(key);
            if(kind=="cortex-debug") {
                const auto folder=extension("marus25.cortex-debug-","dist/debugadapter.js");
                if(folder.isEmpty())return fail("Cortex-Debug is not installed (marus25.cortex-debug)");
                result["adapter"]="node";result["adapterArgs"]=QJsonArray{folder+"/dist/debugadapter.js"};
                if(args.value("swoConfig").toObject().value("enabled").toBool() || args.value("rttConfig").toObject().value("enabled").toBool()
                    || args.value("chainedConfigurations").toObject().value("enabled").toBool() || !args.value("graphConfig").toArray().isEmpty())return fail("SWO, RTT, graphs and chained debug sessions are not supported yet");
                const auto server=args.value("servertype").toString();if(server!="openocd" && server!="external")return fail("Supported Cortex-Debug servertype values: openocd, external");
                if(server=="openocd" && (!args.value("configFiles").isArray() || args.value("configFiles").toArray().isEmpty()))return fail("OpenOCD configFiles must be a non-empty array");
                auto setting=[&](const QString& name,const QString& fallback=QString{}) {
                    return expand(settings_.value(name+".linux").toString(settings_.value(name).toString(fallback))).toString();
                };
                args["extensionPath"]=folder;
                QJsonObject package;QString ignored;DebugJson::read(folder+"/package.json",package,ignored);
                args["pvtVersion"]=package.value("version").toString();
                args["gdbServerConsolePort"]="${serverConsolePort}";
                if(!args.contains("toolchainPath"))args["toolchainPath"]=setting("cortex-debug.armToolchainPath");
                if(!args.contains("toolchainPrefix"))args["toolchainPrefix"]=setting("cortex-debug.armToolchainPrefix","arm-none-eabi");
                if(!args.contains("gdbPath"))args["gdbPath"]=setting("cortex-debug.gdbPath");
                if(args.value("gdbPath").toString().isEmpty())args.remove("gdbPath");
                if(server=="openocd" && !args.contains("serverpath"))args["serverpath"]=setting("cortex-debug.openocdPath","openocd");
                if(!args.contains("searchDir"))args["searchDir"]=QJsonArray{root_};
                args["swoConfig"]=QJsonObject{{"enabled",false},{"decoders",QJsonArray{}},{"cpuFrequency",0},{"swoFrequency",0},{"source","probe"}};
                args["rttConfig"]=QJsonObject{{"enabled",false},{"decoders",QJsonArray{}}};
                args["graphConfig"]=QJsonArray{};
                for(const auto& key:{"preLaunchCommands","postLaunchCommands","preAttachCommands","postAttachCommands","preRestartCommands","postRestartCommands","preResetCommands","postResetCommands","postStartSessionCommands","postRestartSessionCommands","openOCDLaunchCommands","openOCDPreConfigLaunchCommands"})
                    if(!args.contains(key))args[key]=QJsonArray{};
                for(const auto& key:{"configFiles","searchDir","preLaunchCommands","postLaunchCommands","preAttachCommands","postAttachCommands","preRestartCommands","postRestartCommands","preResetCommands","postResetCommands","postStartSessionCommands","postRestartSessionCommands","openOCDLaunchCommands","openOCDPreConfigLaunchCommands"}) {
                    if(args.contains(key) && !args.value(key).isArray())return fail(QString(key)+" must be an array of strings");
                    for(const auto& value:args.value(key).toArray())if(!value.isString())return fail(QString(key)+" must contain strings");
                }
                if(!args.contains("runToEntryPoint"))args["runToEntryPoint"]="main";
                for(const auto& key:{"variableUseNaturalFormat","registerUseNaturalFormat"})
                    if(!args.contains(key))args[key]=settings_.value(QString("cortex-debug.")+key).toBool(true);
                if(args.value("showDevDebugOutput").toString()=="none")args.remove("showDevDebugOutput");
            } else if(kind=="lldb") {
                result["adapter"]="lldb-dap";
            } else if(kind=="cppdbg") {
                const auto folder=extension("ms-vscode.cpptools-","debugAdapters/bin/OpenDebugAD7");
                if(folder.isEmpty())return fail("C/C++ debug adapter is not installed (ms-vscode.cpptools)");
                result["adapter"]=folder+"/debugAdapters/bin/OpenDebugAD7";
            } else return fail("Unsupported debug type: "+kind);
            result["launchArguments"]=args;
        }
        if(!error.isEmpty())return false;
        auto args=result.value("launchArguments").toObject();
        args["cwd"]=QDir(root_).absoluteFilePath(result.value("cwd").toString());
        if(kind=="cortex-debug") {args["executable"]=QDir(args.value("cwd").toString()).absoluteFilePath(program);result["program"]=args["executable"];}
        result["launchArguments"]=args;
        normalized=result;return true;
    }
private:
    bool fail(const QString& message) {if(error_->isEmpty())*error_=path(root_,"launch.json")+": "+message;return false;}
    static QJsonObject platform(QJsonObject object) {const auto values=object.take("linux").toObject();object.remove("osx");object.remove("windows");for(auto it=values.begin();it!=values.end();++it)object[it.key()]=it.value();return object;}
    QJsonValue expand(QJsonValue value,int depth=0) {
        if(depth>20) {fail("Variable expansion is recursive");return {};}
        if(value.isArray()) {QJsonArray result;for(const auto& item:value.toArray())result.append(expand(item,depth+1));return result;}
        if(value.isObject()) {auto result=value.toObject();for(auto it=result.begin();it!=result.end();++it)it.value()=expand(it.value(),depth+1);return result;}
        if(!value.isString())return value;
        const auto text=value.toString();QString out;int last=0;
        static const QRegularExpression pattern("\\$\\{([^}]+)\\}");auto matches=pattern.globalMatch(text);
        while(matches.hasNext()) {
            const auto match=matches.next();out+=text.mid(last,match.capturedStart()-last);const auto key=match.captured(1);QString replacement;
            if(key=="workspaceFolder" || key=="workspaceRoot" || key=="cwd" || key=="fileWorkspaceFolder")replacement=root_;
            else if(key=="workspaceFolderBasename")replacement=QFileInfo(root_).fileName();
            else if(key=="userHome")replacement=QDir::homePath();
            else if(key=="pathSeparator" || key=="/")replacement=QString(QDir::separator());
            else if(key.startsWith("env:")) {const auto name=key.mid(4);if(!qEnvironmentVariableIsSet(name.toUtf8().constData())){fail("Environment variable is not set: "+name);return {};}replacement=qEnvironmentVariable(name.toUtf8().constData());}
            else if(key.startsWith("config:")) {const auto name=key.mid(7);if(!settings_.contains(name)){fail("Setting not found: "+name);return {};}const auto setting=expand(settings_.value(name),depth+1);if(!setting.isString()){fail("Variable requires a string setting: "+name);return {};}replacement=setting.toString();}
            else if(key.startsWith("file") || key.startsWith("relativeFile")) {
                if(file_.isEmpty()) {fail("Open a source file to use ${"+key+"}");return {};}
                const QFileInfo file(file_);
                if(key=="file")replacement=file.absoluteFilePath();else if(key=="fileDirname")replacement=file.absolutePath();
                else if(key=="fileBasename")replacement=file.fileName();else if(key=="fileBasenameNoExtension")replacement=file.completeBaseName();
                else if(key=="fileExtname")replacement=file.suffix().isEmpty()?QString{}:"."+file.suffix();
                else if(key=="relativeFile")replacement=QDir(root_).relativeFilePath(file.absoluteFilePath());
                else if(key=="relativeFileDirname")replacement=QFileInfo(QDir(root_).relativeFilePath(file.absoluteFilePath())).path();
                else {fail("Unsupported variable: ${"+key+"}");return {};}
            } else {fail("Unsupported variable: ${"+key+"}");return {};}
            out+=replacement;last=match.capturedEnd();
        }
        out+=text.mid(last);return out;
    }
    bool validEnv(QJsonValue value) {
        if(!value.isObject())return fail("env must be an object");
        const auto env=value.toObject();for(auto it=env.begin();it!=env.end();++it)if(!it.value().isString() && !it.value().isNull())return fail("Environment values must be strings or null");return true;
    }
    static QString quote(QString value) {value.replace("'","'\\''");return "'"+value+"'";}
    bool readTasks(const QString& label,QVector<DebugBuildStep>& steps) {
        QJsonObject tasks;if(!DebugJson::read(path(root_,"tasks.json"),tasks,*error_))return false;
        if(tasks.value("version").toString()!="2.0.0" || !tasks.value("tasks").isArray())return fail("tasks.json requires version 2.0.0 and tasks array");
        QMap<QString,QJsonObject> byName;
        for(const auto& item:tasks.value("tasks").toArray()) {const auto task=platform(item.toObject());const auto name=task.value("label").toString();if(name.isEmpty() || byName.contains(name))return fail("Task labels must be non-empty and unique");byName[name]=task;}
        QSet<QString> visiting,done;
        std::function<bool(QString)> visit=[&](QString name) {
            if(done.contains(name))return true;
            if(visiting.contains(name))return fail("Task dependency cycle: "+name);
            if(!byName.contains(name))return fail("Task not found: "+name);
            if(visiting.size()>64 || done.size()>256)return fail("Too many task dependencies");
            visiting.insert(name);auto task=byName[name];
            if(task.value("isBackground").toBool())return fail("Background preLaunchTask is not supported: "+name);
            QJsonArray dependencies;const auto depends=task.value("dependsOn");
            if(depends.isString())dependencies.append(depends);else if(depends.isArray())dependencies=depends.toArray();else if(!depends.isUndefined())return fail("dependsOn must be a label or array of labels");
            if(dependencies.size()>1 && task.value("dependsOrder").toString("parallel")!="sequence")return fail("Parallel task dependencies are not supported; use dependsOrder: sequence");
            for(const auto& dep:dependencies) {if(!dep.isString())return fail("dependsOn entries must be task labels");if(!visit(dep.toString()))return false;}
            task=expand(task).toObject();if(!error_->isEmpty())return false;
            const auto command=task.value("command");
            if(command.isUndefined()) {if(dependencies.isEmpty())return fail("Task has no command or dependencies: "+name);}
            else {
                if(!command.isString() || command.toString().isEmpty())return fail("Task command must be a non-empty string: "+name);
                const auto type=task.value("type").toString(tasks.value("type").toString("shell"));
                if(type!="process" && type!="shell")return fail("Unsupported task type: "+type);
                if((tasks.contains("options") && !tasks.value("options").isObject()) || (task.contains("options") && !task.value("options").isObject()))return fail("Task options must be an object");
                auto options=tasks.value("options").toObject();const auto local=task.value("options").toObject();
                for(auto it=local.begin();it!=local.end();++it) {
                    if(it.key()=="env" && options.value("env").isObject() && it.value().isObject()) {auto env=options.value("env").toObject();const auto values=it.value().toObject();for(auto v=values.begin();v!=values.end();++v)env[v.key()]=v.value();options["env"]=env;}
                    else options[it.key()]=it.value();
                }
                options=expand(options).toObject();if(!error_->isEmpty())return false;
                if(options.contains("cwd") && !options.value("cwd").isString())return fail("Task cwd must be a string");
                if(options.contains("shell") && !options.value("shell").isObject())return fail("Task shell must be an object");
                DebugBuildStep step;step.cwd=QDir(root_).absoluteFilePath(options.value("cwd").toString(root_));
                if(!QDir(step.cwd).exists())return fail("Task working directory does not exist: "+step.cwd);
                if(options.contains("env")) {if(!validEnv(options.value("env")))return false;step.env=options.value("env").toObject();}
                if(task.contains("args") && !task.value("args").isArray())return fail("Task args must be an array");
                QStringList arguments;
                for(const auto& arg:task.value("args").toArray()) {if(!arg.isString())return fail("Only string task arguments are supported");arguments.append(arg.toString());}
                if(type=="process") {step.command=arguments;step.command.prepend(command.toString());}
                else {
                    const auto shell=options.value("shell").toObject();
                    const auto executable=shell.value("executable").toString("/bin/sh");
                    if(executable.isEmpty())return fail("Task shell executable is empty");
                    step.command.append(executable);
                    if(shell.contains("args")) {if(!shell.value("args").isArray())return fail("Shell args must be an array");for(const auto& arg:shell.value("args").toArray()){if(!arg.isString())return fail("Shell args must be strings");step.command.append(arg.toString());}}
                    else step.command.append("-c");
                    QString script=arguments.isEmpty()?command.toString():quote(command.toString());
                    for(const auto& arg:arguments)script+=" "+quote(arg);
                    step.command.append(script);
                }
                steps.append(step);
            }
            visiting.remove(name);done.insert(name);return true;
        };
        return visit(label);
    }
    QString root_,file_;QJsonObject settings_;QString* error_{nullptr};
};
