#pragma once
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonObject>
#include <QHash>
#include <QTimer>
#include <functional>

// One adapter process per session. No UI or language-specific launch policy here.
class DapClient final : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonObject&)>;
    explicit DapClient(QObject* parent=nullptr);
    ~DapClient() override;
    void start(const QString& command, const QStringList& arguments, const QString& workingDirectory = {}, const QProcessEnvironment& environment = QProcessEnvironment::systemEnvironment());
    int request(const QString& command, const QJsonObject& arguments={}, Reply reply={});
    void respond(const QJsonObject& request,bool success,const QJsonObject& body={},const QString& message={});
    void stop();
    bool active() const { return process_.state()!=QProcess::NotRunning; }
signals:
    void reverseRequest(const QJsonObject& request);
    void ready();
    void eventReceived(const QString& event, const QJsonObject& body);
    void fatalError(const QString& message);
    void error(const QString& message);
    void finished();
private:
    void read();
    void fail(const QString& message);
    QProcess process_;
    QByteArray input_;
    int sequence_{0};
    quint64 generation_{0};
    struct Pending { Reply reply; qint64 deadline; QString command; };
    QHash<int,Pending> pending_;
    QTimer timer_;
};
