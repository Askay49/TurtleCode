#pragma once

#include "Codicons.hpp"

#include <QColorDialog>
#include <QCoreApplication>
#include <QEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>

// Use controlled Qt dialogs so native/platform icons cannot replace Codicons.
namespace IconDialogs {
inline QIcon buttonIcon(QDialogButtonBox::StandardButton button) {
    const char* name = "check";
    switch (button) {
    case QDialogButtonBox::Save: case QDialogButtonBox::SaveAll: name = "save"; break;
    case QDialogButtonBox::Open: name = "go-to-file"; break;
    case QDialogButtonBox::Cancel: case QDialogButtonBox::Close:
    case QDialogButtonBox::No: case QDialogButtonBox::NoToAll: name = "close"; break;
    case QDialogButtonBox::Discard: name = "trash"; break;
    case QDialogButtonBox::Reset: case QDialogButtonBox::RestoreDefaults: name = "discard"; break;
    case QDialogButtonBox::Retry: name = "refresh"; break;
    case QDialogButtonBox::Abort: name = "debug-stop"; break;
    case QDialogButtonBox::Help: name = "question"; break;
    default: break;
    }
    return Codicons::icon(QString::fromLatin1(name));
}

inline void decorate(QDialogButtonBox* box) {
    for (auto* button : box->buttons()) {
        const auto standard = box->standardButton(button);
        if (standard != QDialogButtonBox::NoButton) button->setIcon(buttonIcon(standard));
    }
}
inline void decorate(QWidget* dialog) {
    for (auto* box : dialog->findChildren<QDialogButtonBox*>()) decorate(box);
}

class ButtonBox : public QDialogButtonBox {
public:
    explicit ButtonBox(StandardButtons buttons, QWidget* parent = nullptr)
        : QDialogButtonBox(buttons, parent) { decorate(this); }
};

inline void decorate(QMessageBox* dialog) {
    const char* name = nullptr;
    switch (dialog->icon()) {
    case QMessageBox::Information: name = "info"; break;
    case QMessageBox::Warning: name = "warning"; break;
    case QMessageBox::Critical: name = "error"; break;
    case QMessageBox::Question: name = "question"; break;
    default: break;
    }
    if (name) dialog->setIconPixmap(Codicons::icon(QString::fromLatin1(name)).pixmap(32, 32));
    for (auto* button : dialog->buttons()) {
        const auto standard = dialog->standardButton(button);
        if (standard != QMessageBox::NoButton)
            button->setIcon(buttonIcon(static_cast<QDialogButtonBox::StandardButton>(standard)));
    }
}

class MessageBox : public QMessageBox {
public:
    using QMessageBox::QMessageBox;
    int exec() override {
        decorate(this);
        return QMessageBox::exec();
    }
    static StandardButton showMessage(Icon icon, QWidget* parent, const QString& title,
                                     const QString& text, StandardButtons buttons, StandardButton defaultButton) {
        MessageBox dialog(icon, title, text, buttons, parent);
        if (defaultButton != NoButton) dialog.setDefaultButton(defaultButton);
        const int result = dialog.exec();
        return result == -1 ? Cancel : static_cast<StandardButton>(result);
    }
    static StandardButton warning(QWidget* parent, const QString& title, const QString& text,
                                  StandardButtons buttons = Ok, StandardButton defaultButton = NoButton) {
        return showMessage(Warning, parent, title, text, buttons, defaultButton);
    }
    static StandardButton critical(QWidget* parent, const QString& title, const QString& text,
                                   StandardButtons buttons = Ok, StandardButton defaultButton = NoButton) {
        return showMessage(Critical, parent, title, text, buttons, defaultButton);
    }
    static StandardButton information(QWidget* parent, const QString& title, const QString& text,
                                      StandardButtons buttons = Ok, StandardButton defaultButton = NoButton) {
        return showMessage(Information, parent, title, text, buttons, defaultButton);
    }
    static StandardButton question(QWidget* parent, const QString& title, const QString& text,
                                   StandardButtons buttons = StandardButtons(Yes | No), StandardButton defaultButton = NoButton) {
        return showMessage(Question, parent, title, text, buttons, defaultButton);
    }
};

class InputDialog : public QInputDialog {
public:
    using QInputDialog::QInputDialog;
    void setVisible(bool visible) override {
        QInputDialog::setVisible(visible);
        if (visible) decorate(this); // QInputDialog creates its button box lazily.
    }
    static QString getText(QWidget* parent, const QString& title, const QString& label,
                           QLineEdit::EchoMode echo = QLineEdit::Normal, const QString& text = {},
                           bool* ok = nullptr, Qt::WindowFlags flags = {},
                           Qt::InputMethodHints hints = Qt::ImhNone) {
        InputDialog dialog(parent, flags);
        dialog.setWindowTitle(title); dialog.setLabelText(label);
        dialog.setTextEchoMode(echo); dialog.setTextValue(text); dialog.setInputMethodHints(hints);
        const bool accepted = dialog.exec() == QDialog::Accepted;
        if (ok) *ok = accepted;
        return accepted ? dialog.textValue() : QString();
    }
    static int getInt(QWidget* parent, const QString& title, const QString& label, int value = 0,
                      int minimum = -2147483647, int maximum = 2147483647, int step = 1,
                      bool* ok = nullptr, Qt::WindowFlags flags = {}) {
        InputDialog dialog(parent, flags);
        dialog.setWindowTitle(title); dialog.setLabelText(label);
        dialog.setInputMode(IntInput); dialog.setIntRange(minimum, maximum);
        dialog.setIntStep(step); dialog.setIntValue(value);
        const bool accepted = dialog.exec() == QDialog::Accepted;
        if (ok) *ok = accepted;
        return accepted ? dialog.intValue() : value;
    }
};

// QFileDialog creates its own overwrite/delete confirmations inside Qt.
class MessageDecoration : public QObject {
    QWidget* root_;
    bool belongsToDialog(QObject* object) const {
        for (auto* parent = object->parent(); parent; parent = parent->parent())
            if (parent == root_) return true;
        return false;
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show)
            if (auto* box = qobject_cast<QMessageBox*>(object); box && belongsToDialog(box)) {
                decorate(box);
                // Qt may refresh standard button icons in its showEvent.
                QTimer::singleShot(0, box, [box] { decorate(box); });
            }
        return false;
    }
public:
    explicit MessageDecoration(QWidget* root) : QObject(root), root_(root) {
        QCoreApplication::instance()->installEventFilter(this);
    }
};

