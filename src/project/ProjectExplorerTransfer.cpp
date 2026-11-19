#include "ProjectExplorer.hpp"

#include "app/Codicons.hpp"
#include "app/I18n.hpp"
#include "app/IconDialogs.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <cerrno>
#include <filesystem>
#include <ranges>
#include <utility>
#include <vector>

#if defined(Q_OS_LINUX)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <stdio.h>
#endif

namespace {
namespace fs = std::filesystem;

// Only the owning process can move its cut item and update open documents.
class CutFileData final : public QMimeData {
public:
    const QUuid id = QUuid::createUuid();
};

bool entryExists(const QFileInfo& info) {
    return info.exists() || info.isSymLink();
}

bool directoryIsWithin(const QFileInfo& source, const QString& directory) {
    if (source.isSymLink() || !source.isDir()) return false;
    const QString parent = source.canonicalFilePath();
    const QString target = QFileInfo(directory).canonicalFilePath();
    if (parent.isEmpty() || target.isEmpty()) return false;
    const QString relative = QDir(parent).relativeFilePath(target);
    return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}

QString availableCopyPath(const QFileInfo& source, const QDir& directory) {
    const QString name = source.fileName();
    const qsizetype dot = source.isDir() ? -1 : name.indexOf('.', 1);
    const QString base = dot < 0 ? name : name.left(dot);
    const QString suffix = dot < 0 ? QString{} : name.mid(dot);
    QString path = directory.filePath(I18n::text("%1 copy%2").arg(base, suffix));
    for (int index = 2; entryExists(QFileInfo(path)); ++index) {
        path = directory.filePath(I18n::text("%1 copy (%2)%3").arg(base, QString::number(index), suffix));
    }
    return path;
}

QString systemError(const std::error_code& error) {
    return QString::fromLocal8Bit(error.message());
}

std::error_code publishPath(const fs::path& source, const fs::path& destination) {
    // The destination can appear after the user chooses a name, even during a copy.
#if defined(Q_OS_LINUX)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
        return {};
#elif defined(Q_OS_MACOS)
    if (renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) return {};
#else
    return std::make_error_code(std::errc::operation_not_supported);
#endif
    return {errno, std::generic_category()};
}

fs::path nativePath(const QString& path) {
    return QFile::encodeName(path).toStdString();
}

QString copyToPath(const QString& source, const QString& directory, const QString& destination) {
    // Stage under the destination directory so publication is atomic on its filesystem.
    if (directoryIsWithin(QFileInfo(source), directory))
        return I18n::text("A folder cannot be copied or moved into itself.");
    QTemporaryDir staging(QDir(directory).filePath(".turtle-copy-XXXXXX"));
    if (!staging.isValid()) return staging.errorString();
    const auto from = nativePath(source);
    const auto staged = nativePath(QDir(staging.path()).filePath("item"));
    std::vector<std::pair<fs::path, fs::perms>> directories;
    QString failure;
    try {
        const auto createDirectory = [&directories](const fs::path& path, fs::file_status status) {
            fs::create_directory(path);
            directories.emplace_back(path, status.permissions() & fs::perms::all);
        };
        const auto sourceStatus = fs::symlink_status(from);
        if (fs::is_directory(sourceStatus)) {
            createDirectory(staged, sourceStatus);
            for (const auto& entry : fs::recursive_directory_iterator(from)) {
                const auto target = staged / entry.path().lexically_relative(from);
                const auto status = entry.symlink_status();
                if (fs::is_directory(status))
                    createDirectory(target, status);
                else
                    fs::copy(entry.path(), target, fs::copy_options::copy_symlinks);
            }
        } else {
            fs::copy(from, staged, fs::copy_options::copy_symlinks);
        }
        // Apply permissions from the bottom up after the contents are in place.
        for (const auto& [path, permissions] : std::views::reverse(directories))
            fs::permissions(path, permissions);
        const auto error = publishPath(staged, nativePath(destination));
        if (!error) return {};
        failure = systemError(error);
    } catch (const fs::filesystem_error& error) {
        failure = systemError(error.code());
    }
    // Allow the temporary directory to clean up even if the source was read-only.
    for (const auto& [path, permissions] : directories) {
        std::error_code ignored;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ignored);
    }
    return failure;
}
}

void ProjectExplorer::setPathBusyCheck(std::function<bool(const QString&)> check) {
    pathBusyCheck_ = std::move(check);
}

bool ProjectExplorer::canCutOrDuplicate(const QString& path) const {
    return !path.isEmpty() && !roots_.contains(path) && entryExists(QFileInfo(path));
}

