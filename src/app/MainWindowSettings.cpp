#include "MainWindow.hpp"

#include "Codicons.hpp"
#include "FontSettings.hpp"
#include "I18n.hpp"
#include "IconDialogs.hpp"
#include "SettingsSpinBox.hpp"
#include "ShortcutManager.hpp"
#include "ThemeManager.hpp"
#include "editor/EditorWidget.hpp"
#include "editor/OutlinePanel.hpp"
#include "git/GitPanel.hpp"
#include "language/GrammarManager.hpp"
#include "language/LanguageRegistry.hpp"
#include "language/LspSettingsDialog.hpp"
#include "project/ProjectExplorer.hpp"
#include "project/WorkspaceIndex.hpp"
#include "search/SearchPanel.hpp"
#include "terminal/TerminalWidget.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabWidget>

#include <algorithm>
#include <vector>

QWidget* MainWindow::createHighlightingSettings(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* description = new QLabel(
        I18n::text("Enable a language to download its Tree-sitter grammar automatically. Installation happens in the background; files remain usable as plain text while a grammar is unavailable."), page);
    description->setObjectName("description");
    description->setWordWrap(true);
    root->addWidget(description);

    auto* immediate = new QLabel(I18n::text("Highlighting changes take effect immediately."), page);
    immediate->setWordWrap(true);
    root->addWidget(immediate);

    auto* status = new QLabel(I18n::text("Select a language to install or enable its grammar."), page);
    status->setObjectName("status");
    status->setWordWrap(true);
    root->addWidget(status);
    connect(grammarManager_, &GrammarManager::statusChanged, status,
            [status](const QString&, const QString& message) { status->setText(message); });

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(4, 4, 4, 4);
    contentLayout->setSpacing(8);

    struct LanguageToggle { QString id; QString name; QPointer<QCheckBox> checkbox; };
    QVector<LanguageToggle> toggles;
    QString currentCategory;
    for (const auto& language : LanguageRegistry::languages()) {
        if (language.category != currentCategory) {
            currentCategory = language.category;
            auto* categoryLabel = new QLabel(currentCategory, content);
            QFont categoryFont = categoryLabel->font();
            categoryFont.setBold(true);
            categoryFont.setPointSize(categoryFont.pointSize() + 1);
            categoryLabel->setFont(categoryFont);
            contentLayout->addWidget(categoryLabel);
        }
        const bool installed = GrammarManager::isInstalled(language.id);
        auto* checkbox = new QCheckBox(language.name + (installed ? I18n::text("  · Installed") : I18n::text("  · Not installed")), content);
        checkbox->setChecked(installed && LanguageRegistry::isEnabled(language.id));
        checkbox->setToolTip(installed ? I18n::text("Toggle syntax highlighting for this language.")
                                      : I18n::text("Enable to download and install the official Tree-sitter grammar."));
        contentLayout->addWidget(checkbox);
        toggles.push_back({language.id, language.name, checkbox});

        connect(checkbox, &QCheckBox::toggled, page,
                [this, id = language.id, name = language.name, checkbox, status](bool enabled) {
            if (!enabled) {
                LanguageRegistry::setEnabled(id, false);
                for (int i = 0; i < tabs_->count(); ++i)
                    if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
                return;
            }
            if (GrammarManager::isInstalled(id)) {
                LanguageRegistry::setEnabled(id, true);
                for (int i = 0; i < tabs_->count(); ++i)
                    if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
                status->setText(I18n::text("Language enabled."));
                return;
            }
            LanguageRegistry::setEnabled(id, true);
            checkbox->setText(name + I18n::text("  · Installing…"));
            status->setText(I18n::text("Preparing grammar download…"));
            grammarManager_->install(id);
        });
    }
    contentLayout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* note = new QLabel(
        I18n::text("Grammars are downloaded from their public Tree-sitter repositories, compiled locally, and stored in your user data directory. A C compiler and tar are required for installation. The IDE itself does not need to be rebuilt."), page);
    note->setObjectName("note");
    note->setWordWrap(true);
    root->addWidget(note);

    connect(grammarManager_, &GrammarManager::installationFinished, page,
            [this, toggles, status](const QString& id, bool success) {
        for (const auto& toggle : toggles) {
            if (toggle.id != id || !toggle.checkbox) continue;
            toggle.checkbox->setEnabled(true);
            if (success) {
                toggle.checkbox->setText(toggle.name + I18n::text("  · Installed"));
                const bool enabled = LanguageRegistry::isEnabled(id);
                {
                    const QSignalBlocker blocker(toggle.checkbox);
                    toggle.checkbox->setChecked(enabled);
                }
                status->setText(enabled ? I18n::text("Grammar installed and language enabled.")
                                        : I18n::text("Grammar installed; language remains disabled."));
            } else {
                toggle.checkbox->setText(toggle.name + I18n::text("  · Not installed"));
                const QSignalBlocker blocker(toggle.checkbox);
                toggle.checkbox->setChecked(false);
            }
        }
        if (success) {
            for (int i = 0; i < tabs_->count(); ++i)
                if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(i))) editor->refreshSyntaxHighlighting();
        }
    });

    return page;
}

