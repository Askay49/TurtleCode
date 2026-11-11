#pragma once
#include "app/Codicons.hpp"
#include "app/ThemeManager.hpp"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

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
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto styled = option; initStyleOption(&styled, index); styled.text.clear();
        const auto* widget = option.widget;
        (widget ? widget->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &styled, painter, widget);
        const auto value = index.sibling(index.row(), 1).data().toString();
        const auto& theme = ThemeManager::instance();
        const auto area = option.rect.adjusted(3, 0, -5, 0);
        QFont font = option.font;
        QFontMetrics metrics(font);
        const auto name = index.data().toString() + (value.isEmpty() ? QString{} : QStringLiteral(": "));
        const int naturalWidth = metrics.horizontalAdvance(name) + 2;
        const int nameWidth = value.isEmpty() ? area.width() : qMin(naturalWidth, qMax(0, area.width() - 24));
        painter->save(); painter->setClipRect(area); painter->setFont(font);
        painter->setPen(theme.color(value.isEmpty() ? "text.primary" : "syntax.property"));
        painter->drawText(QRect(area.left(), area.top(), nameWidth, area.height()), Qt::AlignVCenter,
                          naturalWidth <= nameWidth ? name : metrics.elidedText(name, Qt::ElideRight, nameWidth));
        if (!value.isEmpty()) {
            font.setItalic(index.data(Qt::UserRole + 3).toBool()); painter->setFont(font);
            painter->setPen(theme.color(index.data(Qt::UserRole + 3).toBool() ? "text.muted" : "text.primary"));
            const auto valueArea = area.adjusted(nameWidth, 0, 0, 0);
            painter->drawText(valueArea, Qt::AlignVCenter, QFontMetrics(font).elidedText(value, Qt::ElideRight, valueArea.width()));
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {40, qMax(22, option.fontMetrics.height() + 4)};
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