class FileDialog : public QFileDialog {
    MessageDecoration messages_{this};
public:
    using QFileDialog::QFileDialog;
    void setVisible(bool visible) override {
        QFileDialog::setVisible(visible);
        if (!visible) return;
        decorate(this);
        const struct { const char* object; const char* icon; } tools[] = {
            {"backButton", "arrow-left"}, {"forwardButton", "arrow-right"},
            {"toParentButton", "arrow-up"}, {"newFolderButton", "new-folder"},
            {"listModeButton", "list-unordered"}, {"detailModeButton", "list-tree"}
        };
        for (const auto& tool : tools)
            if (auto* button = findChild<QToolButton*>(QString::fromLatin1(tool.object)))
                button->setIcon(Codicons::icon(QString::fromLatin1(tool.icon)));
    }
    static QString fileName(QWidget* parent, const QString& caption, const QString& dir,
                            const QString& filter, QString* selectedFilter, Options options, AcceptMode mode) {
        FileDialog dialog(parent, caption, dir, filter);
        dialog.setOptions(options | DontUseNativeDialog);
        dialog.setAcceptMode(mode);
        dialog.setFileMode(mode == AcceptSave ? AnyFile : ExistingFile);
        if (selectedFilter && !selectedFilter->isEmpty()) dialog.selectNameFilter(*selectedFilter);
        if (dialog.exec() != QDialog::Accepted) return {};
        if (selectedFilter) *selectedFilter = dialog.selectedNameFilter();
        return dialog.selectedFiles().value(0);
    }
    static QString getOpenFileName(QWidget* parent = nullptr, const QString& caption = {},
                                   const QString& dir = {}, const QString& filter = {},
                                   QString* selectedFilter = nullptr, Options options = {}) {
        return fileName(parent, caption, dir, filter, selectedFilter, options, AcceptOpen);
    }
    static QString getSaveFileName(QWidget* parent = nullptr, const QString& caption = {},
                                   const QString& dir = {}, const QString& filter = {},
                                   QString* selectedFilter = nullptr, Options options = {}) {
        return fileName(parent, caption, dir, filter, selectedFilter, options, AcceptSave);
    }
    static QString getExistingDirectory(QWidget* parent = nullptr, const QString& caption = {},
                                        const QString& dir = {}, Options options = ShowDirsOnly) {
        FileDialog dialog(parent, caption, dir);
        dialog.setOptions(options | DontUseNativeDialog);
        dialog.setFileMode(Directory);
        return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0) : QString();
    }
};

class ColorDialog : public QColorDialog {
public:
    using QColorDialog::QColorDialog;
    void setVisible(bool visible) override {
        QColorDialog::setVisible(visible);
        if (visible) {
            decorate(this);
            for (auto* button : findChildren<QPushButton*>())
                if (button->icon().isNull())
                    button->setIcon(Codicons::icon(button->text() == QColorDialog::tr("&Pick Screen Color")
                                                  ? "symbol-color" : "add"));
        }
    }
    static QColor getColor(const QColor& initial = Qt::white, QWidget* parent = nullptr,
                           const QString& title = {}, ColorDialogOptions options = {}) {
        ColorDialog dialog(initial, parent);
        dialog.setOptions(options | DontUseNativeDialog);
        if (!title.isEmpty()) dialog.setWindowTitle(title);
        return dialog.exec() == QDialog::Accepted ? dialog.selectedColor() : QColor();
    }
};

class ProgressDialog : public QProgressDialog {
public:
    ProgressDialog(const QString& label, const QString& cancelText, int minimum, int maximum, QWidget* parent)
        : QProgressDialog(label, cancelText, minimum, maximum, parent) {
        setCancelButton(cancelText.isEmpty() ? nullptr : new QPushButton(Codicons::icon("close"), cancelText, this));
    }
};
}
