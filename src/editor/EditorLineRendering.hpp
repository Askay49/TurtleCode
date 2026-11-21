#pragma once

#include <QFontMetrics>
#include <QPainter>
#include <cstddef>

namespace EditorLineRendering {
inline constexpr int textInset = 12;
inline constexpr std::size_t longLineBytes = 64u * 1024u;
inline QColor alignmentGap() { return QColor(128, 128, 128, 22); }
inline int rowHeight(const QFontMetrics& metrics) { return metrics.height() + 3; }
inline int baseline(const QFontMetrics& metrics, int height) {
    return (height - metrics.height()) / 2 + metrics.ascent();
}
inline int gutterWidth(const QFontMetrics& metrics, std::size_t lineCount) {
    return metrics.horizontalAdvance(QString::number(lineCount)) + 24;
}
inline void drawLineNumber(QPainter& painter, int gutter, int y, int height, int line) {
    painter.drawText(QRect(8, y, gutter - 14, height), Qt::AlignVCenter | Qt::AlignRight,
                     QString::number(line + 1));
}
}
