#include "app/IconDialogs.hpp"
#include "app/FileIcons.hpp"
#include "app/Codicons.hpp"
#include "SearchPanel.hpp"
#include "app/FontSettings.hpp"

#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStyle>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {
QToolButton* optionButton(const QString& text, const QString& tooltip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setIcon(Codicons::icon(text));
    button->setAccessibleName(tooltip);
    button->setToolTip(tooltip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFixedWidth(28);
    return button;
}

class SearchResultDelegate final : public QStyledItemDelegate {
public:
    SearchResultDelegate(std::function<QString(qsizetype)> suffix, QObject* parent)
        : QStyledItemDelegate(parent), suffix_(std::move(suffix)) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem view(option);
        initStyleOption(&view, index);
        if (index.parent().isValid()) {
            const auto matchIndex = index.data(Qt::UserRole + 4).toLongLong();
            if (matchIndex >= 0) view.text += suffix_(matchIndex);
        }
        const QWidget* widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(
            QStyle::CE_ItemViewItem, &view, painter, widget);
    }

private:
    std::function<QString(qsizetype)> suffix_;
};
}

void SearchPanel::applyFontSettings() {
    const auto size = FontSettings::font(FontSettings::Area::Tree).pointSizeF();
    auto widgets = findChildren<QWidget*>();
    widgets.prepend(this);
    for(auto* child : widgets) {
        auto childFont = child->font();
        childFont.setPointSizeF(size);
        child->setFont(childFont);
    }
}

