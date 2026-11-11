#include "FileIcons.hpp"
#include "FontIcon.hpp"

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>

static void initializeSetiResources() { Q_INIT_RESOURCE(turtle_code_seti); }

namespace {
QJsonObject readObject(const char* path) {
    QFile file(QString::fromLatin1(path));
    if (!file.open(QIODevice::ReadOnly)) qFatal("Could not load bundled Seti associations");
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) qFatal("Invalid bundled Seti associations");
    return document.object();
}
struct SetiData {
    QString family;
    QJsonObject theme;
    QJsonObject languages;
    SetiData() {
        initializeSetiResources();
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/seti/seti.ttf"));
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (families.isEmpty()) qFatal("Could not load bundled Seti font");
        family = families.first();
        theme = readObject(":/seti/vs-seti-icon-theme.json");
        languages = readObject(":/seti/language-associations.json");
    }
};
const SetiData& data() { static const SetiData value; return value; }

QString association(const QJsonObject& map, const QString& name) {
    return map.value(name).toString();
}
QString extensionAssociation(const QJsonObject& map, const QString& name) {
    // Compound extensions have priority: test.tsx, css.map, tfvars.json, etc.
    for (qsizetype dot = name.indexOf(QLatin1Char('.')); dot >= 0;
         dot = name.indexOf(QLatin1Char('.'), dot + 1)) {
        const QString value = association(map, name.mid(dot + 1));
        if (!value.isEmpty()) return value;
    }
    return {};
}
QString resolve(const QJsonObject& theme, const QString& path, const QString& languageId) {
    const QString name = QFileInfo(path).fileName().toLower();
    QString id = association(theme.value("fileNames").toObject(), name);
    if (id.isEmpty()) id = extensionAssociation(theme.value("fileExtensions").toObject(), name);
    if (id.isEmpty()) {
        QString language = languageId;
        if (language.isEmpty()) {
            language = association(data().languages.value("fileNames").toObject(), name);
            if (language.isEmpty()) language = extensionAssociation(data().languages.value("fileExtensions").toObject(), name);
        }
        id = association(theme.value("languageIds").toObject(), language);
    }
    return id;
}
}
QIcon FileIcons::icon(const QString& path, const QString& languageId) {
    const auto& seti = data();
    const QString id = resolve(seti.theme, path, languageId);
    const QString normalId = id.isEmpty() ? seti.theme.value("file").toString() : id;
    const auto definitions = seti.theme.value("iconDefinitions").toObject();
    const auto normal = definitions.value(normalId).toObject();
    const auto lightTheme = seti.theme.value("light").toObject();
    QString lightId = resolve(lightTheme, path, languageId);
    if (lightId.isEmpty()) lightId = id.isEmpty() ? lightTheme.value("file").toString() : normalId;
    const auto light = definitions.value(lightId).toObject();
    const QString character = normal.value("fontCharacter").toString();
    // The VS Code theme stores CSS hex escapes ("\\E01A"), not Unicode strings.
    bool valid = false;
    const uint codepoint = character.mid(1).toUInt(&valid, 16);
    if (!character.startsWith(QLatin1Char('\\')) || !valid || codepoint > 0xffff)
        qFatal("Invalid bundled Seti glyph");
    return IconFonts::icon({.family = seti.family, .character = QChar(static_cast<char16_t>(codepoint)),
        .color = QColor(normal.value("fontColor").toString()),
        .lightColor = QColor(light.value("fontColor").toString()),
        .sizeFactor = 1.5, .name = normalId});
}