void MainWindow::applyLspSettings(bool serverChanged) {
    const bool semantic=QSettings().value("lsp/semanticHighlighting",true).toBool();
    for(int i=0;i<tabs_->count();++i) if(auto* editor=qobject_cast<EditorWidget*>(tabs_->widget(i))) {
        editor->hideCompletions();
        if(!semantic) {lsp_->cancelSemanticTokens(editor->filePath());editor->clearSemanticHighlighting();}
    }
    if(serverChanged) configureLsp();
    else requestSemanticHighlighting(activeEditor(),true);
}

void MainWindow::applyFileVisibilitySettings() {
    const QSettings settings;
    const bool showHidden = settings.value("view/showHidden", false).toBool();
    const bool includeIgnored = settings.value("project/includeIgnored", false).toBool();
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = qobject_cast<MainWindow*>(widget);
        if (!window) continue;
        window->findChild<QAction*>("view.hidden")->setChecked(showHidden);
        window->findChild<QAction*>("view.ignored")->setChecked(includeIgnored);
        window->explorer_->setShowHidden(showHidden);
        window->explorer_->setIncludeIgnored(includeIgnored);
        window->searchPanel_->setShowHidden(showHidden);
        window->searchPanel_->setIncludeIgnored(includeIgnored);
        window->workspaceIndex_->configure(window->explicitProject_ ? window->projectRoots_ : QStringList{},
                                           {showHidden, includeIgnored, {}});
    }
}

