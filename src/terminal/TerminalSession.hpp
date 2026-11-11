#pragma once
#include <QObject>
#include <QByteArray>
#include <QProcess>
#include <QJsonObject>

class QSocketNotifier;
class TerminalSession final : public QObject {
    Q_OBJECT
public:
    explicit TerminalSession(QObject* parent = nullptr);
    ~TerminalSession() override;
    void start(const QString& directory, const QString& program = {}, const QStringList& arguments = {}, const QJsonObject& environment = {});
    void write(const QByteArray& data);
    void resize(int rows, int columns);
    void stop();
    bool isRunning() const { return process_.state() != QProcess::NotRunning; }
    QString program() const { return process_.program(); }
    qint64 processId() const { return process_.processId(); }
signals:
    void output(const QByteArray& data);
    void started();
    void finished(int exitCode);
    void error(const QString& message);
private:
    void readAvailable();
    void flushInput();
    void closePty();
    QProcess process_;
    int master_{-1};
    int rows_{24}, columns_{80};
    QSocketNotifier* reader_{nullptr};
    QSocketNotifier* writer_{nullptr};
    QByteArray pending_;
    qsizetype pendingOffset_{0};
    bool stopping_{false};
};
