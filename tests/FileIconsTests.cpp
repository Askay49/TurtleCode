#include "app/FileIcons.hpp"
#include "app/FontIcon.hpp"
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QImage>
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
static QJsonObject object(const char* path) {
    QFile file(QString::fromLatin1(path)); assert(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    auto palette = app.palette(); palette.setColor(QPalette::Base, Qt::black); app.setPalette(palette);
    // Common languages use language IDs, while headers and special names override them.
    const QPair<const char*, const char*> cases[] = {
        {"main.c", "_c"}, {"main.cpp", "_cpp"}, {"main.h", "_c_1"}, {"main.hpp", "_cpp_1"},
        {"app.py", "_python"}, {"app.js", "_javascript"}, {"app.ts", "_typescript"},
        {"app.jsx", "_react"}, {"app.tsx", "_react"}, {"app.rs", "_rust"}, {"app.go", "_go2"},
        {"data.json", "_json"}, {"data.yaml", "_yml"}, {"data.xml", "_xml"},
        {"data.csv", "_csv"}, {"style.css", "_css"}, {"page.html", "_html_3"},
        {"README.md", "_info"}, {"notes.md", "_markdown"}, {"Dockerfile", "_docker"},
        {"Makefile", "_makefile"}, {"CMakeLists.txt", "_makefile_3"},
        {".gitignore", "_git"}, {".gitlab-ci.yml", "_gitlab"},
        {"app.test.ts", "_typescript_1"}, {"app.spec.tsx", "_react_1"},
        {"style.css.map", "_css"}, {"infra.tfvars.json", "_terraform"},
        {"tsconfig.json", "_tsconfig"}, {"photo.png", "_image"},
        {"manual.pdf", "_pdf"}, {"archive.zip", "_zip_1"}, {"file.unknown", "_default"}
    };
    for (const auto& item : cases) {
        const auto icon = FileIcons::icon(QString::fromLatin1(item.first));
        assert(icon.name() == QString::fromLatin1(item.second));
        assert(hasInk(icon.pixmap(16, 16).toImage()));
    }
    assert(FileIcons::icon("/project/MAIN.CPP").name() == "_cpp");
    assert(FileIcons::icon("/project.with.dots/file.unknown").name() == "_default");
    assert(FileIcons::icon("extensionless", "python").name() == "_python");
    assert(FileIcons::icon("README.md", "python").name() == "_info");
    assert(FileIcons::icon("app.cpp").pixmap(16, 16).toImage()
        != FileIcons::icon("app.py").pixmap(16, 16).toImage());

    const auto theme = object(":/seti/vs-seti-icon-theme.json");
    const auto definitions = theme.value("iconDefinitions").toObject();
    const int id = QFontDatabase::addApplicationFont(":/seti/seti.ttf"); assert(id >= 0);
    const QString family = QFontDatabase::applicationFontFamilies(id).first();
    QFont font(family); font.setPixelSize(24); const QFontMetrics metrics(font);
    // Verify the matching upstream TTF covers every definition, including light variants.
    for (auto it = definitions.begin(); it != definitions.end(); ++it) {
        const auto definition = it.value().toObject();
        bool ok = false;
        const uint codepoint = definition.value("fontCharacter").toString().mid(1).toUInt(&ok, 16);
        assert(ok && metrics.inFontUcs4(codepoint));
        const auto icon = IconFonts::icon({.family = family, .character = QChar(static_cast<char16_t>(codepoint)),
            .color = QColor(definition.value("fontColor").toString()), .sizeFactor = 1.5});
        assert(hasInk(icon.pixmap(16, 16).toImage()));
    }
    // Check all associations refer to available definitions and resolve to visible glyphs.
    for (const QString key : {QStringLiteral("fileNames"), QStringLiteral("fileExtensions"), QStringLiteral("languageIds")}) {
        const auto associations = theme.value(key).toObject();
        for (auto it = associations.begin(); it != associations.end(); ++it) {
            assert(definitions.contains(it.value().toString()));
            const auto icon = key == "languageIds" ? FileIcons::icon("extensionless", it.key())
                : FileIcons::icon(key == "fileNames" ? it.key() : "example." + it.key());
            assert(hasInk(icon.pixmap(16, 16).toImage()));
        }
    }
    auto icon = FileIcons::icon("app.cpp");
    const auto dark = icon.pixmap(16, 16).toImage();
    const auto highDpi = icon.pixmap(QSize(16, 16), 2.0);
    assert(highDpi.size() == QSize(32, 32) && highDpi.devicePixelRatio() == 2.0);
    assert(hasInk(highDpi.toImage()));
    palette.setColor(QPalette::Base, Qt::white); app.setPalette(palette);
    assert(dark != icon.pixmap(16, 16).toImage()); // existing icon follows theme changes
    assert(icon.pixmap(16, 16).toImage() != icon.pixmap(16, 16, QIcon::Disabled).toImage());
    assert(hasInk(icon.pixmap(16, 16, QIcon::Selected).toImage()));
}
