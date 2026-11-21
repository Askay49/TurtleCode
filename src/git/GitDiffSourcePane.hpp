#pragma once

#include "GitDiffModel.hpp"
#include "core/text/PieceTreeBuffer.hpp"
#include <QAbstractScrollArea>
#include <QCache>
#include <QTextLayout>
#include <QTimer>

class GitDiffSourcePane final : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit GitDiffSourcePane(QWidget* parent);
    void setComparison(const QByteArray& source, const GitDiff::Model& model);
    void insertPadding(int row, int count);
    void clear();
    void setRowMetrics(int height, int top, int inset, int tabSize);
    int lineAtRow(int row) const { return alignment_.lineAt(row); }
    QRect rowRect(int row) const;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;
    void scrollContentsBy(int, int) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QTextLayout& layoutForLine(int line, std::size_t begin, std::size_t end);
    std::size_t lineEnd(int line) const;
    std::size_t positionAt(const QPoint& point);
    std::size_t positionAtColumn(int line, qreal x);
    qreal columnAtPosition(int line, std::size_t position);
    void revealPosition();
    void resetLayouts();
    void updateHorizontalRange();
    void updateDragSelection();
    void selectAll();
    void copy() const;

    QByteArray source_;
    PieceTreeBuffer buffer_;
    LineAlignment alignment_;
    QVector<GitDiff::Hunk> hunks_;
    QVector<QByteArray> rightLines_;
    QCache<int, QTextLayout> layouts_{128 * 1024};
    QTimer dragScroll_;
    QPoint dragPoint_;
    std::size_t anchor_{0}, position_{0};
    int rowHeight_{1}, topMargin_{0}, textInset_{12}, tabSize_{4}, gutter_{0}, widestLine_{0};
    bool dragging_{false};
};
