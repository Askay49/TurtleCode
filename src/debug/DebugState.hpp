#pragma once
#include <QMap>
#include "core/text/LineAnchor.hpp"
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSettings>
#include <utility>

// Persistent workspace data, independent of adapter IDs and widgets.
struct DebugState {
    QMap<QString,QSet<int>> points, disabled;
    QJsonObject configuration;
    QStringList watches;
    static QString settingsKey(const QString& workspace) {
        if (workspace.isEmpty()) return "debug/standalone";
        return "debug/workspaces/" + QString::fromLatin1(
            QCryptographicHash::hash(workspace.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    void load(const QString& workspace) {
        restore(QJsonDocument::fromJson(QSettings().value(settingsKey(workspace)).toByteArray()).object());
    }
    void save(const QString& workspace) const {
        auto data = json();
        if (configuration.isEmpty()) data.remove("configuration");
        QSettings().setValue(settingsKey(workspace), QJsonDocument(data).toJson(QJsonDocument::Compact));
    }
    void renameFile(const QString& path, const QString& updated) {
        const auto lines = points.take(path);
        const auto disabledLines = disabled.take(path);
        points[updated].unite(lines);
        disabled[updated].unite(disabledLines);
    }
    void toggleLine(const QString& path, int line) {
        auto& lines = points[path];
        if (!lines.remove(line)) lines.insert(line);
        disabled[path].remove(line);
    }
    bool remapLines(const QString& path, const QMap<int, int>& anchors) {
        QSet<int> mappedPoints, mappedDisabled;
        const auto lines = points.value(path);
        const auto disabledLines = disabled.value(path);
        for (int line : lines) {
            const int mapped = anchors.value(line, line);
            mappedPoints.insert(mapped);
            if (disabledLines.contains(line)) mappedDisabled.insert(mapped);
        }
        if (mappedPoints == lines && mappedDisabled == disabledLines) return false;
        points[path] = std::move(mappedPoints);
        disabled[path] = std::move(mappedDisabled);
        return true;
    }
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
