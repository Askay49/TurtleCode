#include "app/IconDialogs.hpp"
#include "app/I18n.hpp"
#include "ThemeManager.hpp"
#include "WorkspaceBackground.hpp"
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStyle>
#include <algorithm>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QHBoxLayout>

static void initializeHighlightResources() { Q_INIT_RESOURCE(turtle_code_highlighting); }
namespace {
QString themeDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/themes";
}
QByteArray encode(const EditorTheme& theme) {
    QJsonObject colors;
    for (auto it = theme.colors.cbegin(); it != theme.colors.cend(); ++it)
        colors.insert(it.key(), it.value().name());
    QJsonObject styles; for (auto it=theme.fontStyles.cbegin();it!=theme.fontStyles.cend();++it) styles.insert(it.key(),it.value());
    return QJsonDocument(QJsonObject{{"formatVersion", 1}, {"name", theme.name}, {"colors", colors}, {"fontStyles",styles}}).toJson();
}
bool decode(const QByteArray& bytes, const EditorTheme& base, EditorTheme* output, QString* error) {
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = I18n::text("Invalid JSON theme file."); return false;
    }
    const auto obj = doc.object();
    if (obj.value("formatVersion").toInt(-1) != 1 || !obj.value("colors").isObject() ||
        !obj.value("name").isString() || obj.value("name").toString().trimmed().isEmpty()) {
        *error = I18n::text("Expected formatVersion: 1, a name and a colors object."); return false;
    }
    *output = base;
    output->name = obj.value("name").toString().trimmed();
    const auto colors = obj.value("colors").toObject();
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const auto value = it.value().toString();
        const QColor color(value);
        if (value.size() != 7 || !value.startsWith('#') || !color.isValid()) {
            *error = I18n::text("Invalid color for ") + it.key() + I18n::text(". Use #RRGGBB."); return false;
        }
        if (output->colors.contains(it.key()) || QStringList{
            "terminal.background", "terminal.foreground", "terminal.cursor",
            "terminal.selectionBackground", "terminal.selectionForeground"}.contains(it.key()))
            output->colors[it.key()] = color;
    }
    if (obj.contains("fontStyles")) {
        if (!obj.value("fontStyles").isObject()) { *error=I18n::text("Invalid theme font styles."); return false; }
        const auto styles=obj.value("fontStyles").toObject();
        for(auto it=styles.begin();it!=styles.end();++it) {
            const int flags=it.value().toInt(-1);
            if (!it.value().isDouble() || flags<0 || flags>15 || it.value().toDouble()!=flags) { *error=I18n::text("Invalid theme font styles."); return false; }
            if(output->colors.contains(it.key())) output->fontStyles[it.key()]=flags;
        }
    }
    return true;
}
}
ThemeManager& ThemeManager::instance() { static ThemeManager manager; return manager; }
ThemeManager::ThemeManager() {
    initializeHighlightResources();
    // Palettes adapted for Turtle Code; these are not the original VS Code extensions.
    auto add = [this](QString id, QString name, const char* bg, const char* fg, const char* panel,
                      const char* accent, const char* keyword, const char* string, const char* comment,
                      const char* function, const char* type, const char* number) {
        const QColor background(bg), foreground(fg);
        const bool dark = background.lightness() < 128;
        EditorTheme t{id, I18n::text(name), {
            {"window.background", QColor(panel)}, {"text.primary", foreground},
            {"text.muted", QColor(comment)}, {"editor.background", background},
            {"editor.currentLine", dark ? background.lighter(125) : background.darker(105)},
            {"editor.selection", dark ? QColor(accent).darker(210) : QColor(accent).lighter(165)},
            {"editor.cursor", foreground}, {"sidebar.background", QColor(panel)},
            {"border", dark ? QColor(panel).lighter(155) : QColor(panel).darker(120)},
            {"accent", QColor(accent)}, {"accent.text", QColor(dark ? "#ffffff" : "#000000")},
            {"search.match", dark ? QColor("#584c28") : QColor("#fff0ad")},
            {"editor.occurrence", dark ? QColor(accent).darker(260) : QColor(accent).lighter(180)},
            {"search.activeMatch", dark ? QColor("#b0802a") : QColor("#ffd166")},
            {"diagnostic.error", QColor(dark ? "#f14c4c" : "#c62828")},
            {"diagnostic.warning", QColor(dark ? "#cca700" : "#936500")},
            {"syntax.keyword", QColor(keyword)}, {"syntax.string", QColor(string)},
            {"syntax.comment", QColor(comment)}, {"syntax.function", QColor(function)},
            {"syntax.type", QColor(type)}, {"syntax.number", QColor(number)},
            {"syntax.parameter", foreground}, {"syntax.variable", foreground}, {"syntax.preprocessor", QColor(keyword)},
            {"syntax.constant", QColor(number)}, {"syntax.property", QColor(type)},
            {"syntax.builtinType", QColor(type)}, {"syntax.boolean", QColor(number)},
            {"syntax.operator", QColor(keyword)}, {"syntax.punctuation", foreground},
            {"syntax.controlKeyword",QColor(keyword)}, {"syntax.functionKeyword",QColor(keyword)},
            {"syntax.macro",QColor(keyword)}, {"syntax.namespace",QColor(type)}, {"syntax.special",QColor(type)},
            {"syntax.builtinVariable",QColor(keyword)}, {"syntax.bracket",foreground}, {"syntax.escape",QColor(keyword)},
            {"syntax.regexp",QColor(string)}, {"syntax.character",QColor(string)}, {"syntax.interface",QColor(type)}
        }};
        themes_.push_back(t);
    };
    add("cpp-dark", I18n::text("C/C++ — Dark"), "#1e1e1e", "#d4d4d4", "#252526", "#007acc", "#c586c0", "#ce9178", "#6a9955", "#dcdcaa", "#4ec9b0", "#b5cea8");
    add("one-dark", "One Dark Pro", "#282c34", "#abb2bf", "#21252b", "#61afef", "#c678dd", "#98c379", "#7f848e", "#61afef", "#e5c07b", "#d19a66");
    add("github", I18n::text("GitHub — Light"), "#ffffff", "#24292f", "#f6f8fa", "#0969da", "#cf222e", "#0a3069", "#6e7781", "#8250df", "#953800", "#0550ae");
    add("dracula", "Dracula", "#282a36", "#f8f8f2", "#21222c", "#bd93f9", "#ff79c6", "#f1fa8c", "#6272a4", "#50fa7b", "#8be9fd", "#bd93f9");
    add("winter", I18n::text("Winter is Coming — Dark"), "#011627", "#d6deeb", "#01111d", "#219fd5", "#c792ea", "#ecc48d", "#637777", "#82aaff", "#addb67", "#f78c6c");
    add("monokai", I18n::text("Monokai Pro — inspired"), "#2d2a2e", "#fcfcfa", "#221f22", "#ffd866", "#ff6188", "#ffd866", "#939293", "#a9dc76", "#78dce8", "#ab9df2");
    add("night-owl", "Night Owl", "#011627", "#d6deeb", "#01111d", "#7e57c2", "#c792ea", "#ecc48d", "#637777", "#82aaff", "#addb67", "#f78c6c");
    add("ayu", "Ayu — Mirage", "#1f2430", "#cbccc6", "#191e2a", "#ffcc66", "#ffa759", "#bae67e", "#707a8c", "#ffd580", "#73d0ff", "#d4bfff");
    add("purple", "Shades of Purple", "#2d2b55", "#ffffff", "#25234a", "#fad000", "#ff9d00", "#a5ff90", "#b0a9d8", "#fad000", "#9effff", "#ff628c");
    add("noctis", "Noctis", "#052529", "#b2cacd", "#042024", "#16b0a5", "#c695c6", "#d5971a", "#6b9299", "#72b7c0", "#49e9a6", "#f49d76");
    // Tokyo Night by Enkia: https://github.com/tokyo-night/tokyo-night-vscode-theme
    add("tokyo-night", "Tokyo Night", "#1a1b26", "#a9b1d6", "#16161e", "#7aa2f7", "#bb9af7", "#9ece6a", "#565f89", "#7aa2f7", "#7dcfff", "#ff9e64");
    QFile palette(":/highlighting/themes/tokyo-night-vscode.json");
    if (palette.open(QIODevice::ReadOnly)) {
        const auto object=QJsonDocument::fromJson(palette.readAll()).object();
        const auto colors=object.value("colors").toObject();
        for(auto it=colors.begin();it!=colors.end();++it) themes_.last().colors[it.key()]=QColor(it.value().toString());
        const auto styles=object.value("fontStyles").toObject();
        for(auto it=styles.begin();it!=styles.end();++it) themes_.last().fontStyles[it.key()]=it.value().toInt();
    }
    // Turtle Code terminal palettes coordinated with each built-in editor theme.
    const QStringList palettes = {
        "#252526 #cd6666 #8fb573 #dcdcaa #569cd6 #c586c0 #4ec9b0 #b8b8b8",
        "#21252b #e06c75 #98c379 #e5c07b #61afef #c678dd #56b6c2 #abb2bf",
        "#24292f #cf222e #116329 #7d4e00 #0969da #8250df #1b7c83 #6e7781",
        "#21222c #ff5555 #50fa7b #f1fa8c #8be9fd #bd93f9 #80ffea #d7d7d2",
        "#01111d #ef6b73 #addb67 #ecc48d #82aaff #c792ea #7fdbca #b0c0cf",
        "#221f22 #ff6188 #a9dc76 #ffd866 #78dce8 #ab9df2 #78dce8 #d0cdd0",
        "#01111d #ef5350 #addb67 #ecc48d #82aaff #c792ea #7fdbca #b0c0cf",
        "#191e2a #f28779 #bae67e #ffd580 #73d0ff #d4bfff #95e6cb #b0b1ab",
        "#25234a #ff628c #a5ff90 #fad000 #9effff #d5a6ff #80fcff #d6d2ee",
        "#042024 #e5786d #49e9a6 #d5971a #72b7c0 #c695c6 #16b0a5 #97b4b8",
        "#15161e #f7768e #9ece6a #e0af68 #7aa2f7 #bb9af7 #7dcfff #a9b1d6"
    };
    for (int t = 0; t < themes_.size(); ++t) {
        auto& colors = themes_[t].colors;
        const auto ansi = palettes[t].split(' ');
        const bool dark = colors["editor.background"].lightness() < 128;
        for (int i = 0; i < 8; ++i) {
            const QColor normal(ansi[i]);
            colors[QString("terminal.ansi%1").arg(i)] = normal;
            const QColor bright = i == 0 ? colors["text.muted"]
                : i == 7 ? colors["text.primary"] : dark ? normal.lighter(115) : normal.darker(115);
            colors[QString("terminal.ansi%1").arg(i + 8)] = QColor::fromRgb(bright.rgb());
        }
    }
    current_ = themes_.front();
}
QColor ThemeManager::color(const QString& role) const {
    const auto found = current_.colors.constFind(role);
    if (found != current_.colors.cend()) return *found;
    // Optional terminal overrides inherit editor colors, including in older custom themes.
    static const QMap<QString, QString> inherited = {
        {"editor.occurrence", "editor.selection"},
        {"terminal.background", "editor.background"}, {"terminal.foreground", "text.primary"},
        {"terminal.cursor", "editor.cursor"}, {"terminal.selectionBackground", "editor.selection"},
        {"terminal.selectionForeground", "text.primary"}
    };
    return current_.colors.value(inherited.value(role, role), QColor("#d4d4d4"));
}
void ThemeManager::refreshAppearance() {
    WorkspaceBackground::instance().reload();
    apply(current_);
}
void ThemeManager::initialize() {
    WorkspaceBackground::instance().reload();
    while (!themes_.isEmpty() && themes_.last().id.startsWith("custom-")) themes_.removeLast();
    const EditorTheme base = themes_.front();
    const QDir directory(themeDirectory());
    for (const auto& fileName : directory.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile file(directory.filePath(fileName));
        EditorTheme theme; QString error;
        if (file.open(QIODevice::ReadOnly) && file.size() <= 1024 * 1024 && decode(file.readAll(), base, &theme, &error)) {
            theme.id = "custom-" + QFileInfo(fileName).completeBaseName();
            themes_.push_back(theme);
        }
    }
    const QString selected = QSettings().value("appearance/theme", base.id).toString();
    for (const auto& theme : themes_) if (theme.id == selected) { apply(theme); return; }
    apply(base);
}
void ThemeManager::apply(const EditorTheme& theme) {
    current_ = theme;
    // Unpolish the previous stylesheet before installing the new palette.
    // Otherwise Qt can restore its cached palette on a later font change.
    qApp->setStyleSheet(QString());
    if (qApp->style()->objectName().compare("fusion", Qt::CaseInsensitive) != 0)
        qApp->setStyle("Fusion");
    QPalette p;
    auto c = [this](const char* role) { return color(QString::fromLatin1(role)); };
    p.setColor(QPalette::Window, c("window.background")); p.setColor(QPalette::WindowText, c("text.primary"));
    p.setColor(QPalette::Base, c("editor.background")); p.setColor(QPalette::AlternateBase, c("editor.currentLine"));
    p.setColor(QPalette::Text, c("text.primary")); p.setColor(QPalette::Button, c("window.background"));
    p.setColor(QPalette::ButtonText, c("text.primary")); p.setColor(QPalette::Highlight, c("editor.selection"));
    p.setColor(QPalette::HighlightedText, c("text.primary")); p.setColor(QPalette::Link, c("accent"));
    p.setColor(QPalette::ToolTipBase, c("window.background")); p.setColor(QPalette::ToolTipText, c("text.primary"));
    p.setColor(QPalette::Disabled, QPalette::Text, c("text.muted"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c("text.muted"));
    qApp->setPalette(p);
    // Establish the unstyled palette from parents to children before Qt caches
    // it during stylesheet polish. Later font changes must restore this palette.
    auto widgets = QApplication::allWidgets();
    const auto depth = [](QWidget* widget) {
        int result = 0;
        while ((widget = widget->parentWidget())) ++result;
        return result;
    };
    std::sort(widgets.begin(), widgets.end(), [&](QWidget* a, QWidget* b) { return depth(a) < depth(b); });
    for (QWidget* widget : widgets)
        widget->setPalette(p);
    QString stylesheet = QString(
        "QWidget { color:%1; background:%2; }"
        "QLineEdit, QComboBox, QSpinBox, QTableWidget { background:%7; }"
        "QMenu::item:selected, QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background:%6; }"
        "QTreeView::item:hover, QListView::item:hover, QTableView::item:hover { background:%6; }"
        // Keep Qt's stylesheet renderer active in every state. Without an
        // explicit base background, native painting ignores item padding until
        // hover/selection adds a background, shifting icons and text.
        "QMenu::item, QListView::item, QTableView::item { border:0; background:transparent; }"
        "QToolTip { color:%1; background:%2; border:1px solid %3; }"
        "QTreeView, QStackedWidget { background:%4; }"
        "QLineEdit, QComboBox, QSpinBox { padding:4px; border:1px solid %3; }"
        "QSpinBox { padding-right:24px; }"
        "QSpinBox::up-button { subcontrol-origin:border; subcontrol-position:top right; width:22px; border:0; }"
        "QSpinBox::down-button { subcontrol-origin:border; subcontrol-position:bottom right; width:22px; border:0; }"
        "QLineEdit:focus, QComboBox:focus { border-color:%5; }"
        "QToolButton { border:0; padding:4px; background:transparent; }"
        "QToolButton:hover { background:%6; }"
        "QToolButton:pressed, QToolButton:checked { background:%6; }"
        "QToolButton:disabled { color:%8; }"
        "QTabWidget#editorTabs::pane { border:0; margin:0; padding:0; }"
        "QTabWidget#editorTabs::tab-bar { left:0; top:0; }"
        "QWidget#tabGutter { background:%4; }"
        "QTabBar::tab { color:%8; border:0; border-top:2px solid transparent; margin:0; padding:7px 14px; min-height:20px; }"
        "QTabBar::tab:hover { color:%1; background:%6; }"
        "QTabBar::tab:selected { color:%1; background:%7; border-top-color:%5; }"
        "QTreeView::item { padding:2px; border:0; background:transparent; }"
        "QFrame#fileFindBar { background:%2; border:1px solid %3; }")
        .arg(c("text.primary").name(), c("window.background").name(), c("border").name(),
             c("sidebar.background").name(), c("accent").name(), c("editor.selection").name(), c("editor.background").name(), c("text.muted").name());
    if (WorkspaceBackground::instance().enabled()) {
        stylesheet += QStringLiteral(
            "QWidget#workspaceBackground, QWidget#workspaceBackground QWidget { background:transparent; }"
            "QWidget#workspaceBackground QTabBar::tab { background:transparent; }"
            "QWidget#workspaceBackground QTabBar::tab:selected { background:rgba(127,127,127,35); }"
            "QWidget#workspaceBackground QTreeView::item:selected, "
            "QWidget#workspaceBackground QToolButton:checked { background:%1; }"
            "QWidget#workspaceBackground QLineEdit, QWidget#workspaceBackground QComboBox, "
            "QWidget#workspaceBackground QSpinBox { background:%2; }"
            "QWidget#workspaceBackground QMenu, QWidget#workspaceBackground QMenu QWidget, "
            "QWidget#workspaceBackground QDialog, QWidget#workspaceBackground QDialog QWidget, "
            "QWidget#workspaceBackground QComboBox QAbstractItemView { background:%2; }"
            "QWidget#workspaceBackground QMenu::item:selected { background:%1; }")
            .arg(c("editor.selection").name(), c("editor.background").name());
    }
    // Keep chrome states after the wallpaper transparency rules. No fixed palette:
    // custom, light and dark themes all use their existing semantic colors.
    stylesheet += QStringLiteral(
        "QWidget#workspaceBackground QToolButton:hover { background:%1; }"
        "QWidget#workspaceBackground QToolButton:pressed { background:%1; }"
        "QWidget#activityBar { border-right:1px solid %2; }"
        "QWidget#activityBar QToolButton { border:0; border-left:2px solid transparent; padding:0 2px 0 0; }"
        "QWidget#workspaceBackground QWidget#activityBar QToolButton:checked { border-left:2px solid %3; background:transparent; }"
        "QWidget#workspaceBackground QWidget#activityBar QToolButton:hover { background:%1; }"
        "QWidget#activityBar QToolButton:focus { border-left:2px solid %3; }"
        "QLabel#explorerHeading { color:%4; padding:7px 12px; min-height:20px; border-bottom:1px solid %2; }"
        "QWidget#workspaceBackground QTreeView { border:0; outline:0; }"
        "QWidget#workspaceBackground QTreeView::item { padding:3px 4px; }"
        "QWidget#workspaceBackground QTreeView::item:hover { background:%1; }"
        "QWidget#workspaceBackground QTreeView::item:selected { background:%1; }"
        "QWidget#workspaceBackground QTabBar::tab:hover { background:%1; }"
        "QWidget#gitPanel QTreeWidget { border:0; outline:0; }"
        "QWidget#gitPanel QTreeWidget::item { border:0; padding:0 2px; }"
        "QWidget#workspaceBackground QWidget#gitPanel QTreeWidget::item { padding:0 2px; }"
        "QWidget#gitHeading { min-height:26px; }"
        "QLabel#gitHeadingTitle { color:%4; font-size:11px; }"
        "QLabel#gitRepositoryName { font-weight:600; }"
        "QWidget#gitPanel QToolButton { padding:0; border:0; }"
        "QWidget#gitPanel QComboBox#gitBranches { border:0; padding:0 12px 0 2px; background:transparent; }"
        "QComboBox#gitBranches::drop-down { border:0; width:12px; }"
        "QWidget#gitPanel QPlainTextEdit#gitCommitMessage { border:1px solid %2; padding:3px 5px; }"
        "QWidget#gitPanel QPushButton#gitCommit { border:0; border-radius:2px; min-height:28px; padding:0 8px; background:%3; color:%4; }"
        "QWidget#gitPanel QToolButton#gitCommitMore { border-left:1px solid %2; border-radius:2px; background:%3; color:%4; }"
        "QWidget#gitPanel QPushButton#gitCommit:disabled { color:%4; background:%1; }"
        "QWidget#debugHeading { border:0; min-height:28px; }"
        "QLabel#debugHeadingTitle { color:%4; font-size:11px; }"
        "QWidget#debugPanel QToolButton { padding:0; border:0; }"
        "QWidget#debugPanel QToolButton::menu-indicator { image:none; width:0; }"
        "QWidget#debugPanel QComboBox#debugConfigurations { border:0; padding:2px 16px 2px 3px; background:transparent; min-height:22px; }"
        "QWidget#workspaceBackground QWidget#debugPanel QComboBox#debugConfigurations { background:transparent; }"
        "QComboBox#debugConfigurations::drop-down { border:0; width:16px; }"
        "QComboBox#debugConfigurations::down-arrow { image:none; }"
        "QWidget#debugSectionHeader { border:0; border-top:1px solid %2; }"
        "QWidget#debugSectionHeader QToolButton { text-align:left; padding:0; font-size:11px; font-weight:600; }"
        "QWidget#debugSectionHeader QToolButton:checked { background:transparent; }"
        "QWidget#workspaceBackground QWidget#debugSectionHeader QToolButton:checked { background:transparent; }"
        "QWidget#debugPanel QTreeWidget { border:0; outline:0; }"
        "QWidget#debugPanel QTreeWidget::item { padding:2px 3px; border:0; min-height:18px; }"
        "QWidget#workspaceBackground QWidget#debugPanel QTreeWidget::item { padding:2px 3px; }"
        "QWidget#debugPanel QTreeWidget::item:hover { background:%1; }"
        "QWidget#debugPanel QTreeWidget::item:selected { background:%1; }"
        "QSplitter#debugSections::handle { background:%2; }"
        "QSplitter#debugSections::handle:hover { background:%3; }"
        "QLabel#debugStatus { color:%4; padding:4px 10px; }"
        "QTabWidget#terminalTabs::pane { border:0; margin:0; padding:0; }"
        "QTabWidget#terminalTabs::tab-bar { left:0; top:0; }"
        "QTabWidget#terminalTabs QTabBar::tab { color:%4; background:transparent; border:0; border-bottom:1px solid transparent; padding:8px 12px; }"
        "QTabWidget#terminalTabs QTabBar::tab:selected { color:%3; border-bottom-color:%3; background:transparent; }"
        "QWidget#workspaceBackground QTabWidget#terminalTabs QTabBar::tab:selected { background:transparent; }"
        "QLabel#terminalShellName { color:%4; background:transparent; padding:0 3px; }"
        "QWidget#terminalActions { border:0; }"
        "QWidget#workspaceBackground QStatusBar { border-top:1px solid %2; padding:1px 6px; }"
        "QStatusBar::item { border:0; }"
        "QSplitter#workspaceSplitter::handle { background:%2; }"
        "QSplitter#workspaceSplitter::handle:hover { background:%3; }"
        "QWidget#workspaceBackground QMainWindow#editorArea::separator { background:%2; border:0; image:none; height:3px; width:3px; }"
        "QWidget#workspaceBackground QMainWindow#editorArea::separator:hover { background:%3; }"
        "QWidget#workspaceBackground QScrollBar:vertical { background:transparent; width:10px; margin:0; border:0; }"
        "QWidget#workspaceBackground QScrollBar:horizontal { background:transparent; height:10px; margin:0; border:0; }"
        "QWidget#workspaceBackground QScrollBar::handle:vertical { background:%4; min-height:28px; border:2px solid transparent; border-radius:4px; }"
        "QWidget#workspaceBackground QScrollBar::handle:horizontal { background:%4; min-width:28px; border:2px solid transparent; border-radius:4px; }"
        "QWidget#workspaceBackground QScrollBar::handle:hover { background:%3; }"
        "QWidget#workspaceBackground QScrollBar::handle:disabled { background:transparent; }"
        "QWidget#workspaceBackground QScrollBar::add-line, QWidget#workspaceBackground QScrollBar::sub-line { width:0; height:0; border:0; background:transparent; }"
        "QWidget#workspaceBackground QScrollBar::add-page, QWidget#workspaceBackground QScrollBar::sub-page { background:transparent; }"
    ).arg(c("editor.selection").name(), c("border").name(), c("accent").name(), c("text.muted").name());
    const auto accent=c("accent");
    const auto buttonText=accent.lightnessF()>0.55?QStringLiteral("#101018"):QStringLiteral("#ffffff");
    stylesheet += QStringLiteral("QWidget#gitPanel QPushButton#gitCommit:enabled, QWidget#gitPanel QToolButton#gitCommitMore:enabled { color:%1; } QLabel#gitError { color:%2; }")
        .arg(buttonText,c("diagnostic.error").name());
    stylesheet += QStringLiteral("QWidget#workspaceBackground QLabel#gitCount { background:%1; color:%2; border-radius:7px; font-size:10px; padding:0; }")
        .arg(accent.name(),buttonText);
    qApp->setStyleSheet(stylesheet);
    // Stylesheet polish may reset the application palette; keep future widgets
    // on the same baseline as the widgets already present.
    qApp->setPalette(p);
    for (QWidget* widget : QApplication::allWidgets()) {
        widget->update();
    }
    emit themeChanged();
}
bool ThemeManager::saveCustom(EditorTheme theme, QString* error) {
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(themeDirectory())) { *error = I18n::text("Cannot create themes directory."); return false; }
    QSaveFile file(themeDirectory() + '/' + uuid + ".json");
    if (!file.open(QIODevice::WriteOnly) || file.write(encode(theme)) == -1 || !file.commit()) {
        *error = file.errorString(); return false;
    }
    theme.id = "custom-" + uuid;
    themes_.push_back(theme);
    apply(theme);
    QSettings().setValue("appearance/theme", theme.id);
    return true;
}
void ThemeManager::showSettings(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(I18n::text("Theme")); dialog.resize(620, 620);
    auto* layout = new QVBoxLayout(&dialog);
    auto* info = new QLabel(I18n::text("Built-in palettes adapted for Turtle Code. Double-click colors to preview; save a custom copy to keep changes."), &dialog);
    info->setWordWrap(true); layout->addWidget(info);
    auto* combo = new QComboBox(&dialog);
    for (const auto& theme : themes_) combo->addItem(theme.name, theme.id);
    combo->setCurrentIndex(combo->findData(current_.id));
    auto* remove = new QPushButton(I18n::text("Delete theme…"), &dialog); remove->setIcon(Codicons::icon("trash"));
    remove->setObjectName("deleteCustomTheme");
    auto* selector = new QHBoxLayout;
    selector->addWidget(combo, 1); selector->addWidget(remove); layout->addLayout(selector);
    auto* table = new QTableWidget(&dialog); table->setColumnCount(2);
    table->setHorizontalHeaderLabels({I18n::text("Color role"), I18n::text("Color — double-click to edit")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(table);
    auto refresh = [this, table, remove] {
        remove->setEnabled(current_.id.startsWith("custom-"));
        table->setRowCount(current_.colors.size()); int row = 0;
        for (auto it = current_.colors.cbegin(); it != current_.colors.cend(); ++it, ++row) {
            table->setItem(row, 0, new QTableWidgetItem(it.key()));
            auto* item = new QTableWidgetItem(it.value().name()); item->setBackground(it.value());
            item->setForeground(it.value().lightness() < 128 ? Qt::white : Qt::black); table->setItem(row, 1, item);
        }
    };
    refresh();
    bool draft = false;
    auto confirmDiscard = [&] {
        return !draft || IconDialogs::MessageBox::question(&dialog, I18n::text("Unsaved colors"), I18n::text("Discard unsaved color changes?"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard;
    };
    QObject::connect(combo, qOverload<int>(&QComboBox::activated), &dialog, [&](int index) {
        if (!confirmDiscard()) { combo->setCurrentIndex(combo->findData(current_.id)); return; }
        apply(themes_.at(index)); QSettings().setValue("appearance/theme", current_.id); draft = false; refresh();
    });
    QObject::connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int index=combo->currentIndex();
        if (index<0 || index>=themes_.size() || !themes_[index].id.startsWith("custom-")) return;
        const auto theme=themes_[index];
        if (IconDialogs::MessageBox::question(&dialog, I18n::text("Delete theme"),
                I18n::text("Delete custom theme “%1”? This cannot be undone.").arg(theme.name),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)!=QMessageBox::Yes) return;
        QFile file(QDir(themeDirectory()).filePath(theme.id.mid(7)+".json"));
        if (!file.remove()) {
            IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot delete theme"), file.errorString()); return;
        }
        themes_.removeAt(index); combo->removeItem(index);
        apply(themes_.front());
        QSettings().setValue("appearance/theme",current_.id);
        combo->setCurrentIndex(0); draft=false; refresh();
    });
    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int row, int) {
        const QString role = table->item(row, 0)->text();
        const QColor selected = IconDialogs::ColorDialog::getColor(color(role), &dialog, role);
        if (selected.isValid()) { auto theme = current_; theme.colors[role] = selected; apply(theme); draft = true; refresh(); }
    });
    auto* buttons = new QHBoxLayout; layout->addLayout(buttons);
    auto* save = new QPushButton(I18n::text("Save custom copy…"), &dialog); save->setIcon(Codicons::icon("save"));
    auto* import = new QPushButton(I18n::text("Import JSON…"), &dialog); import->setIcon(Codicons::icon("folder-opened"));
    auto* exportButton = new QPushButton(I18n::text("Export JSON…"), &dialog); exportButton->setIcon(Codicons::icon("export"));
    buttons->addWidget(save); buttons->addWidget(import); buttons->addWidget(exportButton);
    auto appendSaved = [&] { combo->addItem(current_.name, current_.id); combo->setCurrentIndex(combo->count()-1); draft = false; refresh(); };
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] {
        bool ok; const auto name = IconDialogs::InputDialog::getText(&dialog, I18n::text("Custom theme"), I18n::text("Name:"), QLineEdit::Normal, current_.name + I18n::text(" (custom)"), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        auto theme = current_; theme.name = name; QString error;
        if (!saveCustom(theme, &error)) IconDialogs::MessageBox::warning(&dialog, I18n::text("Theme"), error); else appendSaved();
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        if (!confirmDiscard()) return;
        const auto path = IconDialogs::FileDialog::getOpenFileName(&dialog, I18n::text("Import Turtle Code theme"), {}, "JSON (*.json)");
        if (path.isEmpty()) return;
        QFile file(path); QString error; EditorTheme theme;
        if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
        else if (file.size() > 1024 * 1024) error = I18n::text("Theme file exceeds 1 MiB.");
        else if (decode(file.readAll(), themes_.front(), &theme, &error) && saveCustom(theme, &error)) { appendSaved(); return; }
        IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot import theme"), error);
    });
    QObject::connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        auto path = IconDialogs::FileDialog::getSaveFileName(&dialog, I18n::text("Export theme"), "theme.json", "JSON (*.json)");
        if (path.isEmpty()) return;
        if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(encode(current_)) == -1 || !file.commit())
            IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot export theme"), file.errorString());
    });
    auto* close = new IconDialogs::ButtonBox(QDialogButtonBox::Close, &dialog); layout->addWidget(close);
    QObject::connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    // Unsaved previews are session-only; restore the last persisted selection on close.
    if (draft) {
        const QString id = QSettings().value("appearance/theme", themes_.front().id).toString();
        for (const auto& theme : themes_) if (theme.id == id) { apply(theme); break; }
    }
}

