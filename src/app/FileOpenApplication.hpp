#pragma once

#include <QApplication>
#include <QDir>
#include <QFileOpenEvent>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <utility>

class FileOpenApplication final : public QApplication {
public:
    using QApplication::QApplication;

    QStringList takeFileOpenRequests() { return std::exchange(paths_, {}); }

    void setFileOpenHandler(QObject* context, std::function<void(const QStringList&)> handler) {
        context_ = context;
        handler_ = std::move(handler);
        scheduleFileOpen();
    }

protected:
    bool event(QEvent* event) override {
        if (event->type() != QEvent::FileOpen) return QApplication::event(event);
        const auto* request = static_cast<QFileOpenEvent*>(event);
        const auto url = request->url();
        const auto path = url.isLocalFile() ? url.toLocalFile()
            : url.scheme().isEmpty() ? request->file() : QString{};
        if (!QDir::isAbsolutePath(path)) return QApplication::event(event);

        paths_.append(path);
        scheduleFileOpen();
        return true;
    }

private:
    void scheduleFileOpen() {
        if (!context_ || paths_.isEmpty() || scheduled_) return;
        scheduled_ = true;
        QTimer::singleShot(0, this, [this] {
            scheduled_ = false;
            if (context_ && !paths_.isEmpty()) handler_(takeFileOpenRequests());
        });
    }

    QStringList paths_;
    QPointer<QObject> context_;
    std::function<void(const QStringList&)> handler_;
    bool scheduled_{false};
};
