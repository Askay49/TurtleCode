#pragma once
#include <QPixmap>
#include <QString>
class QWidget;
class QPainter;
class QRect;
class QColor;

// One image and coordinate system for the entire workspace, independent of scrolling.
class WorkspaceBackground {
public:
    static WorkspaceBackground& instance();
    void reload();
    bool enabled() const { return !image_.isNull() && opacity_ > 0; }
    void paint(QWidget* surface, QPainter& painter, const QRect& rect, const QColor& base);
private:
    QPixmap image_;
    QPixmap scaled_;
    QSize cachedSize_;
    qreal cachedDpr_{0};
    qreal opacity_{0.15};
};
