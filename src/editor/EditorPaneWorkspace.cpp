#include "EditorPaneWorkspace.hpp"
#include "EditorWidget.hpp"

#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

EditorPane::EditorPane() {
    setObjectName("editorPane");
}

EditorWidget* EditorPane::editor() const {
    return qobject_cast<EditorWidget*>(currentWidget());
}

EditorWidget* EditorPane::viewFor(const Document* document) const {
    for (int i = 0; i < count(); ++i) {
        auto* view = qobject_cast<EditorWidget*>(widget(i));
        if (view && view->document().get() == document) return view;
    }
    return nullptr;
}

class EditorPaneWorkspace::Page final : public QWidget {
public:
    Page() {
        auto* body = new QVBoxLayout(this);
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
    }
    QWidget* root() const { return layout()->count() ? layout()->itemAt(0)->widget() : nullptr; }
    QPointer<EditorPane> active, previous;
};

namespace {
QSplitter* makeSplitter(Qt::Orientation orientation) {
    auto* splitter = new QSplitter(orientation);
    splitter->setObjectName("editorPaneSplitter");
    splitter->setChildrenCollapsible(false);
    return splitter;
}

void collectPanes(QWidget* widget, QList<EditorPane*>& result) {
    if (auto* pane = dynamic_cast<EditorPane*>(widget)) {
        result.append(pane);
    } else if (auto* splitter = qobject_cast<QSplitter*>(widget)) {
        for (int i = 0; i < splitter->count(); ++i) collectPanes(splitter->widget(i), result);
    }
}
}

EditorPaneWorkspace::EditorPaneWorkspace(QWidget* parent) : QStackedWidget(parent) {
    setObjectName("editorPaneWorkspace");
}

EditorPaneWorkspace::Page* EditorPaneWorkspace::page() const {
    return static_cast<Page*>(currentWidget());
}

EditorPane* EditorPaneWorkspace::activePane() const {
    return page() ? page()->active.data() : nullptr;
}

EditorWidget* EditorPaneWorkspace::activeEditor() const {
    return activePane() ? activePane()->editor() : nullptr;
}

EditorWidget* EditorPaneWorkspace::pageEditor(int index) const {
    const auto* owner = static_cast<Page*>(widget(index));
    return owner && owner->active ? owner->active->editor() : nullptr;
}

QList<EditorPane*> EditorPaneWorkspace::panes(bool allPages) const {
    QList<EditorPane*> result;
    if (allPages) {
        for (int i = 0; i < count(); ++i) collectPanes(static_cast<Page*>(widget(i))->root(), result);
    } else if (page()) {
        collectPanes(page()->root(), result);
    }
    return result;
}

QList<EditorPane*> EditorPaneWorkspace::panesOnPage(int index) const {
    QList<EditorPane*> result;
    if (auto* owner = static_cast<Page*>(widget(index))) collectPanes(owner->root(), result);
    return result;
}

EditorPane* EditorPaneWorkspace::paneFor(const EditorWidget* editor) const {
    for (auto* pane : panes(true)) if (pane->indexOf(editor) >= 0) return pane;
    return nullptr;
}

void EditorPaneWorkspace::selectPane(EditorPane* pane) {
    if (!pane) return;
    for (int i = 0; i < count(); ++i) {
        auto* owner = static_cast<Page*>(widget(i));
        if (!owner->isAncestorOf(pane)) continue;
        selectPage(i);
        if (owner->active != pane) {
            owner->previous = owner->active;
            owner->active = pane;
        }
        return;
    }
}

QWidget* EditorPaneWorkspace::replaceInLayout(QWidget* before, QWidget* after) {
    auto* parent = before->parentWidget();
    if (auto* splitter = qobject_cast<QSplitter*>(parent)) {
        splitter->replaceWidget(splitter->indexOf(before), after);
    } else {
        delete parent->layout()->replaceWidget(before, after);
        before->setParent(nullptr);
    }
    after->show();
    return after;
}

