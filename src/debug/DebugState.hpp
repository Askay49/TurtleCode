#pragma once
#include <QMap>
#include "core/text/LineAnchor.hpp"
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>

// Persistent workspace data, independent of adapter IDs and widgets.
struct DebugState {
    QMap<QString,QSet<int>> points, disabled;
    QJsonObject configuration;
    QStringList watches;
    void moveLines(const QString& path,int row,int column,int removed,int inserted) {
        auto project=[=](const QSet<int>& source) {
            QSet<int> result;
            for(int line:source) {
                result.insert(projectLineAnchor(line,row,column,removed,inserted));
            }
            return result;
        };
        points[path]=project(points.value(path)); disabled[path]=project(disabled.value(path));
    }
    QJsonObject json() const {
        QJsonArray list;
        for(auto it=points.cbegin();it!=points.cend();++it)
            for(int line:it.value()) list.append(QJsonObject{{"path",it.key()},{"line",line},{"enabled",!disabled.value(it.key()).contains(line)}});
        return {{"configuration",configuration},{"breakpoints",list},{"watches",QJsonArray::fromStringList(watches)}};
    }
    void restore(const QJsonObject& value) {
        points.clear(); disabled.clear(); watches.clear(); configuration=value.value("configuration").toObject();
        for(const auto& v:value.value("breakpoints").toArray()) {
            const auto b=v.toObject(); const auto path=b.value("path").toString(); const int line=b.value("line").toInt();
            if(path.isEmpty() || line<1) continue;
            points[path].insert(line); if(!b.value("enabled").toBool(true)) disabled[path].insert(line);
        }
        for(const auto& v:value.value("watches").toArray()) if(v.isString()) watches.append(v.toString());
    }
};

struct DebugSessionState {
    QString lastError;
    QMap<QString,QSet<int>> verified;
    QHash<int,QPair<QString,int>> serverPoints;
    QHash<QString,quint64> pointVersions;
    QHash<QString,QHash<int,QString>> pointMessages;
    QSet<QString> editedSources;
    QJsonObject launchArguments,capabilities;
    int thread{0},frame{-1};
    quint64 epoch{0},watchGeneration{0};
    bool stopped{false},configuring{false},ending{false};
};
