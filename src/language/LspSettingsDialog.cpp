#include "app/IconDialogs.hpp"
#include "LspSettingsDialog.hpp"
#include "LanguageRegistry.hpp"
#include "app/I18n.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include "app/SettingsSpinBox.hpp"
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString quotedArguments(const QStringList& arguments) {
    QStringList result;
    for (auto argument : arguments) {
        // QProcess::splitCommand uses triple quotes for a literal quote.
        argument.replace('"', "\"\"\"");
        result.append('"' + argument + '"');
    }
    return result.join(' ');
}
}
LspSettingsDialog::LspSettingsDialog(LspManager* manager, QWidget* parent) : QDialog(parent) {
    setWindowTitle(I18n::text("Language servers")); resize(980, 760);
    // The dialog owns a draft; Cancel never modifies active profiles.
    auto draft = std::make_shared<LspConfiguration>(LspConfiguration::load());
    const auto original = draft->json();
    auto* layout = new QVBoxLayout(this);
    auto* global = new QHBoxLayout;
    auto* enabled = new QCheckBox(I18n::text("Enable language servers"), this);
    enabled->setObjectName("lspEnabled"); enabled->setChecked(QSettings().value("lsp/enabled",true).toBool());
    auto* semantic = new QCheckBox(I18n::text("Semantic highlighting (LSP)"), this);
    semantic->setObjectName("semanticHighlighting"); semantic->setChecked(QSettings().value("lsp/semanticHighlighting",true).toBool());
    global->addWidget(enabled); global->addWidget(semantic); global->addStretch(); layout->addLayout(global);
    auto* completionRow = new QHBoxLayout;
    auto* automatic = new QCheckBox(I18n::text("Automatically show completions while typing"), this);
    automatic->setChecked(QSettings().value("lsp/autoCompletion",true).toBool());
    auto* delay = new SettingsSpinBox(this); delay->setObjectName("completionDelayMs"); delay->setRange(50,2000);
    delay->setValue(std::clamp(QSettings().value("lsp/completionDelayMs",150).toInt(),50,2000));
    delay->setEnabled(automatic->isChecked()); connect(automatic,&QCheckBox::toggled,delay,&QSpinBox::setEnabled);
    completionRow->addWidget(automatic); completionRow->addWidget(new QLabel(I18n::text("Completion delay, ms"),this)); completionRow->addWidget(delay); completionRow->addStretch(); layout->addLayout(completionRow);
    auto* splitter = new QSplitter(this); layout->addWidget(splitter,1);
    auto* languages = new QListWidget(splitter); languages->setObjectName("lspLanguages");
    for (const auto& language : LanguageRegistry::languages()) {
        auto* item = new QListWidgetItem(language.name,languages); item->setData(Qt::UserRole,language.id);
    }
    auto* right = new QWidget(splitter); auto* rightLayout = new QVBoxLayout(right);
    auto* profileRow = new QHBoxLayout;
    auto* profiles = new QComboBox(right); profiles->setObjectName("lspProfile");
    auto* add = new QPushButton(I18n::text("Add profile"),right); add->setIcon(Codicons::icon("add")); add->setObjectName("lspAddProfile");
    auto* remove = new QPushButton(I18n::text("Delete profile"),right); remove->setIcon(Codicons::icon("trash"));
    profileRow->addWidget(profiles,1); profileRow->addWidget(add); profileRow->addWidget(remove); rightLayout->addLayout(profileRow);
    auto* fields = new QGroupBox(I18n::text("Server profile (shared by assigned languages)"),right);
    auto* form = new QFormLayout(fields);
    auto* profileEnabled = new QCheckBox(I18n::text("Enable this profile"),fields); profileEnabled->setObjectName("lspProfileEnabled"); form->addRow(profileEnabled);
    auto* name = new QLineEdit(fields); name->setObjectName("lspProfileName"); form->addRow(I18n::text("Profile name"),name);
    auto* commandRow = new QWidget(fields); auto* commandLayout = new QHBoxLayout(commandRow); commandLayout->setContentsMargins(0,0,0,0);
    auto* command = new QLineEdit(commandRow); command->setObjectName("lspCommand");
    auto* browse = new QPushButton(I18n::text("Browse..."),commandRow); browse->setIcon(Codicons::icon("folder-opened")); commandLayout->addWidget(command,1); commandLayout->addWidget(browse);
    form->addRow(I18n::text("Server executable"),commandRow);
    auto* arguments = new QLineEdit(fields); arguments->setObjectName("lspArguments"); form->addRow(I18n::text("Arguments (quote paths containing spaces)"),arguments);
    auto* directory = new QLineEdit(fields); directory->setObjectName("lspCompileDirectory");
    auto* directoryLabel = new QLabel(I18n::text("clangd compilation database directory"),fields); form->addRow(directoryLabel,directory);
    auto* logging = new QComboBox(fields); logging->setObjectName("lspLogLevel");
    logging->addItems({I18n::text("Off"),I18n::text("Normal (16 KiB, memory only)"),I18n::text("Detailed (1 MiB, memory only)")}); form->addRow(I18n::text("Logging"),logging);
    rightLayout->addWidget(fields);
    auto* pages = new QTabWidget(right); rightLayout->addWidget(pages,1);
    auto* journalPage = new QWidget(pages); auto* journalLayout = new QVBoxLayout(journalPage);
    auto* status = new QLabel(journalPage); status->setObjectName("lspProfileStatus"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); status->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum); journalLayout->addWidget(status);
    auto* actions = new QHBoxLayout;
    auto* check = new QPushButton(I18n::text("Check connection"),journalPage); check->setIcon(Codicons::icon("plug")); check->setObjectName("lspCheckConnection");
    auto* restart = new QPushButton(I18n::text("Restart server"),journalPage); restart->setIcon(Codicons::icon("debug-restart"));
    auto* clear = new QPushButton(I18n::text("Clear log"),journalPage); clear->setIcon(Codicons::icon("clear-all"));
    actions->addWidget(check); actions->addWidget(restart); actions->addWidget(clear); actions->addStretch(); journalLayout->addLayout(actions);
    auto* journal = new QPlainTextEdit(journalPage); journal->setObjectName("lspLog"); journal->setReadOnly(true); journalLayout->addWidget(journal,1);
    auto* logNote = new QLabel(I18n::text("Logs are never written to disk. Normal: 8 KiB for errors + 8 KiB for events per server instance. Detailed messages may contain document text."),journalPage); logNote->setWordWrap(true); journalLayout->addWidget(logNote);
    pages->addTab(journalPage,I18n::text("Status and log"));
    auto* advanced = new QWidget(pages); auto* advancedLayout = new QFormLayout(advanced);
    auto* languageId = new QLineEdit(advanced); languageId->setObjectName("lspLanguageId"); languageId->setPlaceholderText(I18n::text("Automatic")); advancedLayout->addRow(I18n::text("Language ID for selected language"),languageId);
    auto* initialization = new QPlainTextEdit(advanced); initialization->setObjectName("lspInitializationOptions");
    auto* serverSettings = new QPlainTextEdit(advanced); serverSettings->setObjectName("lspServerSettings");
    advancedLayout->addRow(I18n::text("Initialization options (JSON object)"),initialization);
    advancedLayout->addRow(I18n::text("Server settings (JSON object)"),serverSettings);
    pages->addTab(advanced,I18n::text("Advanced"));
    splitter->setStretchFactor(0,0); splitter->setStretchFactor(1,1); splitter->setSizes({240,720});
    auto* note = new QLabel(I18n::text("Install servers yourself, then select their executable and arguments. Documents choose their server automatically. A successful connection does not guarantee support for every feature."),this); note->setWordWrap(true); layout->addWidget(note);
    auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); layout->addWidget(buttons);

    struct State { QString language, profile; bool loading{false}; QPointer<LspClient> probe; };
    auto state = std::make_shared<State>();
    auto findProfile = [draft](const QString& id) -> LspProfile* { for(auto& profile:draft->profiles) if(profile.id==id) return &profile; return nullptr; };
    auto readObject = [this](QPlainTextEdit* editor,QJsonObject* value) {
        if(editor->toPlainText().trimmed().isEmpty()) { *value={}; return true; }
        QJsonParseError error; const auto document=QJsonDocument::fromJson(editor->toPlainText().toUtf8(),&error);
        if(error.error!=QJsonParseError::NoError || !document.isObject()) {
            IconDialogs::MessageBox::warning(this,I18n::text("Language servers"),I18n::text("Enter a valid JSON object.")+"\n"+error.errorString()); editor->setFocus(); return false;
        }
        *value=document.object(); return true;
    };
    auto store = [=,this] {
        if(state->loading) return true;
        if(!state->language.isEmpty()) {
            const auto id=languageId->text().trimmed();
            if(id.isEmpty()) draft->languageIds.remove(state->language); else draft->languageIds[state->language]=id;
        }
        auto* profile=findProfile(state->profile); if(!profile) return true;
        QJsonObject init,settings;
        if(!readObject(initialization,&init) || !readObject(serverSettings,&settings)) return false;
        profile->server.name=name->text().trimmed();
        if(profile->server.name.isEmpty()) profile->server.name=profile->id;
        profile->enabled=profileEnabled->isChecked(); profile->server.command=command->text().trimmed();
        profile->server.arguments=QProcess::splitCommand(arguments->text());
        if(profile->id=="clangd" && !directory->text().trimmed().isEmpty()) profile->server.arguments.append("--compile-commands-dir="+directory->text().trimmed());
        profile->server.initializationOptions=init; profile->server.settings=settings; profile->server.logLevel=logging->currentIndex();
        return true;
    };
    auto refresh = [=,this] {
        for(int i=0;i<languages->count();++i) {
            auto* item=languages->item(i); const auto id=item->data(Qt::UserRole).toString(); const auto* language=LanguageRegistry::language(id);
            const auto* profile=findProfile(draft->bindings.value(id));
            QString stateText=I18n::text("Not configured");
            if(profile) {
                if(!enabled->isChecked() || !profile->enabled) stateText=I18n::text("Disabled");
                else if(!profile->server.command.isEmpty()) {
                    stateText=profile->server.name;
                    const auto running=manager->clients(profile->id);
                    for(auto* client:running) if(client->ready()) {stateText+= " — "+I18n::text("Ready");break;}
                    if(!running.isEmpty() && std::none_of(running.begin(),running.end(),[](auto* client){return client->ready();})) stateText+=" — "+running.first()->status();
                }
            }
            item->setText(language->name+" — "+stateText);
        }
        QString text,logText;
        if(state->probe) { text=state->probe->status(); logText=state->probe->log(); }
        else {
            const auto clients=manager->clients(state->profile);
            if(clients.isEmpty()) text=I18n::text("Not started");
            for(auto* client:clients) { if(!text.isEmpty()) text+='\n'; text+=client->status(); }
            logText=manager->log(state->profile);
        }
        auto* client=state->probe.data();
        if(!client) for(auto* candidate:manager->clients(state->profile)) if(candidate->ready()) {client=candidate;break;}
        if(client && client->ready()) {
            const auto caps=client->capabilities(); QStringList features;
            const QList<QPair<QString,QString>> names{{"completionProvider",I18n::text("Completion")},{"hoverProvider",I18n::text("Hover")},{"definitionProvider",I18n::text("Definition")},{"signatureHelpProvider",I18n::text("Signature help")},{"renameProvider",I18n::text("Rename")},{"documentFormattingProvider",I18n::text("Formatting")},{"semanticTokensProvider",I18n::text("Semantic tokens")}};
            for(const auto& pair:names) {const auto value=caps.value(pair.first);if(value.isObject() || value.toBool()) features.append(pair.second);}
            text+='\n'+I18n::text("Server capabilities: ")+features.join(", ");
        }
        status->setText(text);
        if(journal->toPlainText()!=logText) journal->setPlainText(logText);
    };
    auto load = [=,this](const QString& id) {
        state->loading=true; state->profile=id;
        const auto* profile=findProfile(id); fields->setEnabled(profile); initialization->setEnabled(profile); serverSettings->setEnabled(profile); remove->setEnabled(profile); check->setEnabled(profile); restart->setEnabled(profile);
        name->setText(profile?profile->server.name:QString{}); command->setText(profile?profile->server.command:QString{});
        auto args=profile?profile->server.arguments:QStringList{}; QString compileDirectory;
        if(id=="clangd") for(auto it=args.begin();it!=args.end();) {if(it->startsWith("--compile-commands-dir=")){compileDirectory=it->mid(23);it=args.erase(it);}else ++it;}
        arguments->setText(quotedArguments(args)); directory->setText(compileDirectory); directory->setVisible(id=="clangd"); directoryLabel->setVisible(id=="clangd");
        profileEnabled->setChecked(profile && profile->enabled); logging->setCurrentIndex(profile?profile->server.logLevel:1);
        initialization->setPlainText(QString::fromUtf8(QJsonDocument(profile?profile->server.initializationOptions:QJsonObject{}).toJson(QJsonDocument::Indented)));
        serverSettings->setPlainText(QString::fromUtf8(QJsonDocument(profile?profile->server.settings:QJsonObject{}).toJson(QJsonDocument::Indented)));
        languageId->setText(draft->languageIds.value(state->language)); state->loading=false; refresh();
    };
    auto populate = [=,this](const QString& selected) {
        state->loading=true; profiles->clear(); profiles->addItem(I18n::text("No server"),QString{});
        for(const auto& profile:draft->profiles) profiles->addItem(profile.server.name,profile.id);
        profiles->setCurrentIndex(std::max(0,profiles->findData(selected))); state->loading=false; load(selected);
    };
    connect(languages,&QListWidget::currentRowChanged,this,[=,this](int row) {
        if(row<0 || state->loading) return;
        if(!store()) {state->loading=true;for(int i=0;i<languages->count();++i)if(languages->item(i)->data(Qt::UserRole).toString()==state->language)languages->setCurrentRow(i);state->loading=false;return;}
        if(state->probe) {delete state->probe.data();state->probe=nullptr;}
        state->language=languages->item(row)->data(Qt::UserRole).toString(); populate(draft->bindings.value(state->language));
    });
    connect(profiles,qOverload<int>(&QComboBox::currentIndexChanged),this,[=,this](int) {
        if(state->loading) return;
        if(!store()) {state->loading=true;profiles->setCurrentIndex(std::max(0,profiles->findData(state->profile)));state->loading=false;return;}
        if(state->probe) {delete state->probe.data();state->probe=nullptr;}
        const auto id=profiles->currentData().toString(); draft->bindings[state->language]=id; load(id);
    });
    connect(add,&QPushButton::clicked,this,[=,this] {
        if(!store())return;
        bool ok=false; const auto text=IconDialogs::InputDialog::getText(this,I18n::text("Add profile"),I18n::text("Profile name"),QLineEdit::Normal,{},&ok).trimmed();
        if(!ok || text.isEmpty())return;
        LspProfile profile;profile.id=QUuid::createUuid().toString(QUuid::WithoutBraces);profile.server.name=text;profile.server.command.clear();profile.server.arguments.clear();profile.server.languageIds.clear();
        if(state->probe){delete state->probe.data();state->probe=nullptr;}
        draft->profiles.append(profile);draft->bindings[state->language]=profile.id;populate(profile.id);
    });
    connect(remove,&QPushButton::clicked,this,[=,this] {
        const auto id=state->profile;if(id.isEmpty())return;
        if(state->probe){delete state->probe.data();state->probe=nullptr;}
        for(auto it=draft->profiles.begin();it!=draft->profiles.end();++it)if(it->id==id){draft->profiles.erase(it);break;}
        for(auto& binding:draft->bindings) if(binding==id) binding.clear();
        populate({});
    });
    connect(browse,&QPushButton::clicked,this,[=,this] { const auto path=IconDialogs::FileDialog::getOpenFileName(this,I18n::text("Server executable"),command->text());if(!path.isEmpty())command->setText(path); });
    connect(check,&QPushButton::clicked,this,[=,this] {
        if(!store())return;
        const auto* profile=findProfile(state->profile);if(!profile)return;
        if(profile->server.command.isEmpty() || QStandardPaths::findExecutable(profile->server.command).isEmpty()) {
            IconDialogs::MessageBox::warning(this,I18n::text("Language servers"),I18n::text("Choose an existing server executable."));return;
        }
        if(state->probe)delete state->probe.data();
        state->probe=new LspClient(profile->server,this);state->probe->setRoot(manager->root());state->probe->checkConnection();refresh();
    });
    connect(restart,&QPushButton::clicked,this,[=,this] { manager->restart(state->profile);if(state->probe)state->probe->checkConnection();refresh(); });
    connect(clear,&QPushButton::clicked,this,[=,this] { manager->clearLog(state->profile);if(state->probe)state->probe->clearLog();refresh(); });
    connect(enabled,&QCheckBox::toggled,this,[=,this]{refresh();});
    auto* timer=new QTimer(this);timer->setInterval(500);connect(timer,&QTimer::timeout,this,refresh);timer->start();
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[=,this] {
        if(!store())return;
        for(const auto& profile:draft->profiles) if(enabled->isChecked() && profile.enabled && !profile.server.command.isEmpty()
            && QStandardPaths::findExecutable(profile.server.command).isEmpty()) {
            IconDialogs::MessageBox::warning(this,I18n::text("Language servers"),profile.server.name+": "+I18n::text("Choose an existing server executable."));return;
        }
        for(const auto& profile:draft->profiles) if(profile.id=="clangd") for(const auto& argument:profile.server.arguments)
            if(argument.startsWith("--compile-commands-dir=") && !QFileInfo::exists(QDir(argument.mid(23)).filePath("compile_commands.json"))) {
                IconDialogs::MessageBox::warning(this,I18n::text("Language servers"),I18n::text("The selected directory does not contain compile_commands.json."));return;
            }
        QSettings settings;
        serverChanged=original!=draft->json() || settings.value("lsp/enabled",true).toBool()!=enabled->isChecked();
        if(original!=draft->json())draft->save();
        settings.setValue("lsp/enabled",enabled->isChecked());settings.setValue("lsp/semanticHighlighting",semantic->isChecked());
        settings.setValue("lsp/autoCompletion",automatic->isChecked());settings.setValue("lsp/completionDelayMs",delay->value());settings.sync();
        if(settings.status()!=QSettings::NoError){IconDialogs::MessageBox::warning(this,I18n::text("Language servers"),I18n::text("Cannot save settings."));return;}
        accept();
    });
    languages->setCurrentRow(0);
    // End probes while callbacks/widgets are still alive, not in QObject's late
    // child destruction after captured dialog state has gone away.
    connect(this,&QDialog::finished,this,[=,this]{if(state->probe){delete state->probe.data();state->probe=nullptr;}});
}
