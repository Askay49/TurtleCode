#pragma once

#include <QIcon>
#include <QString>

// Seti file associations are independent of installed/activated grammars.
namespace FileIcons {
[[nodiscard]] QIcon icon(const QString& path, const QString& languageId = {});
}
