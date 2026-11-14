#pragma once
#include <QDialog>
#include <QIcon>
#include <QVector>
#include <functional>
class QLineEdit;
class QTreeWidget;
class QLabel;
struct PaletteCommand {
    QString id, title, searchText, shortcut, reason;
    QIcon icon;
    bool enabled{true};
    int checked{-1};
    std::function<void()> execute;
};
class CommandPalette final : public QDialog {
public:
    explicit CommandPalette(QVector<PaletteCommand> commands, QWidget* parent);
    void executeSelected();
    static int matchScore(const QString& text, const QString& query);
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    void refresh();
    void acceptCurrent();
    QVector<PaletteCommand> commands_;
    QLineEdit* input_;
    QTreeWidget* results_;
    QLabel* status_;
    QStringList recent_;
    int selected_{-1};
};
