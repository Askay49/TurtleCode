#include "MainWindow.hpp"
#include "Codicons.hpp"
#include "I18n.hpp"
#include "editor/EditorWidget.hpp"
#include "language/LspManager.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSet>
#include <QShortcut>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <tuple>

namespace {
constexpr int symbolLimit = 20000;
constexpr int visibleLimit = 300;
enum ResultRole { PathRole = Qt::UserRole, PositionRole, IdentityRole };

struct Symbol {
    QString name, container, path, relativePath;
    QPoint position;
    int kind;
};

void collectSymbols(const QJsonArray& values, const QString& document, const QString& root,
                    const QString& parent, QVector<Symbol>& symbols, int& budget, int depth = 0) {
    if (depth > 64) return;
    for (const auto& value : values) {
        if (budget-- <= 0) return;
        const auto item = value.toObject();
        const auto name = item.value("name").toString();
        const auto location = item.value("location").toObject();
        const auto uri = QUrl(location.value("uri").toString());
        const auto path = location.isEmpty() ? document : QDir::cleanPath(uri.toLocalFile());
        if (!QDir::isAbsolutePath(path) || (!document.isEmpty() && document != path)) continue;
        const auto relative = root.isEmpty() ? QString{} : QDir(root).relativeFilePath(path);
        if (!root.isEmpty() && (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../"))) continue;
        auto range = item.value("selectionRange").toObject();
        if (range.isEmpty()) range = (location.isEmpty() ? item : location).value("range").toObject();
        const auto start = range.value("start").toObject();
        const int line = start.value("line").toInt(-1);
        const int column = start.value("character").toInt(-1);
        const auto largest = std::numeric_limits<int>::max();
        if (!name.isEmpty() && line >= 0 && column >= 0 && line < largest && column < largest) {
            symbols.append({name, item.value("containerName").toString(parent), path, relative,
                            {line + 1, column + 1}, item.value("kind").toInt()});
        }
        const auto container = parent.isEmpty() ? name : parent + "::" + name;
        collectSymbols(item.value("children").toArray(), document, root, container, symbols, budget, depth + 1);
    }
}

int matchRank(const Symbol& symbol, const QString& query) {
    if (query.isEmpty() || symbol.name.compare(query, Qt::CaseInsensitive) == 0) return 0;
    if (symbol.name.startsWith(query, Qt::CaseInsensitive)) return 1;
    if (symbol.name.contains(query, Qt::CaseInsensitive)) return 2;
    if ((symbol.container + "::" + symbol.name).contains(query, Qt::CaseInsensitive)) return 3;
    return 4;
}

class SymbolSearchDialog final : public QDialog {
public:
    SymbolSearchDialog(EditorWidget* editor, LspManager* lsp, const QString& root, bool project, QWidget* parent)
        : QDialog(parent), editor_(editor), lsp_(lsp), root_(root) {
        setObjectName("symbolSearch");
        setWindowTitle(I18n::text("Go to Symbol"));
        resize(720, 420);
        auto* layout = new QVBoxLayout(this);
        scope_ = new QComboBox(this);
        scope_->setObjectName("symbolScope");
        scope_->setAccessibleName(I18n::text("Symbol search scope"));
        scope_->addItems({I18n::text("Current File"), I18n::text("Project")});
        scope_->setCurrentIndex(project ? 1 : 0);
        input_ = new QLineEdit(this);
        input_->setObjectName("symbolQuery");
        input_->setPlaceholderText(I18n::text("Search symbols by name…"));
        results_ = new QListWidget(this);
        results_->setObjectName("symbolResults");
        results_->setUniformItemSizes(true);
        status_ = new QLabel(this);
        status_->setObjectName("symbolStatus");
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(scope_);
        layout->addWidget(input_);
        layout->addWidget(results_, 1);
        layout->addWidget(status_);

        debounce_.setSingleShot(true);
        debounce_.setInterval(120);
        deadline_.setSingleShot(true);
        connect(&debounce_, &QTimer::timeout, this, [this] { refresh(); });
        connect(&deadline_, &QTimer::timeout, this, [this] {
            if (projectMode()) {
                cancelWorkspaceRequests();
                workspaceError_ = true;
                renderWorkspace();
            } else {
                ++generation_;
                fileWaiting_ = false;
                status_->setText(I18n::text("Could not load file symbols."));
            }
        });
        connect(scope_, &QComboBox::currentIndexChanged, this, [this] {
            reset();
            workspaceClients_.clear();
            workspaceStarted_ = false;
            refresh();
            input_->setFocus();
        });
        connect(input_, &QLineEdit::textChanged, this, [this] {
            if (projectMode()) {
                reset();
                if (input_->text().trimmed().isEmpty()) refresh();
                else {
                    status_->setText(I18n::text("Searching…"));
                    debounce_.start();
                }
            } else if (fileLoaded_) renderFile();
        });
        connect(lsp_, &LspManager::statusChanged, this, [this] {
            if (projectMode() && !workspaceRequests_.isEmpty()) sendWorkspaceRequests();
        });
        connect(lsp_, &LspManager::clientStateChanged, this, [this](LspClient* client) {
            if (fileWaiting_ && editor_ && lsp_->serves(client, editor_->filePath()) && !client->starting())
                loadFileSymbols(false);
        });
        if (editor_) connect(editor_, &EditorWidget::contentChanged, this, [this] {
            if (projectMode()) return;
            reset();
            status_->setText(I18n::text("Searching…"));
            debounce_.start();
        });

        connect(input_, &QLineEdit::returnPressed, this, [this] { if (results_->currentItem()) accept(); });
        connect(results_, &QListWidget::itemActivated, this, [this] { accept(); });
        for (const auto key : {Qt::Key_Up, Qt::Key_Down}) {
            auto* shortcut = new QShortcut(QKeySequence(key), input_);
            shortcut->setContext(Qt::WidgetShortcut);
            connect(shortcut, &QShortcut::activated, this, [this, key] {
                if (results_->count())
                    results_->setCurrentRow(std::clamp(results_->currentRow() + (key == Qt::Key_Down ? 1 : -1),
                                                       0, results_->count() - 1));
            });
        }
        input_->setFocus();
        // Let the dialog paint before synchronizing a document or starting servers.
        QTimer::singleShot(0, this, [this] { if (generation_ == 0) refresh(); });
    }

    ~SymbolSearchDialog() override { cancelWorkspaceRequests(); }
    const QListWidgetItem* selected() const { return results_->currentItem(); }
    bool projectMode() const { return scope_->currentIndex() == 1; }

private:
    struct WorkspaceRequest {
        QPointer<LspClient> client;
        int id{};
        bool finished{};
    };

    void cancelWorkspaceRequests() {
        for (const auto& request : workspaceRequests_)
            if (request.client && request.id) request.client->cancelWorkspaceSymbols(request.id);
        workspaceRequests_.clear();
    }

    void reset() {
        ++generation_;
        debounce_.stop();
        deadline_.stop();
        cancelWorkspaceRequests();
        symbols_.clear();
        results_->clear();
        budget_ = symbolLimit;
        fileWaiting_ = fileLoaded_ = workspaceAnswered_ = workspaceError_ = false;
    }

    void refresh() {
        if (projectMode()) startWorkspaceSearch();
        else loadFileSymbols(true);
    }

    void loadFileSymbols(bool mayStart) {
        fileWaiting_ = false;
        deadline_.stop();
        const auto path = editor_ ? editor_->filePath() : QString{};
        if (path.isEmpty()) {
            status_->setText(I18n::text("Open a file to search its symbols."));
            return;
        }
        if (editor_->isBusy()) {
            status_->setText(I18n::text("Document is busy"));
            return;
        }
        if (!editor_->lspEligible()) {
            status_->setText(I18n::text("This document exceeds the language server transfer budget (32 MiB)."));
            return;
        }
        if (!lsp_->supports(path)) {
            status_->setText(I18n::text("Language server not configured"));
            return;
        }
        if (mayStart || lsp_->ready(path)) lsp_->open(path, editor_->lspText());
        if (!lsp_->ready(path)) {
            status_->setText(lsp_->status(path));
            if (mayStart) {
                fileWaiting_ = true;
                deadline_.start(32000);
            }
            return;
        }
        if (!lsp_->documentSymbolsAvailable(path)) {
            status_->setText(I18n::text("This language server does not provide file symbols."));
            return;
        }
        status_->setText(I18n::text("Searching…"));
        deadline_.start(12000);
        lsp_->documentSymbols(path, [guard=QPointer<SymbolSearchDialog>(this), path,
                                    generation=generation_, revision=editor_->lspRevision()](QJsonValue result) {
            if (!guard || guard->generation_ != generation || !guard->editor_ ||
                guard->editor_->filePath() != path || guard->editor_->lspRevision() != revision) return;
            guard->deadline_.stop();
            if (!result.isArray() && !result.isNull()) {
                guard->status_->setText(I18n::text("Could not load file symbols."));
                return;
            }
            collectSymbols(result.toArray(), path, {}, {}, guard->symbols_, guard->budget_);
            guard->fileLoaded_ = true;
            guard->renderFile();
        });
    }

    void startWorkspaceSearch() {
        if (root_.isEmpty()) {
            status_->setText(I18n::text("Open a project folder to search its symbols."));
            return;
        }
        workspaceQuery_ = input_->text().trimmed();
        if (workspaceQuery_.isEmpty()) {
            status_->setText(I18n::text("Type a symbol name to search the project."));
            return;
        }
        if (!workspaceStarted_) {
            workspaceClients_ = lsp_->startWorkspaceSymbolClients(root_);
            workspaceStarted_ = true;
        }
        if (workspaceClients_.isEmpty()) {
            status_->setText(I18n::text("Language server not configured"));
            return;
        }
        for (const auto& client : workspaceClients_) workspaceRequests_.append({client});
        deadline_.start(42000);
        sendWorkspaceRequests();
    }

    void sendWorkspaceRequests() {
        for (qsizetype index = 0; index < workspaceRequests_.size(); ++index) {
            auto& request = workspaceRequests_[index];
            if (request.finished) continue;
            if (!request.client || (!request.client->ready() && !request.client->starting())) {
                workspaceError_ = true;
                request.finished = true;
                continue;
            }
            if (request.id || !request.client->ready()) continue;
            request.id = request.client->workspaceSymbols(workspaceQuery_,
                [guard=QPointer<SymbolSearchDialog>(this), generation=generation_, index](QJsonValue result) {
                    if (!guard || guard->generation_ != generation) return;
                    auto& request = guard->workspaceRequests_[index];
                    request.finished = true;
                    request.id = 0;
                    const bool answered = result.isArray() || result.isNull();
                    guard->workspaceAnswered_ |= answered;
                    guard->workspaceError_ |= !answered;
                    collectSymbols(result.toArray(), {}, guard->root_, {}, guard->symbols_, guard->budget_);
                    guard->renderWorkspace();
                });
            if (!request.id) request.finished = true;
        }
        renderWorkspace();
    }

    void renderWorkspace() {
        render(workspaceQuery_, true);
        if (std::any_of(workspaceRequests_.cbegin(), workspaceRequests_.cend(),
                        [](const auto& request) { return !request.finished; })) {
            status_->setText(I18n::text("Searching…"));
            return;
        }
        deadline_.stop();
        if (workspaceError_)
            status_->setText(I18n::text(workspaceAnswered_ ? "Some language servers could not return symbols."
                                                           : "Could not load project symbols."));
        else if (!workspaceAnswered_)
            status_->setText(I18n::text("The language servers do not provide project symbols."));
    }

    void renderFile() { render(input_->text().trimmed(), false); }

    void render(const QString& query, bool project) {
        struct Match { int rank; const Symbol* symbol; };
        QVector<Match> matches;
        for (const auto& symbol : symbols_) {
            const int rank = matchRank(symbol, query);
            if (rank < 4 || project) matches.append({rank, &symbol});
        }
        std::stable_sort(matches.begin(), matches.end(), [project](const Match& a, const Match& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return project && std::tie(a.symbol->name, a.symbol->relativePath) < std::tie(b.symbol->name, b.symbol->relativePath);
        });
        const auto previous = selected() ? selected()->data(IdentityRole).toString() : QString{};
        results_->clear();
        QSet<QString> seen;
        for (const auto& match : matches) {
            const auto& symbol = *match.symbol;
            const auto identity = symbol.path + QChar(0) + QString::number(symbol.position.x()) + ':'
                + QString::number(symbol.position.y()) + QChar(0) + symbol.name;
            if (seen.contains(identity)) continue;
            seen.insert(identity);
            if (seen.size() > visibleLimit) break;
            QString label = symbol.name;
            if (!symbol.container.isEmpty()) label += "  ·  " + symbol.container;
            if (project) label += "  ·  " + symbol.relativePath;
            auto* item = new QListWidgetItem(Codicons::symbolIcon(symbol.kind), label, results_);
            item->setToolTip(project ? symbol.path : label);
            item->setData(PathRole, symbol.path);
            item->setData(PositionRole, symbol.position);
            item->setData(IdentityRole, identity);
            if (identity == previous) results_->setCurrentItem(item);
        }
        if (!selected() && results_->count()) results_->setCurrentRow(0);
        if (!results_->count()) status_->setText(I18n::text("No symbols found."));
        else if (seen.size() > visibleLimit || budget_ <= 0)
            status_->setText(I18n::text("Showing up to %1 symbols. Refine your search.").arg(visibleLimit));
        else status_->clear();
    }

    QPointer<EditorWidget> editor_;
    LspManager* lsp_;
    QString root_;
    QComboBox* scope_;
    QLineEdit* input_;
    QListWidget* results_;
    QLabel* status_;
    QTimer debounce_, deadline_;
    QVector<QPointer<LspClient>> workspaceClients_;
    QVector<WorkspaceRequest> workspaceRequests_;
    QVector<Symbol> symbols_;
    QString workspaceQuery_;
    quint64 generation_{};
    int budget_{symbolLimit};
    bool fileWaiting_{}, fileLoaded_{}, workspaceStarted_{}, workspaceAnswered_{}, workspaceError_{};
};
} // namespace

void MainWindow::searchSymbols(bool project) {
    if (closing_) return;
    const auto root = projectRoots_.value(0);
    QPointer<EditorWidget> editor = activeEditor();
    QPointer<QWidget> previousFocus = QApplication::focusWidget();
    SymbolSearchDialog dialog(editor, lsp_, root, project, this);
    const auto result = dialog.exec();
    activateWindow();
    if (result != QDialog::Accepted || !dialog.selected()) {
        if (previousFocus && previousFocus->isVisible()) previousFocus->setFocus(Qt::OtherFocusReason);
        else if (editor) editor->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (dialog.projectMode() ? root != projectRoots_.value(0) : !editor || editor != activeEditor()) return;
    const auto* selected = dialog.selected();
    const auto path = selected->data(PathRole).toString();
    if (!dialog.projectMode() && editor->filePath() != path) return;
    const auto position = selected->data(PositionRole).toPoint();
    recordNavigationLocation();
    if (dialog.projectMode()) {
        openFile(path, position.x(), position.y(), 0, true);
    } else {
        editor->recordVimJump();
        editor->revealMatch(position.x(), position.y(), 0);
        editor->setFocus(Qt::OtherFocusReason);
    }
}