SearchPanel::SearchPanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(240);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 6);
    layout->setSpacing(6);

    auto* titleRow = new QHBoxLayout;
    auto* title = new QLabel(tr("SEARCH"), this);
    QFont titleFont;
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto* detailsToggle = new QToolButton(this);
    detailsToggle->setIcon(Codicons::icon("ellipsis"));
    detailsToggle->setToolTip(tr("Toggle Search Details"));
    detailsToggle->setCheckable(true);
    detailsToggle->setAutoRaise(true);
    titleRow->addWidget(title);
    titleRow->addStretch();
    titleRow->addWidget(detailsToggle);
    layout->addLayout(titleRow);

    auto* searchRow = new QHBoxLayout;
    searchRow->setSpacing(2);
    replaceToggle_ = new QToolButton(this);
    replaceToggle_->setObjectName(QStringLiteral("projectReplaceToggle"));
    replaceToggle_->setIcon(Codicons::icon("chevron-right"));
    replaceToggle_->setToolTip(tr("Toggle Replace"));
    replaceToggle_->setCheckable(true);
    replaceToggle_->setAutoRaise(true);
    query_ = new QLineEdit(this);
    query_->setObjectName(QStringLiteral("projectSearchInput"));
    query_->setPlaceholderText(tr("Search"));
    query_->setClearButtonEnabled(true);
    matchCase_ = optionButton(QStringLiteral("case-sensitive"), tr("Match Case"), this);
    wholeWord_ = optionButton(QStringLiteral("whole-word"), tr("Match Whole Word"), this);
    regex_ = optionButton(QStringLiteral("regex"), tr("Use Regular Expression"), this);
    searchRow->addWidget(replaceToggle_);
    searchRow->addWidget(query_, 1);
    searchRow->addWidget(matchCase_);
    searchRow->addWidget(wholeWord_);
    searchRow->addWidget(regex_);
    layout->addLayout(searchRow);

    replaceRow_ = new QWidget(this);
    auto* replaceLayout = new QHBoxLayout(replaceRow_);
    replaceLayout->setContentsMargins(25, 0, 0, 0);
    replacement_ = new QLineEdit(replaceRow_);
    replacement_->setObjectName(QStringLiteral("projectReplaceInput"));
    replacement_->setPlaceholderText(tr("Replace"));
    replaceButton_ = new QToolButton(replaceRow_);
    replaceButton_->setObjectName(QStringLiteral("projectReplaceButton"));
    replaceButton_->setText(tr("Replace Selected"));
    replaceButton_->setToolTip(tr("Replace checked matches in files"));
    replaceButton_->setEnabled(false);
    replaceLayout->addWidget(replacement_, 1);
    replaceLayout->addWidget(replaceButton_);
    replaceRow_->hide();
    layout->addWidget(replaceRow_);

    details_ = new QWidget(this);
    auto* detailsLayout = new QVBoxLayout(details_);
    detailsLayout->setContentsMargins(25, 0, 0, 0);
    detailsLayout->setSpacing(3);
    include_ = new QLineEdit(details_);
    include_->setPlaceholderText(tr("files to include (for example src/**)"));
    exclude_ = new QLineEdit(details_);
    exclude_->setPlaceholderText(tr("files to exclude (for example build/**)"));
    detailsLayout->addWidget(include_);
    detailsLayout->addWidget(exclude_);
    details_->hide();
    layout->addWidget(details_);

    results_ = new QTreeWidget(this);
    results_->setObjectName(QStringLiteral("projectSearchResults"));
    results_->setHeaderHidden(true);
    results_->setIndentation(14);
    results_->setUniformRowHeights(true);
    results_->setRootIsDecorated(true);
    results_->setMouseTracking(true);
    results_->setItemDelegate(new SearchResultDelegate([this](qsizetype index) {
        if (!replaceToggle_->isChecked() || index >= currentResult_.matches.size()) return QString();
        QString replacement = ProjectReplaceService::previewReplacement(
            currentResult_.matches[index], replacement_->text(), searchedOptions_.regularExpression);
        replacement.replace('\n', QStringLiteral("↵"));
        return QStringLiteral("  →  %1").arg(replacement.isEmpty() ? QStringLiteral("∅")
                                                            : replacement.left(100));
    }, results_));
    layout->addWidget(results_, 1);

    status_ = new QLabel(tr("Type to search across the workspace."), this);
    status_->setObjectName(QStringLiteral("projectSearchStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(250);
    connect(debounce_, &QTimer::timeout, this, &SearchPanel::startSearch);
    connect(query_, &QLineEdit::textChanged, this, &SearchPanel::scheduleSearch);
    connect(query_, &QLineEdit::returnPressed, this, &SearchPanel::startSearch);
    for (auto* button : {matchCase_, wholeWord_, regex_})
        connect(button, &QToolButton::toggled, this, &SearchPanel::scheduleSearch);
    connect(include_, &QLineEdit::textChanged, this, &SearchPanel::scheduleSearch);
    connect(exclude_, &QLineEdit::textChanged, this, &SearchPanel::scheduleSearch);
    connect(replaceToggle_, &QToolButton::toggled, this, [this](bool visible) {
        replaceToggle_->setIcon(Codicons::icon(visible ? "chevron-down" : "chevron-right"));
        replaceRow_->setVisible(visible);
        updateReplacePreview();
        updateReplaceButton();
    });
    connect(replacement_, &QLineEdit::textChanged, this, &SearchPanel::updateReplacePreview);
    connect(replaceButton_, &QToolButton::clicked, this, [this] {
        if (replacing_) replaceService_.cancel();
        else requestReplace();
    });
    connect(detailsToggle, &QToolButton::toggled, details_, &QWidget::setVisible);
    connect(&service_, &ProjectSearchService::searchStarted, this, [this] {
        status_->setText(tr("Searching…"));
    });
    connect(&service_, &ProjectSearchService::searchFinished,
            this, &SearchPanel::displayResult);
    connect(&replaceService_, &ProjectReplaceService::replaceFinished, this,
            [this](const ProjectReplaceResult& result) {
        replacing_ = false;
        replaceButton_->setText(tr("Replace Selected"));
        for (QWidget* field : std::initializer_list<QWidget*>{query_, replacement_, include_, exclude_,
                               matchCase_, wholeWord_, regex_, results_}) field->setEnabled(true);
        replaceSummary_ = tr("Replaced %1 matches in %2 files.")
            .arg(result.matchesReplaced).arg(result.filesChanged);
        if (result.cancelled) replaceSummary_ += tr(" Cancelled.");
        const int skipped = result.skippedProtected.size() + result.skippedChanged.size();
        if (skipped || !result.errors.isEmpty()) {
            replaceSummary_ += tr(" Skipped %1 files; %2 errors.")
                .arg(skipped).arg(result.errors.size());
            QStringList details;
            for (const auto& path : result.skippedProtected)
                details << tr("Unsaved or busy: %1").arg(path);
            for (const auto& path : result.skippedChanged)
                details << tr("Changed since search: %1").arg(path);
            details << result.errors;
            status_->setToolTip(details.join('\n'));
        }
        emit replaceFinished(result);
        if (skipped || !result.errors.isEmpty())
            IconDialogs::MessageBox::warning(this, tr("Replace in Files"),
                                 replaceSummary_ + "\n\n" + status_->toolTip());
        startSearch();
    });
    connect(results_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        QSignalBlocker blocker(results_);
        if (!item->parent()) {
            const QString path = item->data(0, Qt::UserRole).toString();
            const bool checked = item->checkState(0) == Qt::Checked;
            for (const auto index : fileMatches_.value(path)) {
                if (selected_.testBit(index) == checked) continue;
                selected_.setBit(index, checked);
                selectedCount_ += checked ? 1 : -1;
            }
            selectedPerFile_[path] = checked ? fileMatches_.value(path).size() : 0;
            for (int index = 0; index < item->childCount(); ++index)
                if (item->child(index)->data(0, Qt::UserRole + 4).toLongLong() >= 0)
                    item->child(index)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        } else {
            auto* file = item->parent();
            const auto matchIndex = item->data(0, Qt::UserRole + 4).toLongLong();
            if (matchIndex < 0) return;
            const bool checked = item->checkState(0) == Qt::Checked;
            if (selected_.testBit(matchIndex) != checked) {
                selected_.setBit(matchIndex, checked);
                selectedCount_ += checked ? 1 : -1;
                const QString path = file->data(0, Qt::UserRole).toString();
                selectedPerFile_[path] += checked ? 1 : -1;
                const auto count = selectedPerFile_.value(path);
                file->setCheckState(0, count == 0 ? Qt::Unchecked
                    : count == fileMatches_.value(path).size() ? Qt::Checked : Qt::PartiallyChecked);
            }
        }
        updateReplaceButton();
    });
    connect(results_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->data(0, Qt::UserRole + 4).toLongLong() == -1) {
            const QString path = item->data(0, Qt::UserRole).toString();
            QTimer::singleShot(0, this, [this, path] { loadMore(path, 200); });
        }
    });
    connect(results_, &QTreeWidget::itemEntered, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !item->parent()) return;
        const auto index = item->data(0, Qt::UserRole + 4).toLongLong();
        if (index < 0 || index >= currentResult_.matches.size()) return;
        const auto& match = currentResult_.matches[index];
        if (!replaceToggle_->isChecked()) { item->setToolTip(0, match.path); return; }
        const QString replacement = ProjectReplaceService::previewReplacement(
            match, replacement_->text(), searchedOptions_.regularExpression);
        item->setToolTip(0, tr("Match: %1\nReplacement: %2\n%3")
            .arg(match.matchedText, replacement, match.path));
    });
    connect(results_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
        if (!item || item->childCount() > 0) return;
        if (item->data(0, Qt::UserRole + 4).toLongLong() == -1) return;
        emit fileActivated(item->data(0, Qt::UserRole).toString(),
                           item->data(0, Qt::UserRole + 1).toInt(),
                           item->data(0, Qt::UserRole + 2).toInt(),
                           item->data(0, Qt::UserRole + 3).toInt());
    });

    applyFontSettings();
}

