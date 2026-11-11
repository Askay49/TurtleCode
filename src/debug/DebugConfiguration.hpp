#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <functional>
#include "DebugProject.hpp"

// Project launch configuration. UI state and breakpoints remain local settings.
struct DebugConfiguration {
    QString type,program,python,adapter,cwd;
    QJsonArray arguments,adapterArguments,preLaunch;
    QJsonObject launchArguments;
    QString adapterID,request{"launch"};
    QVector<DebugBuildStep> buildSteps;
    QJsonObject env;
    bool stopOnEntry{false},externalSymbols{false};
    static QString legacyPath(const QString& root) {return root.isEmpty()?QString{}:QDir(root).filePath(".turtle/debug.json");}
    static QString filePath(const QString& root) {
        if(root.isEmpty())return {};
        const auto path=DebugProject::path(root,"launch.json");
        return QFileInfo::exists(path) || !QFileInfo::exists(legacyPath(root))?path:legacyPath(root);
    }
    static QJsonObject defaults(bool python) {
        QJsonObject result{{"version",1},{"type",python?"python":"cpp"},{"program",python?"${file}":"build/app"},
                           {"args",QJsonArray{}},{"cwd","${workspaceFolder}"},{"stopOnEntry",false}};
        result[python?"python":"adapter"]=python?"python3":"lldb-dap";
        return result;
    }
    static QString portable(const QString& path,const QString& root) {
        if(path.isEmpty() || !QDir::isAbsolutePath(path)) return path;
        const auto relative=QDir(root).relativeFilePath(path);
        return relative==".." || relative.startsWith("../")?path:relative;
    }
    static bool read(const QString& root,const QString& currentFile,DebugConfiguration& result,QString& error,const QString& selected={}) {
        const auto path=filePath(root);
        auto fail=[&](const QString& message){error=path+": "+message; return false;};
        error.clear();QJsonObject object;QVector<DebugBuildStep> steps;
        if(path.endsWith("/launch.json")) {
            DebugProject project(root,currentFile);
            if(!project.load(selected,object,steps,error))return false;
        } else if(!DebugJson::read(path,object,error))return false;
        if(object.value("version").toDouble()!=1) return fail(QStringLiteral("version must be 1"));
        for(auto it=object.begin();it!=object.end();++it)
            if(!QStringList{"version","type","program","python","adapter","args","cwd","stopOnEntry","externalSymbols","adapterArgs","adapterID","launchArguments","preLaunch","request","env"}.contains(it.key()))
                return fail(QStringLiteral("Unknown setting: ")+it.key());
        const auto type=object.value("type").toString();
        if(type!="python" && type!="cpp" && type!="dap") return fail(QStringLiteral("type must be python, cpp or dap"));
        for(const auto& key:{"program","python","adapter","cwd","adapterID","request"})
            if(object.contains(key) && !object.value(key).isString()) return fail(QString(key)+QStringLiteral(" must be a string"));
        if((type=="python" && object.contains("python") && object.value("python").toString().isEmpty()) ||
           (type!="python" && object.contains("adapter") && object.value("adapter").toString().isEmpty())) return fail(QStringLiteral("Debugger executable must not be empty"));
        if(object.value("program").toString().isEmpty()) return fail(QStringLiteral("program must be a non-empty string"));
        if(object.contains("args") && !object.value("args").isArray()) return fail(QStringLiteral("args must be an array of strings"));
        if(object.contains("stopOnEntry") && !object.value("stopOnEntry").isBool()) return fail(QStringLiteral("stopOnEntry must be true or false"));
        if(object.contains("externalSymbols") && !object.value("externalSymbols").isBool()) return fail(QStringLiteral("externalSymbols must be true or false"));
        if(type=="dap" && (object.value("adapter").toString().isEmpty() || object.value("adapterID").toString().isEmpty()))
            return fail(QStringLiteral("dap requires adapter and adapterID"));
        if(object.contains("launchArguments") && !object.value("launchArguments").isObject()) return fail(QStringLiteral("launchArguments must be an object"));
        if(object.contains("adapterArgs") && !object.value("adapterArgs").isArray()) return fail(QStringLiteral("adapterArgs must be an array of strings"));
        if(object.contains("preLaunch") && !object.value("preLaunch").isArray()) return fail(QStringLiteral("preLaunch must be an array of command arrays"));
        const auto request=object.value("request").toString("launch");
        if(request!="launch" && request!="attach") return fail(QStringLiteral("request must be launch or attach"));
        if(type!="dap" && (request!="launch" || object.contains("launchArguments") || object.contains("adapterArgs") || object.contains("adapterID")))
            return fail(QStringLiteral("Custom adapter options require type dap"));
        auto expand=[&](QString value) {
            value.replace("${workspaceFolder}",QDir(root).absolutePath());
            value.replace("${workspaceRoot}",QDir(root).absolutePath());
            if(!currentFile.isEmpty()) {value.replace("${fileDirname}",QFileInfo(currentFile).absolutePath()); value.replace("${file}",currentFile);}
            return value;
        };
        auto resolve=[&](const QString& value,bool executable=false) {
            const auto expanded=expand(value);
            if(executable && !expanded.contains('/') && !expanded.isEmpty()) return expanded;
            return QDir::cleanPath(QDir(root).absoluteFilePath(expanded));
        };
        result={}; result.type=type;result.buildSteps=steps;
        if(object.contains("env")) {
            if(!object.value("env").isObject())return fail(QStringLiteral("env must be an object"));
            result.env=object.value("env").toObject();
            for(auto it=result.env.begin();it!=result.env.end();++it)if(!it.value().isString() && !it.value().isNull())return fail(QStringLiteral("env values must be strings or null"));
        }
        result.program=resolve(object.value("program").toString());
        result.cwd=resolve(object.value("cwd").toString("${workspaceFolder}"));
        result.python=resolve(object.value("python").toString("python3"),true);
        result.adapter=resolve(object.value("adapter").toString("lldb-dap"),true);
        result.stopOnEntry=object.value("stopOnEntry").toBool(false);
        result.externalSymbols=object.value("externalSymbols").toBool(false);
        for(const auto& value:object.value("args").toArray()) {
            if(!value.isString()) return fail(QStringLiteral("args must contain only strings"));
            result.arguments.append(expand(value.toString()));
        }
        result.request=request; result.adapterID=object.value("adapterID").toString();
        for(const auto& value:object.value("adapterArgs").toArray()) {
            if(!value.isString()) return fail(QStringLiteral("adapterArgs must contain only strings"));
            const auto argument=expand(value.toString());
            if(argument.contains("${")) return fail(QStringLiteral("Unresolved variable in adapterArgs"));
            result.adapterArguments.append(argument);
        }
        for(const auto& command:object.value("preLaunch").toArray()) {
            if(!command.isArray() || command.toArray().isEmpty()) return fail(QStringLiteral("preLaunch commands must be non-empty arrays of strings"));
            QJsonArray arguments;
            for(const auto& value:command.toArray()) {
                if(!value.isString()) return fail(QStringLiteral("preLaunch commands must contain only strings"));
                const auto argument=expand(value.toString());
                if(argument.contains("${")) return fail(QStringLiteral("Unresolved variable in preLaunch"));
                arguments.append(argument);
            }
            if(arguments.first().toString().isEmpty()) return fail(QStringLiteral("Empty preLaunch executable"));
            result.preLaunch.append(arguments);
        }
        bool validLaunch=true;
        std::function<QJsonValue(QJsonValue)> expandValue=[&](QJsonValue value)->QJsonValue {
            if(value.isString()) {
                auto text=expand(value.toString()); text.replace("${program}",result.program);
                auto checked=text; checked.remove("${serverConsolePort}");
                if(checked.contains("${")) validLaunch=false;
                return text;
            }
            if(value.isArray()) {QJsonArray values; for(const auto& item:value.toArray()) values.append(expandValue(item)); return values;}
            if(value.isObject()) {auto values=value.toObject(); for(auto it=values.begin();it!=values.end();++it) it.value()=expandValue(it.value()); return values;}
            return value;
        };
        result.launchArguments=expandValue(object.value("launchArguments").toObject()).toObject();
        if(!validLaunch) return fail(QStringLiteral("Unresolved variable in launchArguments"));
        for(const auto& value:{result.program,result.cwd,result.python,result.adapter})
            if(value.contains("${")) return fail(QStringLiteral("Unresolved variable (open a source file for ${file}, or check the variable name)"));
        for(const auto& value:result.arguments) if(value.toString().contains("${")) return fail(QStringLiteral("Unresolved variable in args"));
        if((type=="python" && result.python.isEmpty()) || (type=="cpp" && result.adapter.isEmpty())) return fail(QStringLiteral("Debugger executable must not be empty"));
        if(result.preLaunch.isEmpty() && !QFileInfo(result.program).isFile()) return fail(QStringLiteral("Program does not exist: ")+result.program);
        if(!QDir(result.cwd).exists()) return fail(QStringLiteral("Working directory does not exist: ")+result.cwd);
        return true;
    }
};