void MainWindow::showSettings(SettingsPage initialPage) {
    QSettings settings;
    QMap<QString, QKeySequence> shortcuts;
    QDialog dialog(this);
    dialog.setObjectName("applicationSettings");
    dialog.setWindowTitle(I18n::text("Settings"));

    auto* root = new QVBoxLayout(&dialog);
    auto* body = new QHBoxLayout;
    root->addLayout(body, 1);

    auto* sections = new QListWidget(&dialog);
    sections->setObjectName("settingsSections");
    sections->setSelectionMode(QAbstractItemView::SingleSelection);
    sections->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sections->setUniformItemSizes(true);
    body->addWidget(sections);

    auto* pages = new QStackedWidget(&dialog);
    pages->setObjectName("settingsPages");
    body->addWidget(pages, 1);

    int navigationWidth = 0;
    const auto addSection = [&](SettingsPage section, const char* id, const char* title, const char* icon, QWidget* page) {
        auto* item = new QListWidgetItem(Codicons::icon(icon), I18n::text(title), sections);
        item->setData(Qt::UserRole, QString::fromLatin1(id));
        item->setData(Qt::UserRole + 1, int(section));
        const auto metrics = sections->fontMetrics();
        navigationWidth = std::max(navigationWidth, metrics.horizontalAdvance(item->text()));
        item->setSizeHint(QSize(0, metrics.height() + 14));
        pages->addWidget(page);
    };
    const auto scrollPage = [](QWidget* content) {
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(content);
        return scroll;
    };
    const auto checkBox = [&](QWidget* parent, const char* name, const char* label, const char* key, bool fallback) {
        auto* box = new QCheckBox(I18n::text(label), parent);
        box->setObjectName(name);
        box->setChecked(settings.value(key, fallback).toBool());
        return box;
    };

    auto* editor = new QWidget;
    auto* editorLayout = new QVBoxLayout(editor);
    auto* typing = new QGroupBox(I18n::text("Typing"), editor);
    auto* typingLayout = new QFormLayout(typing);
    auto* autoPairs = checkBox(typing, "autoPairs", "Automatically close brackets and quotes", "editor/autoPairs", true);
    auto* automatic = checkBox(typing, "lspAutoCompletion", "Automatically show completions while typing", "lsp/autoCompletion", true);
    auto* delay = new SettingsSpinBox(typing);
    delay->setObjectName("completionDelayMs");
    delay->setRange(50, 2000);
    delay->setValue(settings.value("lsp/completionDelayMs", 150).toInt());
    delay->setEnabled(automatic->isChecked());
    connect(automatic, &QCheckBox::toggled, delay, &QSpinBox::setEnabled);
    typingLayout->addRow(autoPairs);
    typingLayout->addRow(automatic);
    typingLayout->addRow(I18n::text("Completion delay, ms"), delay);
    editorLayout->addWidget(typing);

    auto* vimGroup = new QGroupBox(I18n::text("Vim mode"), editor);
    auto* vimLayout = new QVBoxLayout(vimGroup);
    auto* vim = checkBox(vimGroup, "vimModeEnabled", "Enable Vim mode", "editor/vimMode", false);
    vim->setToolTip(I18n::text("Normal: i/a to insert, v to select. Escape returns to Normal."));
    vimLayout->addWidget(vim);
    editorLayout->addWidget(vimGroup);
    editorLayout->addStretch();
    addSection(SettingsPage::Editor, "editor", "Editor", "edit", scrollPage(editor));

    auto* appearance = new QTabWidget;
    appearance->setObjectName("appearanceSettingsTabs");
    auto* interfacePage = new QWidget;
    auto* interfaceLayout = new QVBoxLayout(interfacePage);
    auto* fonts = new QGroupBox(I18n::text("Fonts"), interfacePage);
    auto* fontLayout = new QFormLayout(fonts);
    fontLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    struct FontRow {
        FontSettings::Area area;
        QFontComboBox* family;
        QSpinBox* size;
        QString initialFamily;
        int initialSize;
    };
    std::vector<FontRow> fontRows;
    const auto addFontRow = [&](FontSettings::Area area, const char* label, const QString& name) {
        const auto selected = FontSettings::font(area);
        auto* row = new QWidget(fonts);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* family = new QFontComboBox(row);
        family->setObjectName(name + "FontFamily");
        family->setCurrentFont(selected);
        family->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        family->setMinimumContentsLength(14);
        auto* size = new SettingsSpinBox(row);
        size->setObjectName(name + "FontSize");
        size->setRange(6, 96);
        size->setSuffix(I18n::text(" pt"));
        size->setValue(selected.pointSize());
        rowLayout->addWidget(family, 1);
        rowLayout->addWidget(size);
        fontLayout->addRow(I18n::text(label), row);
        fontRows.push_back({area, family, size, family->currentFont().family(), size->value()});
    };
    addFontRow(FontSettings::Area::Tree, "Project tree:", "tree");
    addFontRow(FontSettings::Area::Editor, "Editor text:", "editor");
    addFontRow(FontSettings::Area::Terminal, "Terminal:", "terminal");
    interfaceLayout->addWidget(fonts);

    auto* background = new QGroupBox(I18n::text("Workspace background"), interfacePage);
    auto* backgroundLayout = new QFormLayout(background);
    backgroundLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* imageRow = new QWidget(background);
    auto* imageLayout = new QHBoxLayout(imageRow);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    auto* imagePath = new QLineEdit(settings.value("appearance/backgroundImage").toString(), imageRow);
    imagePath->setObjectName("backgroundImage");
    imagePath->setReadOnly(true);
    imagePath->setPlaceholderText(I18n::text("No image selected"));
    auto* browse = new QPushButton(Codicons::icon("file-media"), I18n::text("Choose image…"), imageRow);
    auto* remove = new QPushButton(Codicons::icon("trash"), I18n::text("Remove"), imageRow);
    remove->setObjectName("removeBackgroundImage");
    imageLayout->addWidget(browse);
    imageLayout->addWidget(remove);
    backgroundLayout->addRow(I18n::text("Image:"), imagePath);
    backgroundLayout->addRow(QString{}, imageRow);
    auto* opacity = new SettingsSpinBox(background);
    opacity->setObjectName("backgroundOpacity");
    opacity->setRange(0, 100);
    opacity->setSuffix(" %");
    opacity->setValue(settings.value("appearance/backgroundOpacity", 15).toInt());
    backgroundLayout->addRow(I18n::text("Opacity:"), opacity);
    auto* backgroundNote = new QLabel(I18n::text("One image behind the project tree, text and panels. Scaled to fill the workspace without distortion."), background);
    backgroundNote->setWordWrap(true);
    backgroundLayout->addRow(backgroundNote);
    interfaceLayout->addWidget(background);
    connect(remove, &QPushButton::clicked, imagePath, &QLineEdit::clear);
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, imagePath] {
        QStringList patterns;
        for (const auto& format : QImageReader::supportedImageFormats()) patterns << "*." + QString::fromLatin1(format);
        const auto path = IconDialogs::FileDialog::getOpenFileName(&dialog, I18n::text("Choose background image"),
            imagePath->text(), I18n::text("Images (%1)").arg(patterns.join(' ')));
        if (!path.isEmpty()) imagePath->setText(path);
    });

    auto* interfaceLanguage = new QGroupBox(I18n::text("Interface language"), interfacePage);
    auto* languageLayout = new QFormLayout(interfaceLanguage);
    auto* language = new QComboBox(interfaceLanguage);
    language->setObjectName("interfaceLanguage");
    language->addItem("English", "en");
    language->addItem(QString::fromUtf8("Русский"), "ru");
    language->setCurrentIndex(language->findData(I18n::selectedLanguage()));
    languageLayout->addRow(I18n::text("Language:"), language);
    auto* restart = new QLabel(I18n::text("Restart Turtle Code to apply the selected interface language."), interfaceLanguage);
    restart->setWordWrap(true);
    languageLayout->addRow(restart);
    interfaceLayout->addWidget(interfaceLanguage);
    interfaceLayout->addStretch();
    appearance->addTab(scrollPage(interfacePage), I18n::text("Interface"));
    appearance->addTab(scrollPage(ThemeManager::instance().createSettingsPage(appearance, &dialog)), I18n::text("Theme"));
    addSection(SettingsPage::Appearance, "appearance", "Appearance", "color-mode", appearance);

    auto& shortcutManager = ShortcutManager::instance();
    auto* shortcutPage = shortcutManager.createSettingsPage(pages, shortcuts);
    const auto originalShortcuts = shortcuts;
    addSection(SettingsPage::Shortcuts, "shortcuts", "Keyboard shortcuts", "keyboard", scrollPage(shortcutPage));

    auto* languages = new QTabWidget;
    languages->setObjectName("languageSettingsTabs");
    languages->addTab(createHighlightingSettings(languages), I18n::text("Syntax highlighting"));
    auto* lsp = new LspSettingsPage(lsp_, languages);
    languages->addTab(scrollPage(lsp), I18n::text("Language servers"));
    addSection(SettingsPage::Highlighting, "languages", "Languages and tools", "code", languages);

    auto* files = new QWidget;
    auto* filesLayout = new QVBoxLayout(files);
    auto* preview = checkBox(files, "settingsPreviewFiles", "Preview files on single click", "editor/previewFiles", false);
    auto* pinOnEdit = checkBox(files, "settingsPinPreviewOnEdit", "Keep preview files open after editing", "editor/pinPreviewOnEdit", true);
    pinOnEdit->setEnabled(preview->isChecked());
    connect(preview, &QCheckBox::toggled, pinOnEdit, &QCheckBox::setEnabled);
    filesLayout->addWidget(preview);
    auto* previewOptions = new QHBoxLayout;
    previewOptions->addSpacing(20);
    previewOptions->addWidget(pinOnEdit);
    filesLayout->addLayout(previewOptions);
    filesLayout->addSpacing(10);
    auto* hidden = checkBox(files, "settingsShowHidden", "Show Hidden Files", "view/showHidden", false);
    auto* ignored = checkBox(files, "settingsIncludeIgnored", "Include ignored files", "project/includeIgnored", false);
    const auto addFilter = [&](QCheckBox* box, const char* description) {
        filesLayout->addWidget(box);
        auto* note = new QLabel(I18n::text(description), files);
        note->setWordWrap(true);
        filesLayout->addWidget(note);
    };
    addFilter(hidden, "Show hidden files in the project tree, search, and Quick Open.");
    filesLayout->addSpacing(10);
    addFilter(ignored, "Include files ignored by Git in the project tree, search, and Quick Open.");
    filesLayout->addStretch();
    addSection(SettingsPage::Files, "files", "Files and search", "files", scrollPage(files));

    auto* history = new QWidget;
    auto* historyLayout = new QVBoxLayout(history);
    auto* undo = new QGroupBox(I18n::text("Undo and redo"), history);
    auto* undoLayout = new QFormLayout(undo);
    undoLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* memory = new SettingsSpinBox(undo);
    memory->setObjectName("historyMemoryMiB");
    memory->setRange(1, 4096);
    memory->setValue(settings.value("editor/historyMemoryMiB", 64).toInt());
    undoLayout->addRow(I18n::text("Undo/Redo memory budget, MiB"), memory);
    auto* spill = checkBox(undo, "historySpillToDisk", "Store history exceeding the memory budget on disk", "editor/historySpillToDisk", true);
    undoLayout->addRow(spill);
    auto* disk = new SettingsSpinBox(undo);
    disk->setObjectName("historyDiskMiB");
    disk->setRange(0, 1048576);
    disk->setSpecialValueText(I18n::text("Unlimited"));
    disk->setValue(settings.value("editor/historyDiskMiB", 0).toInt());
    disk->setEnabled(spill->isChecked());
    connect(spill, &QCheckBox::toggled, disk, &QSpinBox::setEnabled);
    undoLayout->addRow(I18n::text("History disk budget, MiB (0: unlimited)"), disk);
    auto* historyNote = new QLabel(I18n::text("When disk storage is disabled or its limit is reached, the oldest Undo/Redo actions are removed. Changes apply to open documents. Temporary history is deleted when its document closes."), undo);
    historyNote->setWordWrap(true);
    undoLayout->addRow(historyNote);
    historyLayout->addWidget(undo);

    auto* recovery = new QGroupBox(I18n::text("Recovery"), history);
    auto* recoveryLayout = new QVBoxLayout(recovery);
    auto* backups = checkBox(recovery, "recoveryEnabled", "Automatically back up unsaved documents", "editor/recoveryEnabled", true);
    backups->setToolTip(I18n::text("Existing recovery copies are kept when disabled. Workspace and cursor positions are still remembered."));
    recoveryLayout->addWidget(backups);
    historyLayout->addWidget(recovery);
    historyLayout->addStretch();
    addSection(SettingsPage::History, "history", "History and recovery", "history", scrollPage(history));

    const auto selectPage = [&](SettingsPage page) {
        if (page == SettingsPage::Theme) {
            appearance->setCurrentIndex(1);
            page = SettingsPage::Appearance;
        } else if (page == SettingsPage::LanguageServers) {
            languages->setCurrentIndex(1);
            page = SettingsPage::Highlighting;
        }
        const auto last = settings.value("interface/settingsSection").toString();
        for (int row = 0; row < sections->count(); ++row) {
            auto* item = sections->item(row);
            if (page == SettingsPage::Last ? item->data(Qt::UserRole).toString() == last
                                           : item->data(Qt::UserRole + 1).toInt() == int(page)) {
                sections->setCurrentRow(row);
                return;
            }
        }
        sections->setCurrentRow(0);
    };
    connect(sections, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    if (initialPage != SettingsPage::Appearance)
        appearance->setCurrentIndex(settings.value("interface/settingsAppearanceTab", 0).toInt());
    if (initialPage != SettingsPage::Highlighting)
        languages->setCurrentIndex(settings.value("interface/settingsLanguagesTab", 0).toInt());
    selectPage(initialPage);
    connect(&dialog, &QDialog::finished, &dialog, [&](int) {
        QSettings navigation;
        navigation.setValue("interface/settingsSection", sections->currentItem()->data(Qt::UserRole));
        navigation.setValue("interface/settingsAppearanceTab", appearance->currentIndex());
        navigation.setValue("interface/settingsLanguagesTab", languages->currentIndex());
    });

    auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const bool backgroundChanged = settings.value("appearance/backgroundImage").toString() != imagePath->text()
            || settings.value("appearance/backgroundOpacity", 15).toInt() != opacity->value();
        if (backgroundChanged && !imagePath->text().isEmpty()) {
            QImageReader reader(imagePath->text());
            reader.setAutoTransform(true);
            if (reader.read().isNull()) {
                appearance->setCurrentIndex(0);
                selectPage(SettingsPage::Appearance);
                IconDialogs::MessageBox::warning(&dialog, I18n::text("Workspace background"),
                    I18n::text("Cannot open this image. Choose another file or remove the background."));
                return;
            }
        }
        const bool shortcutsChanged = shortcuts != originalShortcuts;
        if (shortcutsChanged) {
            const auto error = shortcutManager.validate(shortcuts);
            if (!error.isEmpty()) {
                selectPage(SettingsPage::Shortcuts);
                IconDialogs::MessageBox::warning(&dialog, I18n::text("Cannot save shortcuts"), error);
                return;
            }
        }
        if (!lsp->prepareSave()) {
            selectPage(SettingsPage::LanguageServers);
            return;
        }

        const auto selectedLanguage = language->currentData().toString();
        const bool languageChanged = selectedLanguage != I18n::selectedLanguage();
        const bool visibilityChanged = hidden->isChecked() != settings.value("view/showHidden", false).toBool()
            || ignored->isChecked() != settings.value("project/includeIgnored", false).toBool();
        const bool previewChanged = preview->isChecked() != settings.value("editor/previewFiles", false).toBool()
            || pinOnEdit->isChecked() != settings.value("editor/pinPreviewOnEdit", true).toBool();
        const bool vimChanged = vim->isChecked() != settings.value("editor/vimMode", false).toBool();
        const bool historyChanged = memory->value() != settings.value("editor/historyMemoryMiB", 64).toInt()
            || disk->value() != settings.value("editor/historyDiskMiB", 0).toInt()
            || spill->isChecked() != settings.value("editor/historySpillToDisk", true).toBool();
        const bool recoveryChanged = backups->isChecked() != settings.value("editor/recoveryEnabled", true).toBool();
        const bool semanticBeforeSave = settings.value("lsp/semanticHighlighting", true).toBool();
        const bool completionChanged = automatic->isChecked() != settings.value("lsp/autoCompletion", true).toBool()
            || delay->value() != settings.value("lsp/completionDelayMs", 150).toInt();
        bool fontsChanged = false;
        for (const auto& row : fontRows) {
            const auto prefix = FontSettings::key(row.area);
            const auto family = row.family->currentFont().family();
            if (family == row.initialFamily && row.size->value() == row.initialSize) continue;
            fontsChanged = true;
            settings.setValue(prefix + "family", family);
            settings.setValue(prefix + "size", row.size->value());
        }
        if (languageChanged) settings.setValue("interface/language", selectedLanguage);
        if (shortcutsChanged) shortcutManager.writeSettings(settings, shortcuts);
        settings.setValue("appearance/backgroundImage", imagePath->text());
        settings.setValue("appearance/backgroundOpacity", opacity->value());
        settings.setValue("view/showHidden", hidden->isChecked());
        settings.setValue("project/includeIgnored", ignored->isChecked());
        settings.setValue("editor/previewFiles", preview->isChecked());
        settings.setValue("editor/pinPreviewOnEdit", pinOnEdit->isChecked());
        settings.setValue("editor/historyMemoryMiB", memory->value());
        settings.setValue("editor/historySpillToDisk", spill->isChecked());
        settings.setValue("editor/historyDiskMiB", disk->value());
        settings.setValue("editor/vimMode", vim->isChecked());
        settings.setValue("editor/autoPairs", autoPairs->isChecked());
        settings.setValue("editor/recoveryEnabled", backups->isChecked());
        settings.setValue("lsp/autoCompletion", automatic->isChecked());
        settings.setValue("lsp/completionDelayMs", delay->value());
        lsp->writeSettings(settings);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            IconDialogs::MessageBox::warning(&dialog, I18n::text("Settings"), I18n::text("Cannot save settings."));
            return;
        }

        if (shortcutsChanged) shortcutManager.applySettings(shortcuts);
        if (vimChanged) applyVimSettings();
        if (visibilityChanged) applyFileVisibilitySettings();
        if (backgroundChanged) ThemeManager::instance().refreshAppearance();
        const bool lspChanged = lsp->serverChanged || completionChanged
            || semanticBeforeSave != settings.value("lsp/semanticHighlighting", true).toBool();
        for (auto* widget : QApplication::allWidgets()) {
            if (auto* window = qobject_cast<MainWindow*>(widget)) {
                if (previewChanged) {
                    const auto previews = window->filePreviews_;
                    for (auto* editor : previews) {
                        if (!preview->isChecked() || (pinOnEdit->isChecked() && editor->isModified()))
                            window->pinFilePreview(editor);
                    }
                }
                if (recoveryChanged) window->applyRecoverySettings();
                if (lspChanged) window->applyLspSettings(lsp->serverChanged);
                if (fontsChanged) {
                    window->searchPanel_->applyFontSettings();
                    window->outline_->applyFontSettings();
                    window->gitPanel_->applyFontSettings();
                }
            }
            if (auto* document = qobject_cast<EditorWidget*>(widget)) {
                if (fontsChanged) document->applyFontSettings();
                if (historyChanged && !document->isBusy()) document->applyHistorySettings();
            } else if (fontsChanged) {
                if (auto* terminal = qobject_cast<TerminalWidget*>(widget)) terminal->applyFontSettings();
                if (auto* tree = qobject_cast<ProjectExplorer*>(widget)) tree->setFont(FontSettings::font(FontSettings::Area::Tree));
            }
        }
        if (languageChanged && selectedLanguage != I18n::activeLanguage())
            statusBar()->showMessage(I18n::text("Restart Turtle Code to apply the selected interface language."), 8000);
        dialog.accept();
    });

    for (auto* button : dialog.findChildren<QPushButton*>()) button->setAutoDefault(false);
    const QSize available = screen()->availableGeometry().size() - QSize(40, 60);
    sections->setFixedWidth(navigationWidth + 44);
    dialog.resize(QSize(1150, 780).boundedTo(available));
    sections->setFocus();
    dialog.exec();
}
