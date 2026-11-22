#pragma once
#include "app/Codicons.hpp"
#include "app/ThemeManager.hpp"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

namespace DebugViews {
enum ItemRole {
    UnavailableRole = Qt::UserRole + 3,
    ExpressionRole,
    ParentReferenceRole,
    ReadOnlyRole,
    HasValueRole
};
class ConfigurationCombo final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent* event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        Codicons::icon("chevron-down").paint(&painter,QRect(width()-17,(height()-14)/2,14,14));
    }
};

class ElidedLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

// Preserve the two model columns for evaluation and copying, but paint each
// expression and its value inline like a debugger inspector.
class ValueDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool edit(QTreeWidget* tree, QTreeWidgetItem* item, bool value, const QString& label,
              std::function<void(const QString&)> submit, std::function<void()> closed = {}) {
        const Edit request{value, label, std::move(submit), std::move(closed)};
        const QScopedValueRollback editing(request_, &request);
        tree->setCurrentItem(item);
        tree->scrollToItem(item);
        tree->setFocus(Qt::OtherFocusReason);
        editingStarted_ = false;
        tree->editItem(item);
        return editingStarted_;
    }

    static QRect valueArea(const QRect& rect, const QFontMetrics& metrics, const QModelIndex& index) {
        const auto area = textArea(rect);
        const auto width = metrics.horizontalAdvance(index.data().toString() + QStringLiteral(": ")) + 2;
        return area.adjusted(qMin(width, qMax(0, area.width() - 24)), 0, 0, 0);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto styled = option; initStyleOption(&styled, index); styled.text.clear();
        const auto* widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
        const auto value = index.sibling(index.row(), 1).data().toString();
        const auto& theme = ThemeManager::instance();
        const auto area = textArea(option.rect);
        QFont font = option.font;
        QFontMetrics metrics(font);
        const bool hasValue = !value.isEmpty() || index.data(HasValueRole).toBool();
        const auto name = index.data().toString() + (hasValue ? QStringLiteral(": ") : QString{});
        const int naturalWidth = metrics.horizontalAdvance(name) + 2;
        const auto valueRect = valueArea(option.rect, metrics, index);
        const int nameWidth = hasValue ? valueRect.left() - area.left() : area.width();
        painter->save(); painter->setClipRect(area); painter->setFont(font);
        painter->setPen(theme.color(hasValue ? "syntax.property" : "text.primary"));
        painter->drawText(QRect(area.left(), area.top(), nameWidth, area.height()), Qt::AlignVCenter,
                          naturalWidth <= nameWidth ? name : metrics.elidedText(name, Qt::ElideRight, nameWidth));
        if (!value.isEmpty()) {
            const bool unavailable = index.data(UnavailableRole).toBool();
            font.setItalic(unavailable); painter->setFont(font);
            painter->setPen(theme.color(unavailable ? "text.muted" : "text.primary"));
            painter->drawText(valueRect, Qt::AlignVCenter, QFontMetrics(font).elidedText(value, Qt::ElideRight, valueRect.width()));
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {40, qMax(22, option.fontMetrics.height() + 4)};
    }

protected:
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        if (!request_) return nullptr;
        auto* editor = new Editor(*request_, parent);
        editor->setObjectName("debugInlineEdit");
        editor->setAccessibleName(request_->label);
        editor->setFont(option.font);
        editor->setStyleSheet(QStringLiteral("QLineEdit { padding: 0px 2px; }"));
        editingStarted_ = true;
        return editor;
    }

    void setEditorData(QWidget* widget, const QModelIndex& index) const override {
        auto* editor = static_cast<Editor*>(widget);
        if (editor->initialized) return;
        editor->initialized = true;
        const auto source = editor->request.value ? index.siblingAtColumn(1) : index;
        editor->setText(source.data().toString());
        editor->selectAll();
    }

    void updateEditorGeometry(QWidget* widget, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto* editor = static_cast<Editor*>(widget);
        const auto area = editor->request.value ? valueArea(option.rect, option.fontMetrics, index) : textArea(option.rect);
        widget->setGeometry(area);
    }

    void setModelData(QWidget* widget, QAbstractItemModel*, const QModelIndex&) const override {
        const auto* editor = static_cast<Editor*>(widget);
        if (!editor->acceptRequested) return;
        // Applying edits can rebuild the tree. Let Qt finish closing the editor first.
        QMetaObject::invokeMethod(parent(), [submit = editor->request.submit, text = editor->text()] { submit(text); },
                                  Qt::QueuedConnection);
    }

    void destroyEditor(QWidget* widget, const QModelIndex& index) const override {
        const auto* editor = static_cast<Editor*>(widget);
        if (editor->request.closed) QMetaObject::invokeMethod(parent(), editor->request.closed, Qt::QueuedConnection);
        QStyledItemDelegate::destroyEditor(widget, index);
    }

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) static_cast<Editor*>(object)->acceptRequested = true;
        }
        if (event->type() == QEvent::FocusOut) {
            const auto reason = static_cast<QFocusEvent*>(event)->reason();
            if (reason != Qt::PopupFocusReason && reason != Qt::ActiveWindowFocusReason)
                emit closeEditor(static_cast<QWidget*>(object));
            return false;
        }
        if (event->type() == QEvent::Hide) {
            emit closeEditor(static_cast<QWidget*>(object));
            return false;
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

private:
    struct Edit {
        bool value;
        QString label;
        std::function<void(const QString&)> submit;
        std::function<void()> closed;
    };
    class Editor final : public QLineEdit {
    public:
        Editor(const Edit& edit, QWidget* parent) : QLineEdit(parent), request(edit) {}
        Edit request;
        bool initialized{false};
        bool acceptRequested{false};
    };

    static QRect textArea(const QRect& rect) { return rect.adjusted(3, 0, -5, 0); }

    const Edit* request_{nullptr};
    mutable bool editingStarted_{false};
};

