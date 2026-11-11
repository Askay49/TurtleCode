#include "app/Codicons.hpp"
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <cassert>

static bool hasInk(const QImage& image) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() != 0) return true;
    return false;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto names = Codicons::names();
    assert(names.size() > 600);
    // Check font coverage too: a visible missing-glyph box must not pass as an icon.
    QFile file(":/codicons/mapping.json"); assert(file.open(QIODevice::ReadOnly));
    const auto mapping = QJsonDocument::fromJson(file.readAll()).object();
    const int id = QFontDatabase::addApplicationFont(":/codicons/codicon.ttf");
    assert(id >= 0);
    QFont font(QFontDatabase::applicationFontFamilies(id).first());
    font.setPixelSize(16);
    const QFontMetrics metrics(font);
    for (auto it = mapping.begin(); it != mapping.end(); ++it)
        assert(metrics.inFontUcs4(it.key().toUInt()));
    for (const auto& name : names) {
        const auto icon = Codicons::icon(name);
        assert(!icon.isNull());
        // Upstream deliberately includes an empty placeholder named "blank".
        assert(hasInk(icon.pixmap(16, 16).toImage()) == (name != "blank"));
    }
    for (int kind = 0; kind <= 26; ++kind)
        assert(!Codicons::completionIcon(kind).isNull());
    auto icon = Codicons::icon("search");
    const auto normal = icon.pixmap(16, 16).toImage();
    const auto highDpi = icon.pixmap(QSize(16, 16), 2.0);
    assert(highDpi.size() == QSize(32, 32));
    assert(highDpi.devicePixelRatio() == 2.0);
    assert(hasInk(highDpi.toImage()));
    auto palette = app.palette();
    palette.setColor(QPalette::ButtonText, Qt::red);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, Qt::green);
    palette.setColor(QPalette::HighlightedText, Qt::blue);
    app.setPalette(palette);
    assert(normal != icon.pixmap(16, 16).toImage());
    assert(icon.pixmap(16, 16).toImage() != icon.pixmap(16, 16, QIcon::Disabled).toImage());
    assert(icon.pixmap(16, 16).toImage() != icon.pixmap(16, 16, QIcon::Selected).toImage());
    auto folder = Codicons::folderIcon();
    assert(folder.pixmap(16, 16, QIcon::Normal, QIcon::On).toImage()
        != folder.pixmap(16, 16, QIcon::Normal, QIcon::Off).toImage());
}