HighlightStyle ThemeManager::highlightStyle(const QString& role) const {
    return {color(role),current_.fontStyles.value(role,0)};
}
HighlightStyle ThemeManager::captureStyle(const QString& capture,const QString& fallbackRole) const {
    static const QMap<QString,QString> roles{
        {"comment","syntax.comment"},{"string","syntax.string"},{"string.escape","syntax.escape"},{"string.regexp","syntax.regexp"},
        {"character","syntax.character"},{"number","syntax.number"},{"boolean","syntax.boolean"},
        {"variable","syntax.variable"},{"variable.parameter","syntax.parameter"},{"parameter","syntax.parameter"},
        {"variable.member","syntax.property"},{"variable.builtin","syntax.builtinVariable"},{"property","syntax.property"},
        {"function","syntax.function"},{"function.builtin","syntax.special"},{"function.macro","syntax.macro"},{"method","syntax.function"},
        {"constant","syntax.constant"},{"constant.builtin","syntax.special"},{"constant.macro","syntax.macro"},
        {"type","syntax.type"},{"type.builtin","syntax.builtinType"},{"type.qualifier","syntax.keyword"},
        {"module","syntax.namespace"},{"namespace","syntax.namespace"},
        {"keyword","syntax.keyword"},{"keyword.conditional","syntax.controlKeyword"},{"keyword.repeat","syntax.controlKeyword"},
        {"keyword.exception","syntax.controlKeyword"},{"keyword.function","syntax.functionKeyword"},{"keyword.operator","syntax.operator"},
        {"keyword.directive","syntax.preprocessor"},{"keyword.import","syntax.preprocessor"},
        {"operator","syntax.operator"},{"preprocessor","syntax.preprocessor"},{"attribute","syntax.preprocessor"},
        {"punctuation","syntax.punctuation"},{"delimiter","syntax.punctuation"},{"punctuation.bracket","syntax.bracket"},
        {"label","syntax.function"},{"tag","syntax.type"},{"constructor","syntax.functionKeyword"}
    };
    QString name=capture;
    while(!name.isEmpty()) {
        if(roles.contains(name)) return highlightStyle(roles.value(name));
        const auto dot=name.lastIndexOf('.'); if(dot<0) break; name=name.left(dot);
    }
    return highlightStyle(fallbackRole);
}
HighlightStyle ThemeManager::semanticStyle(const QString& type,const QStringList& modifiers) const {
    QString role;
    const bool builtin=modifiers.contains("defaultLibrary");
    if(type=="parameter") role="syntax.parameter";
    else if(type=="property" || type=="field") role="syntax.property";
    else if(type=="function" || type=="method") role=builtin?"syntax.special":"syntax.function";
    else if(type=="macro") role=builtin?"syntax.special":"syntax.macro";
    else if(type=="namespace") role="syntax.namespace";
    else if(type=="builtinType") role="syntax.builtinType";
    else if(type=="type" || type=="class" || type=="struct" || type=="enum" || type=="typeAlias" || type=="typeParameter") role=builtin?"syntax.builtinType":"syntax.type";
    else if(type=="interface") role="syntax.interface";
    else if(type=="enumMember") role=builtin?"syntax.special":"syntax.constant";
    else if(type=="variable") {
        if(builtin) role="syntax.builtinVariable";
        else if(modifiers.contains("callable")) role="syntax.function";
        else if(modifiers.contains("static")) role="syntax.constant";
        // Like Tokyo Night's @lsp.type.variable, preserve base syntax for ordinary variables.
    } else if(type=="selfKeyword" || type=="selfTypeKeyword") role="syntax.builtinVariable";
    else if(type=="keyword" || type=="modifier" || type=="lifetime") role="syntax.keyword";
    else if(type=="comment") role="syntax.comment";
    else if(type=="string") role="syntax.string";
    else if(type=="number") role="syntax.number";
    else if(type=="boolean") role="syntax.boolean";
    else if(type=="operator") role="syntax.operator";
    else if(type=="decorator" || type=="deriveHelper") role="syntax.preprocessor";
    else if(type=="escapeSequence") role="syntax.escape";
    else if(type=="regexp") role="syntax.regexp";
    HighlightStyle result=role.isEmpty()?HighlightStyle{}:highlightStyle(role);
    if(modifiers.contains("deprecated")) result.fontStyles|=8;
    return result;
}
