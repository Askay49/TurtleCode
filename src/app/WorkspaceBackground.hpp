#pragma once
#include <QPixmap>
#include <QString>
#include <functional>
class QWidget;
class QPainter;
class QRect;
class QColor;

// One image and coordinate system for the entire workspace, independent of scrolling.
class WorkspaceBackground {
public:
    static WorkspaceBackground& instance();
    void reload();
    void reloadAsync(std::function<void()> onLoadError);
    bool enabled() const { return !image_.isNull() && opacity_ > 0; }
    bool usesWallpaperStyle() const { return loading_ || enabled(); }
    void paint(QWidget* surface, QPainter& painter, const QRect& rect, const QColor& base);
private:
    QString resetForReload();
    QPixmap image_;
    QPixmap scaled_;
    QSize cachedSize_;
    qreal cachedDpr_{0};
    qreal opacity_{0.15};
    quint64 generation_{0};
    bool loading_{false};
};