void SearchPanel::setRoots(const QStringList& roots) {
    roots_ = roots;
    if (!query_->text().isEmpty()) scheduleSearch();
}

void SearchPanel::setShowHidden(bool show) {
    if (showHidden_ == show) return;
    showHidden_ = show;
    if (!query_->text().isEmpty()) scheduleSearch();
}

void SearchPanel::focusSearch() {
    query_->setFocus();
    query_->selectAll();
}

void SearchPanel::scheduleSearch() {
    if (replacing_) return;
    service_.cancel();
    currentResult_ = {};
    results_->clear();
    fileMatches_.clear();
    fileItems_.clear();
    loadedPerFile_.clear();
    selectedPerFile_.clear();
    selected_.clear();
    selectedCount_ = 0;
    replaceSummary_.clear();
    updateReplaceButton();
    if (query_->text().isEmpty()) {
        debounce_->stop();
        results_->clear();
        status_->setText(tr("Type to search across the workspace."));
        return;
    }
    debounce_->start();
}

ProjectSearchOptions SearchPanel::options() const {
    ProjectSearchOptions value;
    value.roots = roots_;
    value.query = query_->text();
    value.filesToInclude = include_->text();
    value.filesToExclude = exclude_->text();
    value.matchCase = matchCase_->isChecked();
    value.wholeWord = wholeWord_->isChecked();
    value.regularExpression = regex_->isChecked();
    value.showHidden = showHidden_;
    return value;
}