class ValueTree final : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    enum class EditTarget { Current, Name, Value };
    std::function<bool(QTreeWidgetItem*, EditTarget)> editRow;

protected:
    bool event(QEvent* event) override {
        if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && hasFocus()) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_F2 && key->modifiers() == Qt::NoModifier && currentItem() && editRow) {
                // F2 also belongs to the window's Rename Symbol action.
                if (event->type() == QEvent::KeyPress) editRow(currentItem(), EditTarget::Current);
                event->accept();
                return true;
            }
        }
        return QTreeWidget::event(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        const auto point = event->position().toPoint();
        const auto index = indexAt(point);
        const auto rect = visualRect(index);
        if (event->button() == Qt::LeftButton && index.isValid() && rect.contains(point) && editRow) {
            const bool onValue = ValueDelegate::valueArea(rect, fontMetrics(), index).contains(point);
            if (editRow(itemFromIndex(index), onValue ? EditTarget::Value : EditTarget::Name)) return;
        }
        QTreeWidget::mouseDoubleClickEvent(event);
    }
};

class Section final : public QWidget {
public:
    Section(const QString& id, const QString& title, QTreeWidget* tree, QSplitter* splitter) : QWidget(splitter) {
        setObjectName(id + "Section");
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        auto* header = new QWidget(this); header->setObjectName("debugSectionHeader"); header->setFixedHeight(26);
        actions = new QHBoxLayout(header); actions->setContentsMargins(2, 0, 4, 0); actions->setSpacing(0);
        auto* toggle = new QToolButton(header); toggle->setObjectName(id + "Toggle");
        toggle->setText(title); toggle->setAccessibleName(title); toggle->setCheckable(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        actions->addWidget(toggle, 1); layout->addWidget(header); layout->addWidget(tree, 1);
        const auto key = "debug/ui/" + id + "/expanded";
        const auto update = [this, tree, toggle, key, splitter](bool expanded) {
            toggle->setIcon(Codicons::icon(expanded ? "chevron-down" : "chevron-right"));
            const int index = splitter->indexOf(this);
            auto sizes = splitter->sizes();
            if (!expanded && index >= 0) expandedHeight_ = qMax(80, sizes.value(index));
            tree->setVisible(expanded);
            setMinimumHeight(expanded ? 52 : 26); setMaximumHeight(expanded ? QWIDGETSIZE_MAX : 26);
            if (index >= 0) { sizes[index] = expanded ? expandedHeight_ : 26; splitter->setSizes(sizes); }
            QSettings().setValue(key, expanded);
        };
        toggle->setChecked(QSettings().value(key, true).toBool());
        update(toggle->isChecked());
        connect(toggle, &QToolButton::toggled, this, update);
    }
    QHBoxLayout* actions;
private:
    int expandedHeight_{200};
};
}
