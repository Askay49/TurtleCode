#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Central catalogue of editor languages. Adding a language means registering its
// metadata and providing its Tree-sitter backend/query; editor UI stays unchanged.
struct LanguageDefinition {
    QString id;
    QString name;
    QStringList extensions;
    QString grammar;       // Tree-sitter grammar repository / module id
    QString category;
    QString repository;
    QString parserSymbol;
    QString sourceSubdirectory;
    QString defaultBranch;
};

class LanguageRegistry final {
public:
    [[nodiscard]] static const QVector<LanguageDefinition>& languages();
    [[nodiscard]] static const LanguageDefinition* language(const QString& id);
    [[nodiscard]] static const LanguageDefinition* languageForPath(const QString& path);
    [[nodiscard]] static bool isEnabled(const QString& id);
    static void setEnabled(const QString& id, bool enabled);
};
