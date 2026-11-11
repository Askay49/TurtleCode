#include "Codicons.hpp"
#include "FontIcon.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// Explicit initialization also keeps the resource object in static-library links.
static void initializeCodiconResources() { Q_INIT_RESOURCE(turtle_code_codicons); }

namespace {
struct IconData {
    QString family;
    QHash<QString, char16_t> glyphs;
    IconData() {
        initializeCodiconResources();
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/codicons/codicon.ttf"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (families.isEmpty()) qFatal("Could not load the bundled Codicons font");
        family = families.first();
        QFile file(QStringLiteral(":/codicons/mapping.json"));
        if (!file.open(QIODevice::ReadOnly)) qFatal("Could not load the Codicons mapping");
        const auto mapping = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = mapping.begin(); it != mapping.end(); ++it)
            for (const auto& name : it.value().toArray())
                glyphs.insert(name.toString(), static_cast<char16_t>(it.key().toUInt()));
    }
};
const IconData& data() { static const IconData value; return value; }


}

QStringList Codicons::names() {
    auto result = data().glyphs.keys();
    result.sort();
    return result;
}
QIcon Codicons::icon(const QString& name, const QColor& color) {
    const auto glyph = data().glyphs.constFind(name);
    if (glyph == data().glyphs.cend()) {
        qWarning("Unknown Codicon: %s", qPrintable(name));
        return {};
    }
    return IconFonts::icon({.family = data().family, .character = QChar(*glyph), .color = color, .name = name});
}
QIcon Codicons::folderIcon() {
    return IconFonts::icon({.family = data().family,
        .character = QChar(data().glyphs.value(QStringLiteral("folder"))),
        .expandedCharacter = QChar(data().glyphs.value(QStringLiteral("folder-opened"))),
        .name = QStringLiteral("folder")});
}

QIcon Codicons::completionIcon(int kind) {
    // LSP CompletionItemKind is one-based, with 25 standardized values.
    static const char* const names[] = {"symbol-text", "symbol-method", "symbol-function",
        "symbol-constructor", "symbol-field", "symbol-variable", "symbol-class", "symbol-interface",
        "symbol-namespace", "symbol-property", "symbol-unit", "symbol-value", "symbol-enum",
        "symbol-keyword", "symbol-snippet", "symbol-color", "file", "references", "folder",
        "symbol-enum-member", "symbol-constant", "symbol-struct", "symbol-event", "symbol-operator",
        "symbol-type-parameter"};
    return icon(QString::fromLatin1(names[kind >= 1 && kind <= 25 ? kind - 1 : 0]));
}
