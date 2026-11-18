#include "terminal/TerminalWidget.hpp"
#include "terminal/TerminalSession.hpp"
#include "app/ThemeManager.hpp"
#include "app/WorkspaceBackground.hpp"
#include <QApplication>
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSettings>
#include <QScrollBar>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QTimer>
#include <QImage>
#include <QPainter>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QStandardPaths>
#include <signal.h>
#include <unistd.h>

class TerminalTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QSettings().clear(); ThemeManager::instance().initialize(); }
    void stopDuringStartup() {
        TerminalSession session;
        session.start({},"/bin/sh",{"-c","sleep 60"}); session.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(),3000);
    }
    void emulation() {
        TerminalWidget terminal; terminal.resize(800, 320); terminal.show();
        terminal.feedOutput("first\r\nsecond\r\n\x1b[1;1H\x1b[31mHELLO\x1b[0m\x1b[K");
        QVERIFY(terminal.screenText().startsWith("HELLO\nsecond\n"));
        terminal.feedOutput("\x1b[?1049hALT SCREEN");
        QVERIFY(terminal.screenText().contains("ALT SCREEN"));
        QVERIFY(!terminal.screenText().contains("HELLO"));
        terminal.feedOutput("\x1b[?1049l");
        QVERIFY(terminal.screenText().startsWith("HELLO\nsecond\n"));
        terminal.reset();
        const QByteArray unicode = QString::fromUtf8("Привет 世界 é").toUtf8();
        for (const auto byte : unicode) terminal.feedOutput(QByteArray(1, byte));
        QVERIFY(terminal.screenText().startsWith(QString::fromUtf8("Привет 世界 é")));
        // Render actual cells, wide glyphs, cursor and ANSI attributes.
        QVERIFY(!terminal.grab().isNull());
    }
    void themedPalette() {
        auto& theme = ThemeManager::instance();
        TerminalWidget terminal; terminal.resize(800, 320); terminal.show();
        const int width = terminal.fontMetrics().horizontalAdvance('M');
        QByteArray colors;
        for (int i = 0; i < 16; ++i)
            colors += "\x1b[" + QByteArray::number(i < 8 ? 30 + i : 90 + i - 8) + "m\x1b[7m \x1b[0m";
        colors += "\x1b[48;5;196m \x1b[48;2;18;52;86m \x1b[0m\x1b[?25l";
        terminal.feedOutput(colors);
        // Keep a colored line in scrollback as well as on the live/alternate screen.
        for (int row = 0; row < terminal.rows(); ++row) terminal.feedOutput("\r\n");
        terminal.verticalScrollBar()->setValue(0);
        const auto original = terminal.screenText();
        for (const auto& id : {"cpp-dark", "one-dark", "github", "dracula", "winter", "monokai",
                               "night-owl", "ayu", "purple", "noctis", "tokyo-night"}) {
            terminal.hide();
            QSettings().setValue("appearance/theme", id); theme.initialize();
            terminal.show(); QApplication::processEvents();
            auto image = terminal.viewport()->grab().toImage();
            QCOMPARE(image.pixelColor(0, 0), theme.color("editor.background"));
            for (int i = 0; i < 16; ++i)
                QCOMPARE(image.pixelColor(5 + i * width, 5), theme.color(QString("terminal.ansi%1").arg(i)));
            QCOMPARE(image.pixelColor(5 + 16 * width, 5), QColor("#ff0000"));
            QCOMPARE(image.pixelColor(5 + 17 * width, 5), QColor("#123456"));
            QCOMPARE(terminal.screenText(), original);
            terminal.feedOutput("\x1b[?1049h\x1b[H" + colors);
            image = terminal.viewport()->grab().toImage();
            QCOMPARE(image.pixelColor(5 + width, 5), theme.color("terminal.ansi1"));
            terminal.feedOutput("\x1b[?1049l");
            terminal.verticalScrollBar()->setValue(0);
        }
        QSettings().remove("appearance/theme"); theme.initialize();
    }
    void sharedWallpaper() {
        QTemporaryDir directory;
        QImage wallpaper(800, 400, QImage::Format_RGB32);
        for (int y = 0; y < wallpaper.height(); ++y)
            for (int x = 0; x < wallpaper.width(); ++x)
                wallpaper.setPixelColor(x, y, QColor(x / 4, y / 2, 100));
        const auto path = directory.filePath("wallpaper.png");
        QVERIFY(wallpaper.save(path));
        QSettings().setValue("appearance/backgroundImage", path);
        QSettings().setValue("appearance/backgroundOpacity", 100);
        ThemeManager::instance().initialize();
        QWidget workspace; workspace.setObjectName("workspaceBackground"); workspace.resize(800, 400);
        TerminalWidget terminal(&workspace); terminal.setGeometry(40, 90, 650, 240);
        workspace.show(); QApplication::processEvents();
        terminal.feedOutput(" \x1b[41m \x1b[0;7m \x1b[0m\x1b[?25l");
        const int width = terminal.fontMetrics().horizontalAdvance('M');
        auto verify = [&] {
            const auto actual = terminal.viewport()->grab().toImage();
            QImage expected(terminal.viewport()->size(), QImage::Format_RGB32);
            QPainter painter(&expected);
            WorkspaceBackground::instance().paint(terminal.viewport(), painter, expected.rect(),
                                                  ThemeManager::instance().color("terminal.background"));
            painter.end();
            QCOMPARE(actual.pixelColor(5, 5), expected.pixelColor(5, 5));
            QCOMPARE(actual.pixelColor(5 + width, 5), ThemeManager::instance().color("terminal.ansi1"));
            QCOMPARE(actual.pixelColor(5 + 2 * width, 5), ThemeManager::instance().color("terminal.foreground"));
            QCOMPARE(actual.pixelColor(5 + 3 * width, 5), expected.pixelColor(5 + 3 * width, 5));
            const QPoint mapped = terminal.viewport()->mapTo(&workspace, QPoint(5, 5));
            if (workspace.size() == wallpaper.size())
                QCOMPARE(actual.pixelColor(5, 5), wallpaper.pixelColor(mapped));
        };
        verify();
        workspace.resize(900, 450); QApplication::processEvents(); verify();
        terminal.feedOutput("\x1b[?1049h\x1b[H \x1b[41m \x1b[0;7m \x1b[0m"); verify();
        terminal.feedOutput("\x1b[?1049l");
        for (int row = 0; row < terminal.rows(); ++row) terminal.feedOutput("\r\n");
        terminal.verticalScrollBar()->setValue(0); verify();
        QSettings().remove("appearance/backgroundImage");
        QSettings().remove("appearance/backgroundOpacity");
        ThemeManager::instance().initialize();
        QCOMPARE(terminal.viewport()->grab().toImage().pixelColor(5, 5),
                 ThemeManager::instance().color("terminal.background"));
    }
    void terminalThemeOverrides() {
        const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/themes";
        QVERIFY(QDir().mkpath(directory));
        QFile file(directory + "/terminal-test.json"); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"formatVersion", 1}, {"name", "Terminal test"},
            {"colors", QJsonObject{{"editor.background", "#fafafa"}, {"terminal.background", "#102030"},
                {"terminal.foreground", "#abcdef"}, {"terminal.cursor", "#ff00aa"},
                {"terminal.selectionBackground", "#214365"}, {"terminal.selectionForeground", "#fedcba"},
                {"terminal.ansi1", "#123987"}}}}).toJson()); file.close();
        TerminalWidget terminal; terminal.resize(800, 320); terminal.show(); terminal.setFocus();
        terminal.feedOutput("\x1b[41m \x1b[0m ");
        QSettings().setValue("appearance/theme", "custom-terminal-test"); ThemeManager::instance().initialize();
        QApplication::processEvents();
        auto image = terminal.viewport()->grab().toImage();
        const int width = terminal.fontMetrics().horizontalAdvance('M');
        QCOMPARE(image.pixelColor(0, 0), QColor("#102030"));
        QCOMPARE(image.pixelColor(5, 5), QColor("#123987"));
        QCOMPARE(image.pixelColor(5 + 2 * width, 5), QColor("#ff00aa"));
        terminal.feedOutput("\x1b[?25l");
        QTest::mousePress(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QTest::mouseRelease(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        image = terminal.viewport()->grab().toImage();
        QCOMPARE(image.pixelColor(5, 5), QColor("#214365"));
        // ANSI-only edits must update even when the application QPalette is unchanged.
        terminal.feedOutput("\x1b[1;1H\x1b[41m \x1b[0m");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto updated = file.readAll().replace("#123987", "#234a98"); file.close();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(updated); file.close();
        ThemeManager::instance().initialize();
        image = terminal.viewport()->grab().toImage();
        QCOMPARE(image.pixelColor(5, 5), QColor("#234a98"));
        QVERIFY(QFile::remove(file.fileName()));
        QSettings().remove("appearance/theme"); ThemeManager::instance().initialize();
        // Old custom themes continue to inherit their editor background/foreground.
        QCOMPARE(ThemeManager::instance().color("terminal.background"), ThemeManager::instance().color("editor.background"));
    }
    void keyboardAndPaste() {
        TerminalWidget terminal; terminal.show(); terminal.setFocus();
        QByteArray input;
        connect(&terminal, &TerminalWidget::input, this, [&](const QByteArray& bytes) { input += bytes; });
        QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier); QCOMPARE(input, QByteArray(1, '\x03'));
        input.clear(); QTest::keyClick(&terminal, Qt::Key_I, Qt::ControlModifier); QCOMPARE(input, QByteArray("\t"));
        input.clear(); QTest::keyClick(&terminal, Qt::Key_BracketLeft, Qt::ControlModifier); QCOMPARE(input, QByteArray("\x1b"));
        input.clear(); QTest::keyClick(&terminal, Qt::Key_Tab); QCOMPARE(input, QByteArray("\t"));
        input.clear(); QTest::keyClick(&terminal, Qt::Key_Up); QCOMPARE(input, QByteArray("\x1b[A"));
        terminal.feedOutput("\x1b[?1h"); input.clear();
        QTest::keyClick(&terminal, Qt::Key_Up); QCOMPARE(input, QByteArray("\x1bOA"));
        terminal.feedOutput("\x1b[?2004h"); input.clear();
        QApplication::clipboard()->setText(QString::fromUtf8("привет\nnext\x1b\x7f"));
        QTest::keyClick(&terminal, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(input, QByteArray("\x1b[200~") + QString::fromUtf8("привет\nnext").toUtf8() + "\x1b[201~");
        input.clear(); QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("日本語"));
        QApplication::sendEvent(&terminal, &ime); QCOMPARE(input, QString::fromUtf8("日本語").toUtf8());
    }
    void historySelectionAndFonts() {
        TerminalWidget terminal; terminal.resize(800, 320); terminal.show();
        terminal.feedOutput("hello world");
        QTest::mousePress(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        const int width = terminal.fontMetrics().horizontalAdvance('M');
        QTest::mouseMove(terminal.viewport(), QPoint(4 + 4 * width + 1, 5));
        QTest::mouseRelease(terminal.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(4 + 4 * width + 1, 5));
        QCOMPARE(terminal.selectedText(), QString("hello"));
        QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(QApplication::clipboard()->text(), QString("hello"));
        for (int i = 0; i < 11000; ++i) terminal.feedOutput("\r\nline");
        const int capacity = std::min(10000, static_cast<int>((16 * 1024 * 1024) / (terminal.columns() * sizeof(VTermScreenCell))));
        QCOMPARE(terminal.verticalScrollBar()->maximum(), capacity);
        QCOMPARE(terminal.verticalScrollBar()->value(), capacity);
        terminal.verticalScrollBar()->setValue(100);
        terminal.feedOutput("\r\nline");
        QCOMPARE(terminal.verticalScrollBar()->value(), 99);
        terminal.feedOutput("\x1b[3J"); QCOMPARE(terminal.verticalScrollBar()->maximum(), 0);
        const int oldRows = terminal.rows();
        QSettings().setValue("fonts/terminal/size", 26); terminal.applyFontSettings();
        QCOMPARE(terminal.font().pointSize(), 26); QVERIFY(terminal.rows() < oldRows);
        TerminalWidget fresh; QCOMPARE(fresh.font().pointSize(), 26);
        QSettings().remove("fonts/terminal/size");
    }
    void ptyDirectoryResizeAndInput() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        TerminalSession session; QByteArray output; QSignalSpy done(&session, &TerminalSession::finished);
        connect(&session, &TerminalSession::output, this, [&](const QByteArray& bytes) { output += bytes; });
        session.resize(31, 97);
        session.start(directory.path(), "/bin/sh", {"-c", "stty -echo; test -t 0 && test -t 1 && test -t 2 && printf 'PTY_OK\\n'; stty size; pwd; printf 'READY\\n'; read reply; printf '<%s>\\n' \"$reply\""});
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY"), 5000);
        QVERIFY(output.contains("PTY_OK\r\n")); QVERIFY(output.contains("31 97\r\n"));
        QVERIFY(output.contains(directory.path().toUtf8()));
        session.write("payload\n"); QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 5000);
        QVERIFY(output.contains("<payload>")); QCOMPARE(done[0][0].toInt(), 0);
        // Failure is visible and a new session can be started afterwards.
        QSignalSpy errors(&session, &TerminalSession::error);
        session.start(directory.path(), "/definitely/missing/blink-shell");
        QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 5000);
        output.clear(); session.start(directory.path(), "/bin/sh", {"-c", "printf restarted"});
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("restarted"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(), 5000);
    }
    void interruptAndLiveResize() {
        TerminalWidget terminal; terminal.resize(800, 320); terminal.show();
        TerminalSession session; QByteArray output;
        connect(&session, &TerminalSession::output, &terminal, &TerminalWidget::feedOutput);
        connect(&session, &TerminalSession::output, this, [&](const QByteArray& bytes) { output += bytes; });
        connect(&terminal, &TerminalWidget::input, &session, &TerminalSession::write);
        connect(&terminal, &TerminalWidget::terminalSizeChanged, &session, &TerminalSession::resize);
        session.start("/tmp", "/bin/sh", {"-i"});
        QTRY_VERIFY_WITH_TIMEOUT(session.processId() > 0, 5000);
        session.write("stty -echo; printf '\\122\\105\\101\\104\\131\\n'\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("READY\r\n"), 5000);
        output.clear(); session.write("sleep 30\n"); QTest::qWait(100);
        QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier);
        session.write("printf '\\101\\106\\124\\105\\122\\n'\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains("AFTER\r\n"), 5000);
        terminal.resize(640, 240); output.clear(); session.write("stty size\n");
        QTRY_VERIFY_WITH_TIMEOUT(output.contains(QByteArray::number(terminal.rows()) + " " + QByteArray::number(terminal.columns())), 5000);
        session.write("exit\n"); QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(), 5000);
    }
    void vimRoundTrip() {
        const auto vim = QStandardPaths::findExecutable("vim");
        if (vim.isEmpty()) QSKIP("vim is not installed");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("unicode file.txt");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("VIM_READY\n"); }
        TerminalWidget terminal; terminal.resize(900, 400); terminal.show();
        TerminalSession session;
        connect(&session, &TerminalSession::output, &terminal, &TerminalWidget::feedOutput);
        connect(&terminal, &TerminalWidget::input, &session, &TerminalSession::write);
        session.resize(terminal.rows(), terminal.columns());
        session.start(directory.path(), vim, {"-u", "NONE", "-i", "NONE", "-n", "-N", "--noplugin", "--cmd", "set showmode", "--cmd", "language messages C", path});
        QTRY_VERIFY_WITH_TIMEOUT(terminal.screenText().contains("VIM_READY"), 5000);
        session.write("ggO");
        // Confirm that the full-screen application has actually entered Insert mode.
        QTRY_VERIFY_WITH_TIMEOUT(terminal.screenText().contains("INSERT"), 5000);
        QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("Привет из терминала"));
        QApplication::sendEvent(&terminal, &ime);
        QTRY_VERIFY_WITH_TIMEOUT(terminal.screenText().contains(QString::fromUtf8("Привет из терминала")), 5000);
        QTest::keyClick(&terminal, Qt::Key_Escape); session.write(":wq\r");
        QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(), 5000);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QString::fromUtf8("Привет из терминала\nVIM_READY\n").toUtf8());
    }
    void floodAndShutdown() {
        TerminalSession session; int turns = 0; qsizetype total = 0;
        QTimer timer; timer.setInterval(1); connect(&timer, &QTimer::timeout, this, [&] { ++turns; }); timer.start();
        connect(&session, &TerminalSession::output, this, [&](const QByteArray& bytes) { total += bytes.size(); });
        session.start("/tmp", "/bin/sh", {"-c", "exec yes blink-terminal"});
        QTRY_VERIFY_WITH_TIMEOUT(total > 512 * 1024 && turns > 2, 5000);
        session.stop(); QTRY_VERIFY_WITH_TIMEOUT(!session.isRunning(), 5000);
        // Destruction must reap the child without leaving a running terminal job.
        qint64 pid = 0;
        { TerminalSession child; child.start("/tmp", "/bin/sh", {"-c", "exec sleep 30"});
          QTRY_VERIFY_WITH_TIMEOUT(child.processId() > 0, 5000); pid = child.processId(); }
        QVERIFY(kill(pid, 0) == -1 && errno == ESRCH);
    }
};
int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    QApplication app(argc, argv); app.setOrganizationName("TurtleCodeTests"); app.setApplicationName("TerminalTests");
    TerminalTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "TerminalTests.moc"