void SearchPanel::startSearch() {
    debounce_->stop();
    if (query_->text().isEmpty()) return;
    service_.cancel();
    {
        QSignalBlocker blocker(results_);
        results_->clear();
    }
    currentResult_ = {};
    fileMatches_.clear();
    fileItems_.clear();
    loadedPerFile_.clear();
    selectedPerFile_.clear();
    selected_.clear();
    selectedCount_ = 0;
    updateReplaceButton();
    if (roots_.isEmpty()) {
        status_->setText(tr("Open a folder to search files."));
        return;
    }
    searchedOptions_ = options();
    service_.search(searchedOptions_);
}

void SearchPanel::displayResult(const ProjectSearchResult& result) {
    currentResult_ = result;
    QSignalBlocker blocker(results_);
    results_->setUpdatesEnabled(false);
    results_->clear();
    fileMatches_.clear();
    fileItems_.clear();
    loadedPerFile_.clear();
    selectedPerFile_.clear();
    selected_.resize(result.matches.size());
    selected_.fill(true);
    selectedCount_ = result.matches.size();
    if (!result.error.isEmpty()) {
        status_->setText(tr("Invalid search: %1").arg(result.error));
        results_->setUpdatesEnabled(true);
        updateReplaceButton();
        return;
    }
    QStringList fileOrder;
    for (qsizetype matchIndex = 0; matchIndex < result.matches.size(); ++matchIndex) {
        const auto& match = result.matches[matchIndex];
        if (!fileMatches_.contains(match.path)) fileOrder.push_back(match.path);
        fileMatches_[match.path].push_back(matchIndex);
    }
    int initialBudget = 500;
    for (const auto& path : fileOrder) {
        const auto& indices = fileMatches_[path];
        auto* file = new QTreeWidgetItem(results_);
        file->setText(0, QStringLiteral("%1 — %2  (%3)")
            .arg(QFileInfo(path).fileName(), result.matches[indices.front()].relativePath)
            .arg(indices.size()));
        file->setToolTip(0, path);
        file->setIcon(0, FileIcons::icon(path));
        file->setData(0, Qt::UserRole, path);
        file->setFlags(file->flags() | Qt::ItemIsUserCheckable);
        file->setCheckState(0, Qt::Checked);
        fileItems_.insert(path, file);
        selectedPerFile_.insert(path, indices.size());
        const int initial = std::min<int>(50, std::min<int>(initialBudget, indices.size()));
        loadMore(path, initial);
        initialBudget -= initial;
        file->setExpanded(initial > 0);
    }
    status_->setText((replaceSummary_.isEmpty() ? QString() : replaceSummary_ + '\n') +
        tr("%1 results in %2 files searched")
        .arg(result.matches.size()).arg(result.filesSearched));
    results_->setUpdatesEnabled(true);
    updateReplacePreview();
    updateReplaceButton();
}

