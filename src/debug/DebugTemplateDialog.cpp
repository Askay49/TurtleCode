#include "DebugTemplates.hpp"

#include "DebugConfiguration.hpp"
#include "app/I18n.hpp"
#include "app/IconDialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>

namespace {
QString t(const char* text) { return I18n::text(text); }

enum Template { Python, NativeFile, CMake, OpenOcd, ExternalGdb };

bool cSource(const QString& path) {
    return QFileInfo(path).suffix() == "c";
}

bool nativeSource(const QString& path) {
    const auto suffix = QFileInfo(path).suffix();
    return cSource(path) || suffix == "C" || QStringList{"cc", "cpp", "cxx", "c++"}.contains(suffix.toLower());
}

QJsonObject jsonFile(const QDir& dir, const QString& name) {
    // File API references name files in the reply directory.
    if (name.isEmpty() || QFileInfo(name).fileName() != name) return {};
    QFile file(dir.filePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.read(4 * 1024 * 1024)).object();
}

QMap<QString, QString> cmakeExecutables(const QString& build) {
    QDir reply(QDir(build).filePath(".cmake/api/v1/reply"));
    const auto indices = reply.entryList({"index-*.json"}, QDir::Files, QDir::Name | QDir::Reversed);
    const auto objects = jsonFile(reply, indices.value(0)).value("objects").toArray();
    for (const auto& object : objects) {
        const auto reference = object.toObject();
        if (reference.value("kind") != "codemodel" || reference.value("version").toObject().value("major") != 2)
            continue;
        const auto model = jsonFile(reply, reference.value("jsonFile").toString());
        const auto configurations = model.value("configurations").toArray();
        for (const auto& configuration : configurations) {
            const auto config = configuration.toObject();
            if (config.value("name") != "Debug" && configurations.size() != 1) continue;
            QMap<QString, QString> result;
            for (const auto& target : config.value("targets").toArray()) {
                const auto details = jsonFile(reply, target.toObject().value("jsonFile").toString());
                if (details.value("type") != "EXECUTABLE") continue;
                const auto artifacts = details.value("artifacts").toArray();
                if (artifacts.isEmpty()) continue;
                const auto program = artifacts.first().toObject().value("path").toString();
                if (!program.isEmpty())
                    result[details.value("name").toString()] = QDir(build).absoluteFilePath(program);
            }
            return result;
        }
    }
    return {};
}

class TemplateDialog final : public QDialog {
public:
    TemplateDialog(QWidget* parent, const QString& root, const QString& currentFile,
                   const std::function<QString(const DebugTemplates::Draft&)>& save)
        : QDialog(parent), root_(root), currentFile_(currentFile) {
        setObjectName("debugTemplateDialog");
        setWindowTitle(t("Add Debug Configuration"));
        resize(850, 315);
        auto* layout = new QVBoxLayout(this);
        auto* content = new QHBoxLayout;
        layout->addLayout(content, 1);
        templates_ = new QTreeWidget(this);
        templates_->setObjectName("debugTemplates");
        templates_->setHeaderHidden(true);
        templates_->setRootIsDecorated(false);
        templates_->setItemsExpandable(false);
        templates_->setIndentation(12);
        templates_->setMinimumWidth(278);
        content->addWidget(templates_);
        pages_ = new QStackedWidget(this);
        content->addWidget(pages_, 1);

        const auto addGroup = [this](const char* label) {
            auto* group = new QTreeWidgetItem(templates_, {t(label)});
            group->setFlags(Qt::ItemIsEnabled);
            auto font = group->font(0);
            font.setBold(true);
            group->setFont(0, font);
            return group;
        };
        const auto addTemplate = [](QTreeWidgetItem* group, const char* label, Template kind) {
            auto* item = new QTreeWidgetItem(group, {t(label)});
            item->setData(0, Qt::UserRole, kind);
            item->setToolTip(0, t(label));
            return item;
        };
        auto* python = addTemplate(addGroup("Python"), "Run Python", Python);
        auto* native = addGroup("C/C++");
        auto* single = addTemplate(native, "Build and Run a C/C++ File", NativeFile);
        auto* cmake = addTemplate(native, "Run a CMake / Qt Target", CMake);
        auto* firmware = addGroup("STM32");
        addTemplate(firmware, "Launch via ST-Link and OpenOCD", OpenOcd);
        addTemplate(firmware, "Attach to an External GDB Server", ExternalGdb);
        templates_->expandAll();

        QString ignored;
        DebugJson::read(DebugProject::path(root, "settings.json"), settings_, ignored, true);
        pythonPage();
        nativePage();
        cmakePage();
        firmwarePage(false);
        firmwarePage(true);

        error_ = note();
        error_->setObjectName("debugTemplateError");
        layout->addWidget(error_);
        auto* buttons = new IconDialogs::ButtonBox(QDialogButtonBox::Cancel, this);
        auto* add = buttons->addButton(t("Add"), QDialogButtonBox::AcceptRole);
        add->setObjectName("debugTemplateAdd");
        add->setIcon(Codicons::icon("add"));
        add->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this, save] {
            DebugTemplates::Draft draft;
            auto error = makeDraft(draft);
            if (error.isEmpty()) error = save(draft);
            if (error.isEmpty()) accept();
            else setNote(error_, error);
        });
        connect(templates_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
            if (!item || !item->parent()) return;
            pages_->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
            setNote(error_, {});
            updateTools();
        });
        const bool hasCMake = QFileInfo(root_.filePath("CMakeLists.txt")).isFile();
        const auto suffix = QFileInfo(currentFile_).suffix().toLower();
        auto* preferred = suffix == "py" ? python : hasCMake ? cmake : currentFile_.isEmpty() ? python : single;
        templates_->setCurrentItem(preferred);
        templates_->setFocus();
    }

