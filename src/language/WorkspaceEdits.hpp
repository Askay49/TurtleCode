#pragma once
#include "core/text/EditHistory.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>
#include <QString>
#include <optional>
struct WorkspaceDocumentEdit {
    QString path;
    std::optional<int> version;
    QJsonArray edits;
};
namespace WorkspaceEdits {
QString pathKey(const QString& path);
bool decode(const QJsonObject& workspace, QVector<WorkspaceDocumentEdit>* output, QString* error);
bool convert(const QByteArray& before, const QJsonArray& edits, std::vector<EditChange>* output, QString* error);
bool convertFormatting(const QByteArray& before, const QJsonArray& edits, std::vector<EditChange>* output, QString* error);
bool rangeText(const QByteArray& before, const QJsonObject& range, QString* text, QString* error);
}
