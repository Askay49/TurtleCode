#pragma once

#include <QIcon>
#include <QColor>
#include <QStringList>

// The complete bundled Codicons mapping is available by its upstream names.
namespace Codicons {
[[nodiscard]] QStringList names();
[[nodiscard]] QIcon icon(const QString& name, const QColor& color = {});
[[nodiscard]] QIcon folderIcon();
[[nodiscard]] QIcon completionIcon(int lspKind);
}
