#include "WorkspaceBackground.hpp"
#include <QApplication>
#include <QFutureWatcher>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {
QImage readBackground(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}
}

WorkspaceBackground& WorkspaceBackground::instance() {
    static WorkspaceBackground background;
    return background;
}
QString WorkspaceBackground::resetForReload() {
    ++generation_;
    loading_ = false;
    QSettings settings;
    image_ = QPixmap();
    opacity_ = std::clamp(settings.value("appearance/backgroundOpacity", 15).toInt(), 0, 100) / 100.0;
    scaled_ = QPixmap();
    cachedSize_ = QSize();
    return opacity_ > 0 ? settings.value("appearance/backgroundImage").toString() : QString{};
}
void WorkspaceBackground::reload() {
    const auto path = resetForReload();
    if (!path.isEmpty()) image_ = QPixmap::fromImage(readBackground(path));
}
void WorkspaceBackground::reloadAsync(std::function<void()> onLoadError) {
    const auto path = resetForReload();
    if (path.isEmpty()) return;

    loading_ = true;
    auto* watcher = new QFutureWatcher<QImage>(qApp);
    QObject::connect(watcher, &QFutureWatcher<QImage>::finished, qApp,
                     [this, watcher, generation = generation_, onLoadError = std::move(onLoadError)] {
        watcher->deleteLater();
        if (generation != generation_) return;

        loading_ = false;
        image_ = QPixmap::fromImage(watcher->result());
        if (image_.isNull()) {
            onLoadError();
            return;
        }
        for (auto* window : QApplication::topLevelWidgets()) {
            if (window->objectName() != QStringLiteral("workspaceBackground")) continue;
            window->update();
            for (auto* child : window->findChildren<QWidget*>()) child->update();
        }
    });
    watcher->setFuture(QtConcurrent::run(readBackground, path));
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