EditorPane* EditorPaneWorkspace::split(Qt::Orientation orientation) {
    auto* current = activePane();
    if (!current) return nullptr;
    auto* next = new EditorPane;
    auto* parent = qobject_cast<QSplitter*>(current->parentWidget());
    if (parent && parent->orientation() == orientation) {
        auto sizes = parent->sizes();
        const int index = parent->indexOf(current);
        const int available = std::max(2, sizes[index] - parent->handleWidth());
        const int half = available / 2;
        sizes[index] = available - half;
        sizes.insert(index + 1, half);
        parent->insertWidget(index + 1, next);
        next->show();
        parent->setSizes(sizes);
        selectPane(next);
        return next;
    }
    // QSplitter clamps small weights to each widget's minimum. Use the existing
    // pane's extent so the empty new pane and the populated pane get equal weights.
    const auto size = current->size().expandedTo(current->minimumSizeHint());
    const int weight = std::max(1, orientation == Qt::Horizontal ? size.width() : size.height());
    auto* splitter = makeSplitter(orientation);
    replaceInLayout(current, splitter);
    splitter->addWidget(current);
    current->show();
    splitter->addWidget(next);
    next->show();
    splitter->setSizes({weight, weight});
    selectPane(next);
    return next;
}

void EditorPaneWorkspace::detachPane(EditorPane* pane) {
    auto* splitter = qobject_cast<QSplitter*>(pane->parentWidget());
    pane->hide();
    pane->setParent(nullptr);
    if (!splitter || splitter->count() != 1) return;
    auto* remaining = splitter->widget(0);
    remaining->setParent(nullptr);
    replaceInLayout(splitter, remaining);
    splitter->deleteLater();
}

void EditorPaneWorkspace::removePage(Page* closing) {
    const int index = indexOf(closing);
    removeWidget(closing);
    if (count()) setCurrentIndex(std::min(index, count() - 1));
    if (previousPage_ == closing) previousPage_.clear();
    closing->deleteLater();
}

bool EditorPaneWorkspace::closePane(EditorPane* pane) {
    if (!pane) return false;
    Page* owner = nullptr;
    for (int i = 0; i < count(); ++i) {
        auto* candidate = static_cast<Page*>(widget(i));
        if (candidate->isAncestorOf(pane)) { owner = candidate; break; }
    }
    if (!owner) return false;
    QList<EditorPane*> siblings;
    collectPanes(owner->root(), siblings);
    if (siblings.size() == 1) {
        if (count() == 1) return false;
        if (owner == page()) removePage(owner);
        else {
            removeWidget(owner);
            if (previousPage_ == owner) previousPage_.clear();
            owner->deleteLater();
        }
    } else {
        const auto index = siblings.indexOf(pane);
        if (owner->active == pane) owner->active = siblings[index > 0 ? index - 1 : index + 1];
        if (owner->previous == pane) owner->previous.clear();
        detachPane(pane);
        pane->deleteLater();
    }
    return true;
}

void EditorPaneWorkspace::keepPane(EditorPane* pane) {
    for (auto* other : panes()) if (other != pane) closePane(other);
}

EditorPane* EditorPaneWorkspace::newPage(int after) {
    auto* owner = new Page;
    auto* pane = new EditorPane;
    owner->layout()->addWidget(pane);
    owner->active = pane;
    insertWidget(after < 0 ? currentIndex() + 1 : std::min(int(count()), after + 1), owner);
    selectPage(indexOf(owner));
    return pane;
}

bool EditorPaneWorkspace::movePaneToNewPage() {
    if (panes().size() < 2) return false;
    auto* moving = activePane();
    auto* old = page();
    const auto others = panes();
    old->active = others.first() == moving ? others[1] : others.first();
    old->previous.clear();
    detachPane(moving);
    auto* placeholder = newPage();
    replaceInLayout(placeholder, moving);
    page()->active = moving;
    placeholder->deleteLater();
    return true;
}

bool EditorPaneWorkspace::selectPage(int index) {
    if (index < 0 || index >= count()) return false;
    if (index != currentIndex()) {
        previousPage_ = currentWidget();
        setCurrentIndex(index);
    }
    return true;
}

void EditorPaneWorkspace::movePage(int target) {
    auto* current = currentWidget();
    if (!current) return;
    removeWidget(current);
    insertWidget(std::clamp(target, 0, count()), current);
    setCurrentWidget(current);
}

int EditorPaneWorkspace::previousPage() const {
    return indexOf(previousPage_);
}

