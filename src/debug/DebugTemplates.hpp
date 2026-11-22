#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>

class QWidget;

namespace DebugTemplates {
struct Draft {
    QJsonObject launch;
    QJsonArray tasks;
};

void choose(QWidget* parent, const QString& root, const QString& currentFile,
            const std::function<QString(const Draft&)>& save);
bool append(const QString& root, const Draft& draft, QString& name, QString& error);
QStringList taskLabels(const QJsonArray& tasks);
}
