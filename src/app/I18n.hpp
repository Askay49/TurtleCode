#pragma once
#include <QString>
namespace I18n {
// Initialize once before constructing application UI/static command registries.
bool initialize();
QString text(const char* source);
QString text(const QString& source);
QString selectedLanguage();
QString activeLanguage();
bool saveLanguage(const QString& language);
}