void SearchPanel::loadMore(const QString& path, int count) {
    auto* file = fileItems_.value(path, nullptr);
    if (!file) return;
    QSignalBlocker blocker(results_);
    if (file->childCount() > 0 &&
        file->child(file->childCount() - 1)->data(0, Qt::UserRole + 4).toLongLong() == -1)
        delete file->takeChild(file->childCount() - 1);
    const auto& indices = fileMatches_[path];
    const int start = loadedPerFile_.value(path);
    const int end = std::min<int>(indices.size(), start + count);
    for (int position = start; position < end; ++position) {
        const auto matchIndex = indices[position];
        const auto& match = currentResult_.matches[matchIndex];
        auto* item = new QTreeWidgetItem(file);
        item->setText(0, QStringLiteral("%1:%2  %3")
            .arg(match.line).arg(match.column).arg(match.preview));
        item->setToolTip(0, match.path);
        item->setData(0, Qt::UserRole, match.path);
        item->setData(0, Qt::UserRole + 1, match.line);
        item->setData(0, Qt::UserRole + 2, match.column);
        item->setData(0, Qt::UserRole + 3, match.length);
        item->setData(0, Qt::UserRole + 4, static_cast<qlonglong>(matchIndex));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, selected_.testBit(matchIndex) ? Qt::Checked : Qt::Unchecked);
    }
    loadedPerFile_[path] = end;
    if (end < indices.size()) {
        auto* more = new QTreeWidgetItem(file);
        more->setText(0, tr("Show next %1 of %2 matches…")
            .arg(std::min<int>(200, indices.size() - end)).arg(indices.size() - end));
        more->setData(0, Qt::UserRole, path);
        more->setData(0, Qt::UserRole + 4, -1);
    }
}

void SearchPanel::updateReplacePreview() {
    results_->viewport()->update();
}

void SearchPanel::updateReplaceButton() {
    if (replacing_) return;
    replaceButton_->setEnabled(replaceToggle_->isChecked() && selectedCount_ > 0 &&
                               currentResult_.error.isEmpty());
    replaceButton_->setToolTip(tr("Replace %1 checked matches in files").arg(selectedCount_));
}

void SearchPanel::requestReplace() {
    if (!replaceButton_->isEnabled()) return;
    ProjectReplaceRequest request;
    request.matches = currentResult_.matches;
    request.selected = selected_;
    request.fileHashes = currentResult_.fileHashes;
    request.replacement = replacement_->text();
    request.regularExpression = searchedOptions_.regularExpression;
    for (auto it = selectedPerFile_.cbegin(); it != selectedPerFile_.cend(); ++it)
        if (it.value() > 0) request.selectedPaths.push_back(it.key());
    if (!request.matches.isEmpty()) emit replaceRequested(request);
}

void SearchPanel::startReplace(ProjectReplaceRequest request) {
    if (replacing_ || request.matches.isEmpty()) return;
    replacing_ = true;
    service_.cancel();
    debounce_->stop();
    for (QWidget* field : std::initializer_list<QWidget*>{query_, replacement_, include_, exclude_,
                           matchCase_, wholeWord_, regex_, results_}) field->setEnabled(false);
    replaceButton_->setText(tr("Cancel"));
    replaceButton_->setEnabled(true);
    status_->setText(tr("Replacing…"));
    replaceService_.replace(std::move(request));
}
