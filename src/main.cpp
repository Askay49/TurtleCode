#include <QTimer>
#include "app/I18n.hpp"
#include "app/AppIdentity.hpp"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFont>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <unistd.h>

#include "app/MainWindow.hpp"
#include "app/ThemeManager.hpp"

namespace {
QString primaryServerName() {
    return QStringLiteral("turtle-code-%1").arg(static_cast<qulonglong>(::getuid()));
}

QStringList normalizedArguments(const QStringList& arguments) {
    QStringList paths;
    for (const QString& argument : arguments)
        paths.push_back(QDir::cleanPath(QFileInfo(argument).absoluteFilePath()));
    return paths;
}

bool forwardToRunningInstance(const QString& serverName, const QStringList& paths) {
    QLocalSocket socket;
    socket.connectToServer(serverName, QIODevice::WriteOnly);
    if (!socket.waitForConnected(250))
        return false;
    QJsonArray array;
    for (const QString& path : paths) array.append(path);
    const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("paths"), array}}).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(payload) == -1 || !socket.waitForBytesWritten(1000)) {
        socket.disconnectFromServer();
        return false;
    }
    socket.disconnectFromServer();
    return true;
}
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    AppIdentity::setCurrent();
    // Do not move recovery journals while a previous-version process is writing them.
    QLocalSocket previousInstance;
    previousInstance.connectToServer(QStringLiteral("blink-code-%1").arg(static_cast<qulonglong>(::getuid())));
    if (previousInstance.waitForConnected(100)) {
        qCritical("Close the previous editor instance before starting Turtle Code.");
        return 1;
    }
    const auto migrationError = AppIdentity::migrate();
    if (!migrationError.isEmpty()) {
        qCritical().noquote() << migrationError;
        return 1;
    }
    QApplication::setApplicationDisplayName("Turtle Code");
    QGuiApplication::setDesktopFileName(QStringLiteral("turtle-code"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app/turtle-code.png")));
    QCoreApplication::setApplicationVersion("0.9.1");
    I18n::initialize();
    ThemeManager::instance().initialize();

    QCommandLineParser parser;
    parser.setApplicationDescription(I18n::text("Turtle Code — native Linux code editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption newWindowOption({QStringLiteral("n"), QStringLiteral("new-window")},
                                              I18n::text("Open a new application window."));
    const QCommandLineOption reuseWindowOption({QStringLiteral("r"), QStringLiteral("reuse-window")},
                                                I18n::text("Open paths in the existing application window."));
    parser.addOption(newWindowOption);
    parser.addOption(reuseWindowOption);
    parser.addPositionalArgument(QStringLiteral("path"),
        I18n::text("Files or a project folder to open. Only the first folder is used."),
        QStringLiteral("[path...]") );
    parser.process(app);

    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(11);
    app.setFont(font);

    const QStringList paths = normalizedArguments(parser.positionalArguments());
    const bool newWindow = parser.isSet(newWindowOption);
    const QString primaryName = primaryServerName();

    if (!newWindow && forwardToRunningInstance(primaryName, paths))
        return 0;

    QString serverName = primaryName;
    if (newWindow) {
        QLocalSocket primaryProbe;
        primaryProbe.connectToServer(primaryName, QIODevice::WriteOnly);
        if (primaryProbe.waitForConnected(200)) {
            primaryProbe.disconnectFromServer();
            serverName = primaryName + QStringLiteral("-window-%1").arg(QCoreApplication::applicationPid());
        }
    }
    QLocalServer server;
    if (!server.listen(serverName)) {
        QLocalServer::removeServer(serverName);
        if (!server.listen(serverName))
            qWarning() << I18n::text("Could not create local IPC server:") << server.errorString();
    }

    MainWindow window;
    window.resize(1280, 800);
    if (!paths.isEmpty()) window.openPaths(paths);
    window.show();
    if(!newWindow) QTimer::singleShot(0,&window,[&window,paths]{window.restorePreviousSession(paths.isEmpty());});

    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window] {
        while (QLocalSocket* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, &window] {
                QByteArray payload = socket->property("pendingPayload").toByteArray();
                payload += socket->readAll();
                socket->setProperty("pendingPayload", payload);
                if (!payload.endsWith('\n')) return;
                const QJsonDocument document = QJsonDocument::fromJson(payload.trimmed());
                if (!document.isObject()) return;
                const QJsonArray array = document.object().value(QStringLiteral("paths")).toArray();
                QStringList paths;
                for (const QJsonValue& value : array)
                    if (value.isString()) paths.push_back(value.toString());
                window.openPaths(paths);
                window.raise();
                window.activateWindow();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    return app.exec();
}