EditorPane* EditorPaneWorkspace::focusTarget(QChar direction, int count, bool explicitCount) const {
    const auto ordered = panes();
    if (ordered.isEmpty()) return nullptr;
    auto* current = activePane();
    if (direction == 't') return ordered.first();
    if (direction == 'b') return ordered.last();
    if (direction == 'p') return page()->previous ? page()->previous.data() : current;
    if (direction == 'w' || direction == 'W') {
        const int size = int(ordered.size());
        if (explicitCount) return ordered[std::min(count, size) - 1];
        const int step = direction == 'w' ? 1 : -1;
        return ordered[(ordered.indexOf(current) + step + size) % size];
    }
    const bool horizontal = direction == 'h' || direction == 'l';
    const bool forward = direction == 'l' || direction == 'j';
    const auto area = [this](EditorPane* item) { return QRect(item->mapTo(this, QPoint()), item->size()); };
    while (count-- > 0) {
        const auto from = area(current);
        EditorPane* best = nullptr;
        int bestGap = std::numeric_limits<int>::max(), bestCross = bestGap;
        for (auto* candidate : ordered) {
            if (candidate == current) continue;
            const auto to = area(candidate);
            const int gap = horizontal ? (forward ? to.left() - from.right() : from.left() - to.right())
                                       : (forward ? to.top() - from.bottom() : from.top() - to.bottom());
            if (gap < 0) continue;
            const int cross = horizontal ? std::max({0, to.top() - from.bottom(), from.top() - to.bottom()})
                                         : std::max({0, to.left() - from.right(), from.left() - to.right()});
            if (cross < bestCross || (cross == bestCross && gap < bestGap)) {
                best = candidate;
                bestCross = cross;
                bestGap = gap;
            }
        }
        if (!best) break;
        current = best;
    }
    return current;
}

void EditorPaneWorkspace::resizePane(Qt::Orientation orientation, int amount, bool absolute, int cellSize) {
    QWidget* branch = activePane();
    for (auto* splitter = qobject_cast<QSplitter*>(branch ? branch->parentWidget() : nullptr);
         splitter; branch = splitter, splitter = qobject_cast<QSplitter*>(splitter->parentWidget())) {
        if (splitter->orientation() != orientation || splitter->count() < 2) continue;
        auto sizes = splitter->sizes();
        const int index = splitter->indexOf(branch);
        const int neighbor = index + 1 < sizes.size() ? index + 1 : index - 1;
        const int maxSize = sizes[index] + sizes[neighbor];
        if (maxSize < 2) return;
        const int requested = amount * cellSize;
        const int size = std::clamp(absolute ? requested : sizes[index] + requested, 1, maxSize - 1);
        sizes[neighbor] = maxSize - size;
        sizes[index] = size;
        splitter->setSizes(sizes);
        return;
    }
}

void EditorPaneWorkspace::equalize() {
    if (!page()) return;
    for (auto* splitter : page()->findChildren<QSplitter*>("editorPaneSplitter"))
        splitter->setSizes(QList<int>(splitter->count(), 1));
}

void EditorPaneWorkspace::rotate(int count, bool backwards) {
    auto* splitter = qobject_cast<QSplitter*>(activePane() ? activePane()->parentWidget() : nullptr);
    if (!splitter || splitter->count() < 2) return;
    const int size = splitter->count();
    const auto sizes = splitter->sizes();
    for (int i = 0; i < count % size; ++i) {
        if (backwards) splitter->insertWidget(0, splitter->widget(size - 1));
        else splitter->insertWidget(size - 1, splitter->widget(0));
    }
    splitter->setSizes(sizes);
}

void EditorPaneWorkspace::exchange(int count, bool explicitCount) {
    auto* pane = activePane();
    auto* splitter = qobject_cast<QSplitter*>(pane ? pane->parentWidget() : nullptr);
    if (!splitter || splitter->count() < 2) return;
    const int at = splitter->indexOf(pane), size = splitter->count();
    const int target = explicitCount ? std::min(count, size) - 1 : (at + 1) % size;
    if (at == target) return;
    auto* other = splitter->widget(target);
    const auto sizes = splitter->sizes();
    splitter->insertWidget(target, pane);
    splitter->insertWidget(at, other);
    splitter->setSizes(sizes);
}

void EditorPaneWorkspace::moveToEdge(QChar direction) {
    auto* moving = activePane();
    if (!moving || panes().size() < 2) return;
    detachPane(moving);
    auto* root = page()->root();
    auto* splitter = makeSplitter(direction == 'H' || direction == 'L' ? Qt::Horizontal : Qt::Vertical);
    replaceInLayout(root, splitter);
    const bool first = direction == 'H' || direction == 'K';
    splitter->addWidget(first ? static_cast<QWidget*>(moving) : root);
    splitter->addWidget(first ? root : static_cast<QWidget*>(moving));
    moving->show();
    root->show();
    splitter->setSizes({1, 1});
}