private:
    QFormLayout* form() {
        auto* page = new QWidget(pages_);
        auto* layout = new QFormLayout(page);
        layout->setContentsMargins(8, 4, 0, 0);
        layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        pages_->addWidget(page);
        return layout;
    }

    QLabel* note() {
        auto* label = new QLabel(this);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->hide();
        return label;
    }

    static void setNote(QLabel* label, const QString& text) {
        label->setText(text);
        label->setVisible(!text.isEmpty());
    }

    QLineEdit* pathField(QFormLayout* layout, const char* label, const char* id,
                         const QString& value = {}, const QString& filter = {}, bool directory = false) {
        auto* row = new QWidget(this);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(value, row);
        edit->setObjectName(QString::fromLatin1(id));
        line->addWidget(edit, 1);
        auto* browse = new QPushButton(t("Browse…"), row);
        browse->setIcon(Codicons::icon("folder-opened"));
        line->addWidget(browse);
        auto* fieldLabel = new QLabel(t(label), row);
        fieldLabel->setBuddy(edit);
        layout->addRow(fieldLabel, row);
        connect(browse, &QPushButton::clicked, this, [=, this] {
            const auto path = root_.absoluteFilePath(edit->text().trimmed());
            const auto start = QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath();
            const auto picked = directory ? IconDialogs::FileDialog::getExistingDirectory(this, t(label), start)
                                          : IconDialogs::FileDialog::getOpenFileName(this, t(label), start, filter);
            if (!picked.isEmpty()) edit->setText(DebugConfiguration::portable(picked, root_.absolutePath()));
        });
        return edit;
    }

    void pythonPage() {
        auto* layout = form();
        pythonCurrent_ = new QCheckBox(t("Use the current Python file at launch"), this);
        pythonCurrent_->setObjectName("debugTemplatePythonCurrent");
        pythonCurrent_->setChecked(QFileInfo(currentFile_).suffix().compare("py", Qt::CaseInsensitive) == 0);
        layout->addRow(pythonCurrent_);
        const auto script = pythonCurrent_->isChecked() ? DebugConfiguration::portable(currentFile_, root_.absolutePath()) : QString{};
        pythonFile_ = pathField(layout, "Python script", "debugTemplatePythonFile", script, t("Python files (*.py);;All files (*)"));
        if (script.isEmpty() && QFileInfo(root_.filePath("main.py")).isFile()) pythonFile_->setText("main.py");
        auto* scriptField = pythonFile_->parentWidget();
        auto* scriptLabel = layout->labelForField(scriptField);
        const auto setScriptEnabled = [scriptField, scriptLabel](bool current) {
            scriptField->setEnabled(!current);
            scriptLabel->setEnabled(!current);
        };
        connect(pythonCurrent_, &QCheckBox::toggled, this, setScriptEnabled);
        setScriptEnabled(pythonCurrent_->isChecked());

        auto interpreter = settings_.value("python.defaultInterpreterPath").toString("python3");
        if (QFileInfo(root_.filePath(".venv/bin/python")).isExecutable()) interpreter = ".venv/bin/python";
        python_ = pathField(layout, "Python interpreter", "debugTemplatePython", interpreter);
        auto* check = new QPushButton(t("Check debugpy"), this);
        check->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto* probe = new QProcess(this);
        auto* timeout = new QTimer(this);
        timeout->setSingleShot(true);
        layout->addRow(check);
        pythonNote_ = note();
        layout->addRow(pythonNote_);
        connect(python_, &QLineEdit::textChanged, this, [this] {
            pythonResult_.clear();
            updateTools();
        });
        connect(check, &QPushButton::clicked, this, [=, this] {
            const auto interpreterPath = executable(python_->text().trimmed());
            if (interpreterPath.isEmpty()) { updateTools(); return; }
            check->setEnabled(false);
            probedPython_ = interpreterPath;
            pythonResult_ = t("Checking for debugpy…");
            updateTools();
            probe->setWorkingDirectory(QDir::tempPath());
            probe->start(interpreterPath, {"-c", "import importlib.util; import sys; sys.exit(0 if importlib.util.find_spec('debugpy') else 42)"});
            timeout->start(4000);
        });
        connect(timeout, &QTimer::timeout, probe, &QProcess::kill);
        const auto checked = [=, this](const QString& result) {
            timeout->stop();
            check->setEnabled(true);
            pythonResult_ = result;
            updateTools();
        };
        connect(probe, &QProcess::errorOccurred, this, [checked](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) checked(t("Could not check debugpy."));
        });
        connect(probe, &QProcess::finished, this, [checked](int code, QProcess::ExitStatus status) {
            if (status == QProcess::NormalExit && code == 0) checked(t("debugpy is available."));
            else if (status == QProcess::NormalExit && code == 42) checked(t("debugpy is missing from the selected interpreter."));
            else checked(t("Could not check debugpy."));
        });
    }

    void nativePage() {
        auto* layout = form();
        const auto source = nativeSource(currentFile_) ? DebugConfiguration::portable(currentFile_, root_.absolutePath()) : QString{};
        nativeFile_ = pathField(layout, "Source file", "debugTemplateSource", source,
                                t("C/C++ files (*.c *.C *.cc *.cpp *.cxx *.c++);;All files (*)"));
        compiler_ = pathField(layout, "Compiler", "debugTemplateCompiler", cSource(source) ? "cc" : "c++");
        nativeNote_ = note();
        layout->addRow(nativeNote_);
        connect(nativeFile_, &QLineEdit::textChanged, this, [this](const QString& path) {
            if (compiler_->text() == "cc" || compiler_->text() == "c++")
                compiler_->setText(cSource(path) ? "cc" : "c++");
        });
        connect(compiler_, &QLineEdit::textChanged, this, [this] { updateTools(); });
    }

    void cmakePage() {
        auto* layout = form();
        auto directory = QString("build/turtle-debug");
        for (const auto& candidate : {"build/debug", "build"}) {
            QFile cache(root_.filePath(QString(candidate) + "/CMakeCache.txt"));
            if (cache.open(QIODevice::ReadOnly) && cache.read(1024 * 1024).contains("CMAKE_BUILD_TYPE:STRING=Debug")) {
                directory = candidate;
                break;
            }
        }
        build_ = pathField(layout, "Build directory", "debugTemplateBuild", directory, {}, true);
        target_ = new QComboBox(this);
        target_->setObjectName("debugTemplateTarget");
        target_->setEditable(true);
        target_->setInsertPolicy(QComboBox::NoInsert);
        layout->addRow(t("CMake target"), target_);
        binary_ = pathField(layout, "Executable path", "debugTemplateExecutable");
        auto* hint = note();
        layout->addRow(hint);
        setNote(hint, t("If the target is not listed, enter its name and the executable path it will produce."));
        cmakeNote_ = note();
        layout->addRow(cmakeNote_);
        const auto updateBinary = [this](const QString& name) {
            const auto artifact = target_->itemData(target_->findText(name)).toString();
            const auto suggestion = DebugConfiguration::portable(artifact, root_.absolutePath());
            if (binary_->text().isEmpty() || binary_->text() == suggestedBinary_) binary_->setText(suggestion);
            suggestedBinary_ = suggestion;
            binary_->setPlaceholderText(name.isEmpty() ? QString{} : QDir(build_->text()).filePath(name));
        };
        connect(target_, &QComboBox::editTextChanged, this, updateBinary);
        connect(target_, &QComboBox::currentIndexChanged, this, [this, updateBinary] { updateBinary(target_->currentText()); });
        const auto refresh = [this, updateBinary] {
            const auto previous = target_->currentText();
            const auto known = cmakeExecutables(root_.absoluteFilePath(build_->text().trimmed()));
            const QSignalBlocker blocker(target_);
            target_->clear();
            for (auto it = known.cbegin(); it != known.cend(); ++it) target_->addItem(it.key(), it.value());
            if (!previous.isEmpty()) target_->setCurrentIndex(target_->findText(previous));
            if (target_->currentIndex() < 0) target_->setEditText(previous);
            updateBinary(target_->currentText());
        };
        auto* refreshTimer = new QTimer(this);
        refreshTimer->setSingleShot(true);
        refreshTimer->setInterval(200);
        connect(build_, &QLineEdit::textChanged, this, [this, refreshTimer] {
            if (binary_->text() == suggestedBinary_) binary_->clear();
            refreshTimer->start();
        });
        connect(refreshTimer, &QTimer::timeout, this, refresh);
        refresh();
    }

    void firmwarePage(bool external) {
        auto* layout = form();
        auto* elf = pathField(layout, "ELF file with symbols", external ? "debugTemplateExternalElf" : "debugTemplateOpenOcdElf",
                              {}, t("ELF files (*.elf *.axf);;All files (*)"));
        if (external) {
            externalElf_ = elf;
            host_ = new QLineEdit("localhost", this);
            host_->setObjectName("debugTemplateHost");
            layout->addRow(t("GDB server host"), host_);
            port_ = new QSpinBox(this);
            port_->setObjectName("debugTemplatePort");
            port_->setRange(1, 65535);
            port_->setValue(3333);
            layout->addRow(t("GDB server port"), port_);
            externalNote_ = note();
            layout->addRow(externalNote_);
        } else {
            openOcdElf_ = elf;
            board_ = pathField(layout, "Board or target configuration", "debugTemplateBoard", {},
                               t("OpenOCD configuration (*.cfg);;All files (*)"));
            board_->setPlaceholderText("target/stm32f4x.cfg");
            stLink_ = new QCheckBox(t("Include the ST-Link interface script"), this);
            stLink_->setChecked(true);
            layout->addRow(stLink_);
            connect(board_, &QLineEdit::textChanged, this, [this](const QString& path) {
                stLink_->setChecked(!QDir::fromNativeSeparators(path).split('/').contains("board"));
            });
            buildTask_ = new QComboBox(this);
            buildTask_->setObjectName("debugTemplateBuildTask");
            buildTask_->addItem(t("No build task"), QString{});
            QJsonObject tasks;
            QString ignored;
            if (DebugJson::read(DebugProject::path(root_.absolutePath(), "tasks.json"), tasks, ignored, true))
                for (const auto& label : DebugTemplates::taskLabels(tasks.value("tasks").toArray()))
                    if (!label.isEmpty()) buildTask_->addItem(label, label);
            layout->addRow(t("Build before launch"), buildTask_);
            openOcdNote_ = note();
            layout->addRow(openOcdNote_);
        }
    }

    QString executable(QString command) const {
        if (command.isEmpty()) return {};
        command.replace("${workspaceFolder}", root_.absolutePath());
        command.replace("${workspaceRoot}", root_.absolutePath());
        command.replace("${userHome}", QDir::homePath());
        if (!command.contains('/')) return QStandardPaths::findExecutable(command);
        const auto path = root_.absoluteFilePath(command);
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable() ? path : QString{};
    }

    void updateTools() {
        const auto missing = [this](const QStringList& commands) {
            QStringList result;
            for (const auto& command : commands)
                if (executable(command).isEmpty()) result.append(command);
            return result.isEmpty() ? QString{} : t("Tools not found: %1").arg(result.join(", "));
        };
        switch (pages_->currentIndex()) {
        case Python: {
            const auto path = executable(python_->text().trimmed());
            setNote(pythonNote_, path.isEmpty() ? t("Python interpreter not found.") :
                                 path == probedPython_ ? pythonResult_ : QString{});
            break;
        }
        case NativeFile:
            setNote(nativeNote_, missing({compiler_->text().trimmed().isEmpty() ? "c++" : compiler_->text().trimmed(), "lldb-dap"}));
            break;
        case CMake:
            setNote(cmakeNote_, missing({"cmake", "lldb-dap"}));
            break;
        case OpenOcd:
        case ExternalGdb: {
            const bool openOcd = pages_->currentIndex() == OpenOcd;
            const auto configured = [this](const QString& setting, const QString& fallback) {
                return settings_.value(setting + ".linux").toString(settings_.value(setting).toString(fallback));
            };
            QStringList tools{"node"};
            if (openOcd) tools.append(configured("cortex-debug.openocdPath", "openocd"));
            auto message = missing(tools);
            if (DebugProject::extension("marus25.cortex-debug-", "dist/debugadapter.js").isEmpty()) {
                if (!message.isEmpty()) message += '\n';
                message += t("Cortex-Debug is not installed.");
            }
            setNote(openOcd ? openOcdNote_ : externalNote_, message);
            break;
        }
        default: break;
        }
    }

    QString workspacePath(const QString& path) const {
        const auto portable = DebugConfiguration::portable(QDir::cleanPath(root_.absoluteFilePath(path)), root_.absolutePath());
        if (portable == ".") return "${workspaceFolder}";
        return QDir::isAbsolutePath(portable) ? portable : "${workspaceFolder}/" + portable;
    }

    bool fileExists(const QString& path) const {
        return !path.isEmpty() && QFileInfo(root_.absoluteFilePath(path)).isFile();
    }

    static QJsonObject task(const QString& label, const QString& command, const QJsonArray& args) {
        return {{"label", label}, {"type", "process"}, {"command", command}, {"args", args},
                {"options", QJsonObject{{"cwd", "${workspaceFolder}"}}}};
    }

    QString makeDraft(DebugTemplates::Draft& draft) const {
        auto& launch = draft.launch;
        launch = {{"request", "launch"}, {"cwd", "${workspaceFolder}"}};
        switch (pages_->currentIndex()) {
        case Python: {
            const auto path = pythonFile_->text().trimmed();
            if (!pythonCurrent_->isChecked() && !fileExists(path)) return t("Choose an existing Python script.");
            const auto interpreter = python_->text().trimmed();
            if (interpreter.isEmpty()) return t("Enter a Python interpreter.");
            launch["name"] = pythonCurrent_->isChecked() ? t("Python: current file") : t("Python: %1").arg(QFileInfo(path).fileName());
            launch["type"] = "debugpy";
            launch["program"] = pythonCurrent_->isChecked() ? "${file}" : workspacePath(path);
            launch["python"] = interpreter.contains('/') && !interpreter.contains("${") ? workspacePath(interpreter) : interpreter;
            return {};
        }
        case NativeFile: {
            const auto source = nativeFile_->text().trimmed();
            if (!fileExists(source) || !nativeSource(source)) return t("Choose an existing C/C++ source file.");
            const auto compiler = compiler_->text().trimmed();
            if (compiler.isEmpty()) return t("Enter a C/C++ compiler.");
            const auto file = QFileInfo(source);
            const auto portable = DebugConfiguration::portable(root_.absoluteFilePath(source), root_.absolutePath());
            const auto hash = QCryptographicHash::hash(portable.toUtf8(), QCryptographicHash::Sha256).toHex().left(12);
            const auto program = "${workspaceFolder}/.turtle/" + file.completeBaseName() + "-" + QString::fromLatin1(hash) + ".debug";
            launch["name"] = t("C/C++: %1").arg(file.fileName());
            launch["type"] = "lldb";
            launch["program"] = program;
            launch["disableASLR"] = false;
            launch["runInTerminal"] = true;
            draft.tasks.append(task(t("Build %1"), compiler.contains('/') ? workspacePath(compiler) : compiler,
                                    {"-g", "-O0", workspacePath(source), "-o", program}));
            return {};
        }
        case CMake: {
            if (!fileExists("CMakeLists.txt")) return t("Open a project with CMakeLists.txt at its root.");
            const auto build = build_->text().trimmed();
            const auto target = target_->currentText().trimmed();
            const auto program = binary_->text().trimmed();
            if (build.isEmpty() || target.isEmpty() || program.isEmpty()) return t("Enter a build directory, target and executable path.");
            const auto directory = workspacePath(build);
            launch["name"] = t("CMake: %1").arg(target);
            launch["type"] = "lldb";
            launch["program"] = workspacePath(program);
            launch["disableASLR"] = false;
            launch["runInTerminal"] = true;
            draft.tasks.append(task(t("Configure %1"), "cmake", {"-S", "${workspaceFolder}", "-B", directory, "-DCMAKE_BUILD_TYPE=Debug"}));
            draft.tasks.append(task(t("Build %1"), "cmake", {"--build", directory, "--config", "Debug", "--target", target}));
            return {};
        }
        case OpenOcd:
        case ExternalGdb: {
            const bool openOcd = pages_->currentIndex() == OpenOcd;
            const auto elf = (openOcd ? openOcdElf_ : externalElf_)->text().trimmed();
            if (elf.isEmpty()) return t("Enter the ELF file path.");
            if (!fileExists(elf)) {
                if (!openOcd) return t("Choose an existing ELF file.");
                if (buildTask_->currentData().toString().isEmpty()) return t("Choose an existing ELF file or select a build task.");
            }
            launch["type"] = "cortex-debug";
            launch["executable"] = workspacePath(elf);
            launch["name"] = openOcd ? t("STM32: ST-Link / OpenOCD") : t("STM32: external GDB server");
            if (openOcd) {
                const auto board = board_->text().trimmed();
                if (board.isEmpty()) return t("Enter the OpenOCD board or target configuration.");
                const auto config = fileExists(board) ? workspacePath(board) : board;
                QJsonArray configs;
                if (stLink_->isChecked()) configs.append("interface/stlink.cfg");
                configs.append(config);
                launch["servertype"] = "openocd";
                launch["configFiles"] = configs;
                const auto prerequisite = buildTask_->currentData().toString();
                if (!prerequisite.isEmpty()) launch["preLaunchTask"] = prerequisite;
            } else {
                auto host = host_->text().trimmed();
                if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
                if (host.contains(':')) {
                    if (QHostAddress(host).protocol() != QAbstractSocket::IPv6Protocol)
                        return t("Enter a GDB server hostname or IP address.");
                    host = '[' + host + ']';
                } else if (host.isEmpty() || host.contains(QRegularExpression("[^\\p{L}\\p{N}_.-]")))
                    return t("Enter a GDB server hostname or IP address.");
                launch["request"] = "attach";
                launch["servertype"] = "external";
                launch["gdbTarget"] = host + ':' + QString::number(port_->value());
            }
            return {};
        }
        default: return t("Select a debug template.");
        }
    }

    QDir root_;
    QString currentFile_;
    QJsonObject settings_;
    QTreeWidget* templates_;
    QStackedWidget* pages_;
    QLabel* error_;
    QLineEdit *pythonFile_, *python_, *nativeFile_, *compiler_, *build_, *binary_;
    QLineEdit *openOcdElf_, *externalElf_, *board_, *host_;
    QCheckBox *pythonCurrent_, *stLink_;
    QComboBox *target_, *buildTask_;
    QSpinBox* port_;
    QLabel *pythonNote_, *nativeNote_, *cmakeNote_, *openOcdNote_, *externalNote_;
    QString probedPython_, pythonResult_, suggestedBinary_;
};
}

void DebugTemplates::choose(QWidget* parent, const QString& root, const QString& currentFile,
                            const std::function<QString(const Draft&)>& save) {
    TemplateDialog dialog(parent, root, currentFile, save);
    dialog.exec();
}
