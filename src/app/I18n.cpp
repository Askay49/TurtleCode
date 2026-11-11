#include "I18n.hpp"
#include <QCoreApplication>
#include <QTranslator>
#include <QSettings>
#include <QLocale>
#include <QLibraryInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <memory>
static void initializeTranslationResources() { Q_INIT_RESOURCE(turtle_code_translations); }
namespace {
class CatalogTranslator final : public QTranslator {
public:
    bool read(const QString& path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return false;
        QJsonParseError error; const auto doc=QJsonDocument::fromJson(file.readAll(),&error);
        if (error.error!=QJsonParseError::NoError || !doc.isObject()) return false;
        const auto object=doc.object();
        for (auto it=object.begin();it!=object.end();++it) if (it.value().isString()) values_.insert(it.key(),it.value().toString());
        return !values_.isEmpty();
    }
    bool isEmpty() const override { return values_.isEmpty(); }
    QString translate(const char*,const char* source,const char*,int) const override {
        return values_.value(QString::fromUtf8(source));
    }
private:
    QHash<QString,QString> values_;
};
struct Translators {
    std::unique_ptr<CatalogTranslator> application;
    std::unique_ptr<QTranslator> qt;
    QString language{"en"};
};
Translators& translators() { static Translators state; return state; }
}
QString I18n::selectedLanguage() {
    const auto fallback=QLocale::system().language()==QLocale::Russian?QStringLiteral("ru"):QStringLiteral("en");
    const auto language=QSettings().value("interface/language",fallback).toString();
    return language=="ru"?QStringLiteral("ru"):QStringLiteral("en");
}
QString I18n::activeLanguage() { return translators().language; }
bool I18n::saveLanguage(const QString& language) {
    if (language!="ru" && language!="en") return false;
    QSettings settings; settings.setValue("interface/language",language); settings.sync();
    return settings.status()==QSettings::NoError;
}
bool I18n::initialize() {
    initializeTranslationResources();
    auto& state=translators();
    if (state.application) QCoreApplication::removeTranslator(state.application.get());
    if (state.qt) QCoreApplication::removeTranslator(state.qt.get());
    state.application.reset(); state.qt.reset(); state.language=selectedLanguage();
    if (state.language=="en") return true;
    auto application=std::make_unique<CatalogTranslator>();
    if (!application->read(":/i18n/ru.json")) { state.language="en"; return false; }
    auto qt=std::make_unique<QTranslator>();
    if (qt->load("qtbase_ru",QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QCoreApplication::installTranslator(qt.get()); state.qt=std::move(qt);
    }
    QCoreApplication::installTranslator(application.get()); state.application=std::move(application);
    return true;
}
QString I18n::text(const char* source) { return QCoreApplication::translate("Turtle Code",source); }
QString I18n::text(const QString& source) { const auto bytes=source.toUtf8(); return text(bytes.constData()); }
