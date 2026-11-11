#pragma once

#include <QColor>
#include <QIcon>

// Shared scalable renderer for the bundled UI and file icon fonts.
namespace IconFonts {
struct Glyph {
    QString family;
    QChar character;
    QChar expandedCharacter;
    QColor color;
    QColor lightColor;
    qreal sizeFactor{1.0};
    QString name;
};
[[nodiscard]] QIcon icon(const Glyph& glyph);
}
