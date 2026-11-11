#include "FontIcon.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <algorithm>

namespace {
class FontIconEngine final : public QIconEngine {
public:
    explicit FontIconEngine(const IconFonts::Glyph& glyph) : glyph_(glyph) {}
    QIconEngine* clone() const override { return new FontIconEngine(glyph_); }
    QString key() const override { return QStringLiteral("Turtle Code.FontIcon"); }
    QString iconName() override { return glyph_.name; }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        if (rect.isEmpty()) return;
        painter->save();
        const auto palette = QApplication::palette();
        QColor color;
        if (mode == QIcon::Disabled) color = palette.color(QPalette::Disabled, QPalette::ButtonText);
        else if (glyph_.color.isValid()) {
            color = palette.color(QPalette::Base).lightnessF() > 0.5 && glyph_.lightColor.isValid()
                ? glyph_.lightColor : glyph_.color;
        } else color = palette.color(mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::ButtonText);
        painter->setPen(color);
        QFont font(glyph_.family);
        font.setPixelSize(qRound(std::min(rect.width(), rect.height()) * glyph_.sizeFactor));
        font.setWeight(QFont::Normal);
        font.setStyleStrategy(QFont::NoFontMerging);
        painter->setFont(font);
        painter->setRenderHint(QPainter::TextAntialiasing);
        const QChar character = state == QIcon::On && !glyph_.expandedCharacter.isNull()
            ? glyph_.expandedCharacter : glyph_.character;
        // Seti uses 150% font size: its glyph outlines occupy less than one em.
        // Allow the font's line box outside rect, then clip the actual icon cell.
        painter->setClipRect(rect, Qt::IntersectClip);
        painter->drawText(rect.adjusted(0, -rect.height(), 0, rect.height()), Qt::AlignCenter, QString(character));
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return renderPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
        // Before Qt 6.8 QIcon passed physical pixels to scaledPixmap().
        return renderPixmap(size / scale, mode, state, scale);
#else
        return renderPixmap(size, mode, state, scale);
#endif
    }
private:
    QPixmap renderPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) {
        if (size.isEmpty()) return {};
        QPixmap result(size * scale);
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
    IconFonts::Glyph glyph_;
};
}
QIcon IconFonts::icon(const Glyph& glyph) { return QIcon(new FontIconEngine(glyph)); }
