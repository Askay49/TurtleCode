#include "TerminalSession.hpp"
#include "app/I18n.hpp"
#include <QSocketNotifier>
#include <QTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QDir>
#include <pwd.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <algorithm>

TerminalSession::TerminalSession(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::started, this, [this] { if(stopping_) {stopping_=false; stop(); return;} if (reader_) reader_->setEnabled(true); flushInput(); emit started(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { closePty(); emit error(process_.errorString()); }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int code, QProcess::ExitStatus) {
            // The child may exit before the socket notifier delivers its final output.
            readAvailable();
            closePty();
            emit finished(code);
        });
}
TerminalSession::~TerminalSession() {
    // Closing the window ends its terminal, including the shell's foreground job.
    if (isRunning()) {
        const auto pid = process_.processId();
        pid_t foreground = master_ >= 0 ? tcgetpgrp(master_) : -1;
        if (foreground > 0 && foreground != getpgrp()) kill(-foreground, SIGKILL);
        if (pid > 0) kill(-pid, SIGKILL);
        process_.kill();
        process_.waitForFinished(1000);
    }
    closePty();
}
void TerminalSession::start(const QString& directory, const QString& program, const QStringList& arguments, const QJsonObject& overrides) {
    if (isRunning()) return;
    closePty(); stopping_ = false;
    master_ = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    char slaveName[256]{};
    if (master_ < 0 || grantpt(master_) < 0 || unlockpt(master_) < 0 || ptsname_r(master_, slaveName, sizeof slaveName) != 0) {
        const QString message = QString::fromLocal8Bit(strerror(errno));
        closePty(); emit error(message); return;
    }
    resize(rows_, columns_);
    QString shell = program;
    if (shell.isEmpty()) {
        shell = qEnvironmentVariable("SHELL");
        if (shell.isEmpty()) if (const auto* entry = getpwuid(getuid())) shell = QFile::decodeName(entry->pw_shell);
        if (shell.isEmpty()) shell = QStringLiteral("/bin/sh");
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    for(auto it=overrides.begin();it!=overrides.end();++it) {
        if(it.value().isNull()) environment.remove(it.key()); else environment.insert(it.key(),it.value().toString());
    }
    environment.insert("TERM", "xterm-256color");
    environment.insert("COLORTERM", "truecolor");
    environment.remove("COLUMNS"); environment.remove("LINES");
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(directory.isEmpty() ? QDir::homePath() : directory);
    const QByteArray slave(slaveName);
    process_.setChildProcessModifier([slave] {
        // Only async-signal-safe operations are permitted between fork and exec.
        if (setsid() < 0) _exit(126);
        int fd = ::open(slave.constData(), O_RDWR | O_NOCTTY);
        if (fd < 0 || ioctl(fd, TIOCSCTTY, 0) < 0) _exit(126);
        for (int target = 0; target <= 2; ++target) if (dup2(fd, target) < 0) _exit(126);
        if (fd > 2) ::close(fd);
        // Qt may ignore SIGPIPE in the parent; shells need normal signal semantics.
        signal(SIGPIPE, SIG_DFL);
    });
    reader_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
    writer_ = new QSocketNotifier(master_, QSocketNotifier::Write, this);
    reader_->setEnabled(false);
    writer_->setEnabled(false);
    connect(reader_, &QSocketNotifier::activated, this, [this] { readAvailable(); });
    connect(writer_, &QSocketNotifier::activated, this, [this] { flushInput(); });
    process_.start(shell, program.isEmpty() ? QStringList{QStringLiteral("-i")} : arguments);
}
void TerminalSession::readAvailable() {
    if (master_ < 0) return;
    QByteArray result;
    char buffer[16384];
    // Bound each UI turn even for an unending producer such as `yes`.
    while (result.size() < 256 * 1024) {
        const auto count = ::read(master_, buffer, sizeof buffer);
        if (count > 0) result.append(buffer, count);
        else if (count < 0 && errno == EINTR) continue;
        else {
            if ((count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) && reader_) reader_->setEnabled(false);
            break;
        }
    }
    if (!result.isEmpty()) emit output(result);
}
void TerminalSession::write(const QByteArray& data) {
    if (master_ < 0 || stopping_ || !isRunning()) return;
    constexpr qsizetype maxPending = 4 * 1024 * 1024;
    if (data.size() > maxPending - (pending_.size() - pendingOffset_)) {
        emit error(I18n::text("Terminal input is too large. Paste a smaller amount of text.")); return;
    }
    if (pendingOffset_) { pending_.remove(0, pendingOffset_); pendingOffset_ = 0; }
    pending_.append(data); flushInput();
}
void TerminalSession::flushInput() {
    if (master_ < 0 || process_.state() != QProcess::Running) return;
    while (pendingOffset_ < pending_.size()) {
        const auto count = ::write(master_, pending_.constData() + pendingOffset_, pending_.size() - pendingOffset_);
        if (count > 0) pendingOffset_ += count;
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { pending_.clear(); pendingOffset_ = 0; break; }
    }
    if (pendingOffset_ == pending_.size()) { pending_.clear(); pendingOffset_ = 0; }
    if (writer_) writer_->setEnabled(!pending_.isEmpty());
}
void TerminalSession::resize(int rows, int columns) {
    rows_ = std::clamp(rows, 1, 500); columns_ = std::clamp(columns, 2, 1000);
    if (master_ < 0) return;
    winsize size{}; size.ws_row = rows_; size.ws_col = columns_;
    ioctl(master_, TIOCSWINSZ, &size);
}
void TerminalSession::stop() {
    if (!isRunning() || stopping_) return;
    stopping_ = true;
    if(process_.state()==QProcess::Starting) return;
    const auto pid = process_.processId();
    const pid_t foreground = master_ >= 0 ? tcgetpgrp(master_) : -1;
    if (foreground > 0 && foreground != getpgrp()) kill(-foreground, SIGHUP);
    if (pid > 0) kill(-pid, SIGHUP);
    QTimer::singleShot(1000, this, [this, pid, foreground] {
        // A foreground job can ignore HUP even after the shell itself has exited.
        if (pid > 0 && foreground > 0 && foreground != getpgrp() && getsid(foreground) == pid)
            kill(-foreground, SIGKILL);
        if (!isRunning() || process_.processId() != pid) return;
        if (pid > 0) kill(-pid, SIGKILL);
        process_.kill();
    });
}
void TerminalSession::closePty() {
    if (reader_) { reader_->setEnabled(false); reader_->deleteLater(); reader_ = nullptr; }
    if (writer_) { writer_->setEnabled(false); writer_->deleteLater(); writer_ = nullptr; }
    if (master_ >= 0) { ::close(master_); master_ = -1; }
    pending_.clear(); pendingOffset_ = 0;
}
