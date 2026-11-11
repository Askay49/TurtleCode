#pragma once
#include <QHash>
#include <cstddef>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>
struct SemanticToken {
    int line{}, start{}, length{};
    QString type;
    QStringList modifiers;
};
struct SemanticHighlightResult {
    QHash<int,QVector<SemanticToken>> lines;
    QString error;
    bool valid{false};
};
namespace SemanticHighlighting {
SemanticHighlightResult decode(const QJsonArray& data,const QJsonObject& legend,const QString& text,int originLine = 0,std::size_t maxTokens = 100000);
// Keep unaffected rows aligned without copying the whole document after each key.
void applyEdit(QHash<int,QVector<SemanticToken>>& lines,int firstLine,int removedLines,int insertedLines);
}
