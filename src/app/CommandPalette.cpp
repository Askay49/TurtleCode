#include "CommandPalette.hpp"
#include "I18n.hpp"
#include <QLineEdit>
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QSettings>
#include <QScreen>
#include <algorithm>

int CommandPalette::matchScore(const QString& text, const QString& query) {
    const auto haystack = text.toCaseFolded().replace(QChar(0x451), QChar(0x435));
    const auto words = query.simplified().toCaseFolded().replace(QChar(0x451), QChar(0x435)).split(' ', Qt::SkipEmptyParts);
    int score = 0;
    for (const auto& word : words) {
        const int exact = haystack.indexOf(word);
        if (exact >= 0) { score += 200 + word.size() * 8 - std::min(exact, 80); continue; }
        int position = 0, previous = -2;
        for (const auto ch : word) {
            const int found = haystack.indexOf(ch, position);
            if (found < 0) return -1;
            score += 5 + (found == previous + 1 ? 12 : 0);
            if (found == 0 || !haystack[found - 1].isLetterOrNumber()) score += 10;
            previous = found; position = found + 1;
        }
    }
    return score;
}

CommandPalette::CommandPalette(QVector<PaletteCommand> commands, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint), commands_(std::move(commands)) {
    setObjectName("commandPalette");
    setWindowTitle(I18n::text("Command palette"));
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    input_ = new QLineEdit(this);
    input_->setObjectName("commandPaletteInput");
    input_->setPlaceholderText(I18n::text("Search commands…"));
    input_->setClearButtonEnabled(true);
    input_->installEventFilter(this);
    results_ = new QTreeWidget(this);
    results_->setObjectName("commandPaletteResults");
    results_->setColumnCount(2);
    results_->setHeaderHidden(true);
    results_->setRootIsDecorated(false);
    results_->setUniformRowHeights(true);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->header()->setStretchLastSection(false);
    results_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    results_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    results_->installEventFilter(this);
    status_ = new QLabel(this);
    layout->addWidget(input_);
    layout->addWidget(results_, 1);
    layout->addWidget(status_);
    recent_ = QSettings().value("commands/recent").toStringList().mid(0, 20);
    connect(input_, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { acceptCurrent(); });
    connect(results_, &QTreeWidget::itemClicked, this, [this] { acceptCurrent(); });
    connect(results_, &QTreeWidget::currentItemChanged, this, [this] {
        auto* item = results_->currentItem();
        const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
        status_->setText(index < 0 ? I18n::text("No matching commands") :
            !commands_[index].enabled ? commands_[index].reason : I18n::text("Enter to run · Escape to close"));
    });
    const auto bounds = parent->screen()->availableGeometry();
    resize(std::max(1, std::min({720, parent->width() - 24, bounds.width() - 24})),
           std::max(1, std::min({420, parent->height() - 24, bounds.height() - 24})));
    const QPoint origin = parent->mapToGlobal(QPoint((parent->width() - width()) / 2, parent->height() / 6));
    move(std::clamp(origin.x(), bounds.left(), std::max(bounds.left(), bounds.right() - width() + 1)),
         std::clamp(origin.y(), bounds.top(), std::max(bounds.top(), bounds.bottom() - height() + 1)));
    refresh();
    input_->setFocus();
}
void CommandPalette::refresh() {
    const QString previous = results_->currentItem() ? results_->currentItem()->data(0, Qt::UserRole + 1).toString() : QString{};
    results_->clear();
    struct Match { int index, score; };
    QVector<Match> matches;
    for (int i = 0; i < commands_.size(); ++i) {
        const auto& command = commands_[i];
        int score = matchScore(command.title + " " + command.searchText, input_->text());
        if (score < 0) continue;
        if (input_->text().trimmed().isEmpty()) {
            const int recent = recent_.indexOf(command.id);
            score = recent < 0 ? 0 : 100 - recent;
        }
        matches.push_back({i, score});
    }
    std::stable_sort(matches.begin(), matches.end(), [this](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        return QString::localeAwareCompare(commands_[a.index].title, commands_[b.index].title) < 0;
    });
    QTreeWidgetItem* selection = nullptr;
    for (const auto match : matches) {
        const auto& command = commands_[match.index];
        auto* item = new QTreeWidgetItem(results_);
        const QString state = command.checked < 0 ? QString{} : I18n::text(command.checked ? "On" : "Off") + (command.shortcut.isEmpty() ? QString{} : QStringLiteral(" · "));
        item->setText(0, command.title);
        item->setIcon(0, command.icon);
        item->setText(1, state + command.shortcut);
        item->setData(0, Qt::UserRole, match.index);
        item->setData(0, Qt::UserRole + 1, command.id);
        item->setToolTip(0, command.reason);
        if (!command.enabled) for (int col = 0; col < 2; ++col)
            item->setForeground(col, palette().brush(QPalette::Disabled, QPalette::Text));
        if (command.id == previous) selection = item;
    }
    // A changed query always selects its best match.
    if (!input_->text().isEmpty()) selection = nullptr;
    results_->setCurrentItem(selection ? selection : results_->topLevelItem(0));
    if (matches.isEmpty()) status_->setText(I18n::text("No matching commands"));
}
void CommandPalette::acceptCurrent() {
    const auto* item = results_->currentItem();
    if (!item) return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (!commands_[index].enabled) return;
    selected_ = index;
    accept();
}
void CommandPalette::executeSelected() {
    if (selected_ < 0 || !commands_[selected_].enabled) return;
    const auto& command = commands_[selected_];
    recent_.removeAll(command.id);
    recent_.prepend(command.id);
    QSettings().setValue("commands/recent", recent_.mid(0, 20));
    selected_ = -1;
    if (command.execute) command.execute();
}
bool CommandPalette::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) { reject(); return true; }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) { acceptCurrent(); return true; }
        if (object == input_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            const int count = results_->topLevelItemCount();
            if (count) {
                const int row = results_->indexOfTopLevelItem(results_->currentItem());
                results_->setCurrentItem(results_->topLevelItem(std::clamp(row + (key->key() == Qt::Key_Down ? 1 : -1), 0, count - 1)));
                results_->scrollToItem(results_->currentItem());
            }
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}
