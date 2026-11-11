#include "app/I18n.hpp"
#include "app/AppIdentity.hpp"
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cassert>
int main(int argc,char**argv) {
    QTemporaryDir directory; assert(directory.isValid()); qputenv("XDG_CONFIG_HOME",directory.path().toUtf8());
    qputenv("XDG_DATA_HOME", (directory.path()+"/data").toUtf8());
    QCoreApplication app(argc,argv);
    app.setOrganizationName("BlinkCode"); app.setApplicationName("BlinkCode");
    const auto previousConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const auto previousData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    assert(QDir().mkpath(previousConfig+"/themes"));
    assert(QDir().mkpath(previousData+"/grammars"));
    assert(QDir().mkpath(previousData+"/recovery/session"));
    QFile theme(previousConfig+"/themes/custom.json"); assert(theme.open(QIODevice::WriteOnly));
    assert(theme.write("{}")==2); theme.close();
    QSettings previous; previous.setValue("appearance/theme", "custom"); previous.setValue("lsp/profiles", "profiles"); previous.sync();
    QLockFile activeJournal(previousData+"/recovery/session/lock");
    assert(activeJournal.tryLock());
    assert(!AppIdentity::migrate().isEmpty());
    assert(QDir(previousData+"/recovery/session").exists());
    activeJournal.unlock();
    assert(AppIdentity::migrate().isEmpty());
    assert(app.applicationName()=="Turtle Code");
    assert(QSettings().value("appearance/theme")=="custom");
    assert(QSettings().value("lsp/profiles")=="profiles");
    assert(QFile::exists(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/themes/custom.json"));
    assert(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/grammars").exists());
    assert(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/recovery/session").exists());
    QSettings().setValue("appearance/theme", "new-choice");
    assert(AppIdentity::migrate().isEmpty());
    assert(QSettings().value("appearance/theme")=="new-choice");
    app.setOrganizationName("TurtleCodeTests"); app.setApplicationName("I18nTests");
    assert(I18n::saveLanguage("en")); assert(I18n::initialize());
    assert(I18n::text("&File")=="&File");
    assert(I18n::saveLanguage("ru")); assert(I18n::selectedLanguage()=="ru");
    assert(I18n::activeLanguage()=="en"); // Selection is intentionally deferred until restart.
    assert(I18n::initialize()); assert(I18n::activeLanguage()=="ru");
    assert(I18n::text("&File")==QString::fromUtf8("&Файл"));
    assert(QCoreApplication::translate("EditorWidget","Find")==QString::fromUtf8("Найти"));
    assert(QCoreApplication::translate("QPlatformTheme","Cancel")==QString::fromUtf8("Отмена"));
    assert(I18n::text("Theme...")==QString::fromUtf8("Тема оформления..."));
    assert(I18n::text("Renamed %1 references in %2 files. Changes are not saved yet.").arg(3).arg(2).contains(QString::fromUtf8("3")));
    assert(I18n::text("user-theme-name")=="user-theme-name");
    assert(!I18n::saveLanguage("fr")); assert(I18n::selectedLanguage()=="ru");
    QFile file(":/i18n/ru.json"); assert(file.open(QIODevice::ReadOnly));
    const auto catalog=QJsonDocument::fromJson(file.readAll()).object(); assert(catalog.size()>300);
    auto placeholders=[](const QString& text) {
        QStringList values; auto matches=QRegularExpression("%[1-9][0-9]*|%n").globalMatch(text);
        while(matches.hasNext()) values.append(matches.next().captured());
        std::sort(values.begin(),values.end()); return values;
    };
    for(auto it=catalog.begin();it!=catalog.end();++it) {
        assert(it.value().isString() && !it.value().toString().isEmpty());
        assert(placeholders(it.key())==placeholders(it.value().toString()));
    }
    assert(I18n::saveLanguage("en")); assert(I18n::initialize());
    assert(I18n::text("&File")=="&File");
}