QString ProjectExplorer::clipboardPath() {
    const auto* data = QApplication::clipboard()->mimeData();
    if (!data || !data->hasUrls()) return {};
    const auto urls = data->urls();
    if (urls.size() != 1 || !urls.front().isLocalFile()) return {};
    const QString path = urls.front().toLocalFile();
    return QDir::isAbsolutePath(path) ? QDir::cleanPath(path) : QString{};
}

void ProjectExplorer::copyPathToClipboard(const QString& path, bool cut) {
    if (path.isEmpty() || !entryExists(QFileInfo(path)) || (cut && !canCutOrDuplicate(path))) return;
    QMimeData* data = cut ? new CutFileData : new QMimeData;
    data->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(data);
}

void ProjectExplorer::pasteInto(const QString& directory) {
    const QString source = clipboardPath();
    if (source.isEmpty() || directory.isEmpty()) return;
    auto* clipboard = QApplication::clipboard();
    const auto* data = dynamic_cast<const CutFileData*>(clipboard->mimeData());
    const QUuid cut = data ? data->id : QUuid{};
    if (transferPath(source, directory, !cut.isNull()) && !cut.isNull()) {
        const auto* current = dynamic_cast<const CutFileData*>(clipboard->mimeData());
        if (current && current->id == cut) clipboard->clear();
    }
}

void ProjectExplorer::duplicatePath(const QString& path) {
    if (canCutOrDuplicate(path))
        transferPath(path, QFileInfo(path).absolutePath(), false, true);
}

bool ProjectExplorer::transferPath(const QString& source, const QString& directory, bool move, bool duplicate) {
    if (copyingPath_ || (move && roots_.contains(source))) return false;
    const QString title = I18n::text(move ? "Move failed" : "Copy failed");
    const QFileInfo info(source);
    if (!entryExists(info)) {
        IconDialogs::MessageBox::warning(this, title, I18n::text("The selected item no longer exists."));
        return false;
    }
    if (!QFileInfo(directory).isDir()) {
        IconDialogs::MessageBox::warning(this, title, I18n::text("The destination folder no longer exists."));
        return false;
    }
    if (directoryIsWithin(info, directory)) {
        IconDialogs::MessageBox::warning(this, title, I18n::text("A folder cannot be copied or moved into itself."));
        return false;
    }

    const QDir target(directory);
    QString destination = target.filePath(info.fileName());
    if (move && destination == source) return false;
    if (duplicate) {
        destination = availableCopyPath(info, target);
    } else if (entryExists(QFileInfo(destination))) {
        IconDialogs::MessageBox dialog(QMessageBox::Question, I18n::text("Name conflict"),
            I18n::text("An item named “%1” already exists in this folder.").arg(info.fileName()),
            QMessageBox::Cancel, this);
        dialog.setTextFormat(Qt::PlainText);
        auto* keepBoth = dialog.addButton(I18n::text("Keep Both"), QMessageBox::AcceptRole);
        keepBoth->setIcon(Codicons::icon("files"));
        dialog.setDefaultButton(QMessageBox::Cancel);
        dialog.exec();
        if (dialog.clickedButton() != keepBoth) return false;
        destination = availableCopyPath(info, target);
    }
    if (move) return movePath(source, destination);

    copyingPath_ = true;
    viewport()->setCursor(Qt::BusyCursor);
    auto* worker = new QFutureWatcher<QString>(this);
    connect(worker, &QFutureWatcher<QString>::finished, this, [this, worker, source, directory, destination] {
        const QString error = worker->result();
        worker->deleteLater();
        copyingPath_ = false;
        viewport()->unsetCursor();
        if (!error.isEmpty()) {
            emit backgroundError(I18n::text("Could not copy %1 to %2. %3").arg(source, destination, error));
            return;
        }
        refreshDirectory(directory);
        emit filesChanged();
    });
    worker->setFuture(QtConcurrent::run([source, directory, destination] {
        return copyToPath(source, directory, destination);
    }));
    return true;
}

bool ProjectExplorer::movePath(const QString& source, const QString& destination) {
    if (pathBusyCheck_ && pathBusyCheck_(source)) {
        emit backgroundError(I18n::text("Wait for file loading/saving to finish and retry."));
        return false;
    }
    if (const auto error = publishPath(nativePath(source), nativePath(destination))) {
        const QString reason = error == std::errc::cross_device_link
            ? I18n::text("The source and destination are on different filesystems.") : systemError(error);
        emit backgroundError(I18n::text("Could not move %1 to %2. %3").arg(source, destination, reason));
        return false;
    }
    emit pathRenamed(source, destination);
    refreshDirectory(QFileInfo(source).absolutePath());
    refreshDirectory(QFileInfo(destination).absolutePath());
    emit filesChanged();
    return true;
}
