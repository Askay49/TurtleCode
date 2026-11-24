#include <QTimer>
#include "app/I18n.hpp"
#include "app/AppIdentity.hpp"
#include "app/FileOpenApplication.hpp"
#include "app/Version.hpp"
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

#ifdef TURTLE_CODE_WITH_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <memory>
#endif

#include "app/MainWindow.hpp"
#include "app/ThemeManager.hpp"

namespace {
QFont interfaceFont() {
    QFont font(QStringLiteral("monospace"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(11);
#ifdef TURTLE_CODE_WITH_FONTCONFIG
    // Reuse the configured family so Qt can match it directly on first layout.
    using Pattern = std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)>;
    const Pattern query(FcPatternCreate(), &FcPatternDestroy);
    if (!query || !FcPatternAddString(query.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>("monospace"))
        || !FcPatternAddDouble(query.get(), FC_SIZE, font.pointSizeF())
        || !FcConfigSubstitute(nullptr, query.get(), FcMatchPattern))
        return font;
    FcDefaultSubstitute(query.get());
    FcResult result{};
    const Pattern match(FcFontMatch(nullptr, query.get(), &result), &FcPatternDestroy);
    FcChar8* family = nullptr;
    if (match && FcPatternGetString(match.get(), FC_FAMILY, 0, &family) == FcResultMatch && family && *family)
        font.setFamily(QString::fromUtf8(reinterpret_cast<const char*>(family)));
#endif
    return font;
}

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
    FileOpenApplication app(argc, argv);
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
    QCoreApplication::setApplicationVersion(QString::fromLatin1(AppVersion::Display));
    I18n::initialize();

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

    const QStringList paths = normalizedArguments(parser.positionalArguments() + app.takeFileOpenRequests());
    const bool newWindow = parser.isSet(newWindowOption);
    const QString primaryName = primaryServerName();

    if (!newWindow && forwardToRunningInstance(primaryName, paths))
        return 0;

    ThemeManager::instance().initialize(ThemeManager::BackgroundLoad::Asynchronous);
    app.setFont(interfaceFont());

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
    const auto openFiles = [&window](const QStringList& paths) {
        window.openPaths(paths);
        window.raise();
        window.activateWindow();
    };
    window.resize(1280, 800);
    window.openStartupPaths(paths + app.takeFileOpenRequests());
    window.showMaximized();
    app.setFileOpenHandler(&window, openFiles);
    if (!newWindow) QTimer::singleShot(0, &window, &MainWindow::restoreUnsavedDocuments);

    QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window, openFiles] {
        while (QLocalSocket* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, openFiles] {
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
                openFiles(paths);
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    return app.exec();
}
