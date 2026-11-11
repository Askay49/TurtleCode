#pragma once
#include <QAbstractScrollArea>
#include <QColor>
#include <vterm.h>
#include <deque>
#include <vector>

class TerminalWidget final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;
    void feedOutput(const QByteArray& data);
    void reset();
    void applyFontSettings();
    void pasteClipboard();
    void copySelection();
    QString selectedText() const;
    QString screenText() const;
    int rows() const { return rows_; }
    int columns() const { return columns_; }
signals:
    void input(const QByteArray& data);
    void terminalSizeChanged(int rows, int columns);
protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void inputMethodEvent(QInputMethodEvent*) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    void updateSize();
    void applyTheme();
    void syncScroll(bool follow);
    void clearSelection();
    QPoint cellAt(const QPointF& point) const;
    VTermScreenCell cell(int absoluteRow, int column) const;
    QColor color(VTermColor value, bool foreground) const;
    void pasteText(QString text);
    void sendText(const QString& text, VTermModifier modifiers = VTERM_MOD_NONE);
    bool reportMouse(QMouseEvent* event, bool pressed, bool motion);
    VTerm* terminal_{nullptr};
    VTermScreen* screen_{nullptr};
    int rows_{24}, columns_{80};
    int cellWidth_{8}, cellHeight_{16}, ascent_{12};
    VTermPos cursor_{};
    bool cursorVisible_{true}, alternate_{false}, reverse_{false};
    int cursorShape_{VTERM_PROP_CURSORSHAPE_BLOCK}, mouseMode_{VTERM_PROP_MOUSE_NONE};
    std::deque<std::vector<VTermScreenCell>> history_;
    size_t historyBytes_{0};
    QPoint selectionStart_{-1,-1}, selectionEnd_{-1,-1};
    bool selecting_{false};
    QColor foreground_, background_, cursorColor_, selectionBackground_, selectionForeground_;
    QString preedit_;
    bool batchingOutput_{false};
    QByteArray batchedOutput_;
};
