#include "WorkspaceBackground.hpp"
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QWidget>
#include <algorithm>

WorkspaceBackground& WorkspaceBackground::instance() {
    static WorkspaceBackground background;
    return background;
}
void WorkspaceBackground::reload() {
    QSettings settings;
    const auto path = settings.value("appearance/backgroundImage").toString();
    image_ = QPixmap();
    if (!path.isEmpty()) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        image_ = QPixmap::fromImage(reader.read());
    }
    opacity_ = std::clamp(settings.value("appearance/backgroundOpacity", 15).toInt(), 0, 100) / 100.0;
    scaled_ = QPixmap();
    cachedSize_ = QSize();
}
void WorkspaceBackground::paint(QWidget* surface, QPainter& painter, const QRect& rect, const QColor& base) {
    painter.fillRect(rect, base);
    if (!enabled()) return;
    QWidget* workspace = surface;
    while (workspace && workspace->objectName() != "workspaceBackground") workspace = workspace->parentWidget();
    if (!workspace || workspace->size().isEmpty()) return;
    const qreal dpr = workspace->devicePixelRatioF();
    if (cachedSize_ != workspace->size() || cachedDpr_ != dpr || scaled_.isNull()) {
        cachedSize_ = workspace->size(); cachedDpr_ = dpr;
        scaled_ = image_.scaled(cachedSize_ * dpr, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        scaled_.setDevicePixelRatio(dpr);
    }
    const QSizeF logicalSize = QSizeF(scaled_.size()) / dpr;
    const QPointF origin((workspace->width() - logicalSize.width()) / 2,
                         (workspace->height() - logicalSize.height()) / 2);
    painter.save();
    painter.setClipRect(rect);
    painter.setOpacity(opacity_);
    painter.drawPixmap(origin - QPointF(surface->mapTo(workspace, QPoint())), scaled_);
    painter.restore();
}
