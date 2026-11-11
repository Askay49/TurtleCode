#pragma once

#include "Codicons.hpp"
#include "ThemeManager.hpp"
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QStylePainter>

// Stylesheet borders disable Qt's native spin indicators on some styles.
// Keep QSpinBox editing/stepping and draw explicit, theme-aware Codicons.
class SettingsSpinBox final : public QSpinBox {
public:
    explicit SettingsSpinBox(QWidget* parent = nullptr) : QSpinBox(parent) {
        setMinimumHeight(32);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        QStylePainter painter(this);
        option.subControls = QStyle::SC_SpinBoxFrame | QStyle::SC_SpinBoxEditField;
        painter.drawComplexControl(QStyle::CC_SpinBox, option);
        if (buttonSymbols() == QAbstractSpinBox::NoButtons) return;
        const auto steps = stepEnabled();
        const auto draw = [&](QStyle::SubControl control, const char* name, bool enabled) {
            const QRect button = style()->subControlRect(QStyle::CC_SpinBox, &option, control, this);
            const int size = qMin(11, qMin(button.width(), button.height()) - 4);
            if (size <= 0) return;
            const QRect icon(button.center() - QPoint(size / 2, size / 2), QSize(size, size));
            const auto color = ThemeManager::instance().color(
                isEnabled() && enabled ? "text.primary" : "text.muted");
            Codicons::icon(QString::fromLatin1(name), color).paint(&painter, icon);
        };
        draw(QStyle::SC_SpinBoxUp, "add", steps.testFlag(StepUpEnabled));
        draw(QStyle::SC_SpinBoxDown, "remove", steps.testFlag(StepDownEnabled));
    }
};
