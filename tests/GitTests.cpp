#include "git/GitService.hpp"
#include "git/GitPanel.hpp"
#include "git/GitDiffView.hpp"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QCheckBox>
#include <QTextBlock>
#include <QPushButton>
#include <QScrollBar>
#include <QLayout>
#include <QComboBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

namespace {
QByteArray git(const QString& root, const QStringList& args, int expected = 0) {
    QProcess process; process.setWorkingDirectory(root);
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("GIT_CONFIG_NOSYSTEM", "1"); env.insert("GIT_CONFIG_GLOBAL", "/dev/null"); env.insert("LC_ALL", "C");
    process.setProcessEnvironment(env); process.start("git", args);
    if (!process.waitForFinished(10000) || process.exitCode() != expected)
        qFatal("Git fixture failed: %s", process.readAllStandardError().constData());
    return process.readAllStandardOutput();
}
void write(const QString& root, const QString& path, const QByteArray& data) {
    QFile file(root + QLatin1Char('/') + path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) qFatal("Cannot write fixture");
}
void init(const QString& root) {
    git(root, {"init", "-b", "main"});
    git(root, {"config", "user.name", "Turtle Code Test"});
    git(root, {"config", "user.email", "test@example.invalid"});
    git(root, {"config", "commit.gpgsign", "false"});
}
}
class GitTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (QStandardPaths::findExecutable("git").isEmpty()) QSKIP("Git is not installed");
        qputenv("GIT_CONFIG_NOSYSTEM", "1"); qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
        qRegisterMetaType<GitDiffSnapshot>();
    }
    void diffModel() {
        const auto m=GitDiff::compare("one\r\ntwo\r\nthree", "one\r\nTWO\r\nextra\r\nthree");
        QCOMPARE(m.hunks.size(),1); QCOMPARE(m.hunks[0].leftCount,1); QCOMPARE(m.hunks[0].rightCount,2);
        QCOMPARE(GitDiff::selected(m,m.hunks[0].row,m.hunks[0].row),QByteArray("one\r\nTWO\r\nthree"));
        QCOMPARE(GitDiff::selected(m,m.hunks[0].row,m.hunks[0].row,true),QByteArray("one\r\ntwo\r\nextra\r\nthree"));
        QCOMPARE(GitDiff::edited("a\r\nb\nc", "a\nb\nc", "a\nB\nc"),QByteArray("a\r\nB\nc"));
        QCOMPARE(GitDiff::edited("привет\n",QString::fromUtf8("привет\n"),QString::fromUtf8("привёт\n")),QByteArray("привёт\n"));
        QVERIFY(!GitDiff::textual(QByteArray("a\0b",3)));
        QVERIFY(!GitDiff::textual(QByteArray(1,char(0xff))));
        const auto eof=GitDiff::compare("a\n","a"); QCOMPARE(eof.hunks.size(),1);
        QVERIFY(GitDiff::patch("file", "a\n", "a").contains("No newline at end of file"));
    }
    void visibleLineEndings() {
        const QByteArray before="value = 1;\r\nnext\r\n",after="value = 1;\nnext\r\n";
        QVERIFY(!GitDiff::compare(before,after).hunks.isEmpty());
        const auto visible=GitDiff::compare(before,after,true);
        QVERIFY(visible.hunks.isEmpty()); QCOMPARE(GitDiff::join(visible.left),before); QCOMPARE(GitDiff::join(visible.right),after);
        QVERIFY(!GitDiff::compare("x\n","x",true).hunks.isEmpty());
        QVERIFY(!GitDiff::compare("x \r\n","x\n",true).hunks.isEmpty());
        GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="eol.c"; snapshot.left=before; snapshot.right=after;
        QByteArray bytes=after; int changes=-1;
        QPlainTextEdit editor; editor.show();
        GitDiffView::LiveDocument live; live.editor=&editor; live.read=[&] { return bytes; }; live.available=[] { return true; };
        live.highlight=[&](const GitDiff::Model* model) { changes=model?model->hunks.size():-1; };
        GitDiffView view; view.displayVersions(snapshot,live); QTRY_COMPARE(changes,0);
        auto* old=view.findChild<QTextEdit*>("gitDiffOld"); QVERIFY(old->extraSelections().isEmpty());
        bytes="value = 2;\nnext\r\n"; view.documentChanged(); QTRY_COMPARE(changes,1); QVERIFY(!old->extraSelections().isEmpty());
        bytes=after; view.documentChanged(); QTRY_COMPARE(changes,0); QVERIFY(old->extraSelections().isEmpty());
        QVERIFY(view.findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().isEmpty());
    }
    void separatedLargeDiff() {
        QByteArray before,after;
        for(int i=0;i<6500;++i) {
            const auto line=QByteArray::number(i)+" original\r\n"; before+=line;
            after+=(i==50 || i==3000 || i==6400)?QByteArray::number(i)+" changed\r\n":line;
        }
        const auto model=GitDiff::compare(before,after);
        QVERIFY(!model.coarse); QCOMPARE(model.hunks.size(),3);
        QCOMPARE(GitDiff::selected(model,0,model.rows.size()-1),after);
        QCOMPARE(GitDiff::selected(model,0,model.rows.size()-1,true),before);
        const auto first=model.hunks.first();
        const auto partial=GitDiff::selected(model,first.row,first.row+first.rows-1);
        QVERIFY(partial.contains("50 changed")); QVERIFY(partial.contains("3000 original"));
    }
    void diffByteBoundaries() {
        const QList<QByteArray> values={"", "a", "a\n", "a\r\n", "b\na", "a\nb\n", "b\na\n", QByteArray("a\0b\n",4)};
        for(const auto& before:values) for(const auto& after:values) {
            const auto model=GitDiff::compare(before,after);
            QCOMPARE(GitDiff::selected(model,0,model.rows.size()-1),after);
            QCOMPARE(GitDiff::selected(model,0,model.rows.size()-1,true),before);
            for(const auto& row:model.rows) if(!row.changed) QCOMPARE(model.left[row.left],model.right[row.right]);
        }
    }
    void readsWithoutGitExecutable() {
        QTemporaryDir dir; init(dir.path());
        write(dir.path(),"a.txt","head\n"); git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        write(dir.path(),"a.txt","index\n"); git(dir.path(),{"add","."}); write(dir.path(),"a.txt","working\n");
        git(dir.path(),{"branch",QString::fromUtf8("feature/пример")});
        // A missing Git executable must not affect any read path. Restore PATH
        // with RAII even if a test assertion returns early.
        struct PathGuard { QByteArray path=qgetenv("PATH"); ~PathGuard() { qputenv("PATH",path); } } guard;
        qputenv("PATH","/nonexistent-turtle-code-test");
        GitService service; QSignalSpy errors(&service,&GitService::error),versions(&service,&GitService::versionsReady);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QVERIFY(service.status().valid); QVERIFY(service.status().branches.contains(QString::fromUtf8("feature/пример")));
        QCOMPARE(service.status().entries.size(),1); const auto entry=service.status().entries[0];
        QVERIFY(entry.staged() && entry.unstaged());
        service.diff(entry,true); QTRY_VERIFY(!service.busy()); QCOMPARE(versions.size(),1);
        auto snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]); QCOMPARE(snapshot.left,QByteArray("head\n")); QCOMPARE(snapshot.right,QByteArray("index\n"));
        service.diff(entry,false); QTRY_VERIFY(!service.busy()); QCOMPARE(versions.size(),2);
        snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]); QCOMPARE(snapshot.left,QByteArray("index\n")); QCOMPARE(snapshot.right,QByteArray("working\n"));
        service.refresh(); QTRY_VERIFY(!service.busy()); QCOMPARE(errors.size(),0);
    }
    void unchangedDocumentDoesNotRead() {
        GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="idle.cpp"; snapshot.left="old\n"; snapshot.right="new\n";
        QByteArray bytes=snapshot.right,painted; int reads=0; bool ready=true;
        QPlainTextEdit editor; editor.show();
        GitDiffView::LiveDocument live; live.editor=&editor;
        live.read=[&] { ++reads; return bytes; }; live.available=[&] { return ready; };
        live.highlight=[&](const GitDiff::Model* model) { if(model) painted=GitDiff::join(model->right); };
        GitDiffView view; view.displayVersions(snapshot,live); QTRY_COMPARE(painted,bytes);
        const int initial=reads; QTest::qWait(650); QCOMPARE(reads,initial);
        bytes="edited\n"; view.documentChanged(); QTRY_COMPARE(painted,bytes); QCOMPARE(reads,initial+1);
        ready=false; view.setOperationBusy(true); bytes="loaded\n"; ready=true; view.setOperationBusy(false); QTRY_COMPARE(painted,bytes);
        QCOMPARE(reads,initial+2);
    }
    void conflictStages() {
        QTemporaryDir dir; init(dir.path());
        write(dir.path(),"base","base\n"); write(dir.path(),"ours","ours\n"); write(dir.path(),"theirs","theirs\n");
        git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        const auto base=git(dir.path(),{"rev-parse","HEAD:base"}).trimmed();
        const auto ours=git(dir.path(),{"rev-parse","HEAD:ours"}).trimmed();
        const auto theirs=git(dir.path(),{"rev-parse","HEAD:theirs"}).trimmed();
        const QStringList states={"DD","AU","UD","UA","DU","AA","UU"};
        for(const auto& state:states) {
            const bool b=state=="DD" || state=="UD" || state=="DU" || state=="UU";
            const bool o=state=="AU" || state=="UD" || state=="AA" || state=="UU";
            const bool t=state=="UA" || state=="DU" || state=="AA" || state=="UU";
            QByteArray input="0 "+QByteArray(40,'0')+"\tconflict\n";
            if(b) input+="100644 "+base+" 1\tconflict\n";
            if(o) input+="100644 "+ours+" 2\tconflict\n";
            if(t) input+="100644 "+theirs+" 3\tconflict\n";
            QProcess process; process.setWorkingDirectory(dir.path()); process.start("git",{"update-index","--index-info"});
            QVERIFY(process.waitForStarted()); process.write(input); process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(),0);
            write(dir.path(),"conflict","working conflict\n");
            GitService service; QSignalSpy errors(&service,&GitService::error),versions(&service,&GitService::versionsReady);
            service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
            QCOMPARE(service.status().entries.size(),1); const auto entry=service.status().entries[0];
            QCOMPARE(QByteArray(1,entry.index)+entry.worktree,state.toLatin1()); QVERIFY(entry.conflict());
            service.diff(entry,false); QTRY_VERIFY(!service.busy()); QCOMPARE(errors.size(),0); QCOMPARE(versions.size(),1);
            const auto snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]);
            QCOMPARE(snapshot.base,b?QByteArray("base\n"):QByteArray{});
            QCOMPARE(snapshot.ours,o?QByteArray("ours\n"):QByteArray{});
            QCOMPARE(snapshot.theirs,t?QByteArray("theirs\n"):QByteArray{});
            QCOMPARE(snapshot.left,snapshot.ours); QCOMPARE(snapshot.right,QByteArray("working conflict\n"));
        }
    }
    void specialVersions() {
        QTemporaryDir dir; init(dir.path());
        write(dir.path(),"binary",QByteArray("a\0b",3));
        write(dir.path(),"large",QByteArray(8*1024*1024+1,'x'));
        QVERIFY(QFile::link(dir.filePath("binary"),dir.filePath("link")));
        git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        write(dir.path(),"binary",QByteArray("b\0a",3)); write(dir.path(),"large",QByteArray(8*1024*1024+2,'y'));
        QVERIFY(QFile::remove(dir.filePath("link"))); QVERIFY(QFile::link(dir.filePath("large"),dir.filePath("link")));
        GitService service; QSignalSpy errors(&service,&GitService::error),versions(&service,&GitService::versionsReady);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy()); QCOMPARE(service.status().entries.size(),3);
        for(const auto& entry:service.status().entries) {
            service.diff(entry,false); QTRY_VERIFY(!service.busy()); QVERIFY(!versions.isEmpty());
            const auto snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]);
            if(entry.path=="binary") { QCOMPARE(snapshot.left,QByteArray("a\0b",3)); QVERIFY(snapshot.patch.contains("Binary")); }
            else { QVERIFY(snapshot.special); QVERIFY(snapshot.right.isEmpty()); QVERIFY(!snapshot.patch.isEmpty()); }
        }
        QCOMPARE(errors.size(),0);
    }
    void versionsAndSelections() {
        qRegisterMetaType<GitDiffSnapshot>();
        QTemporaryDir dir; init(dir.path()); const QString path=QString::fromUtf8("файл [x]\n.txt");
        write(dir.path(),path,"a\nb\nc\n"); git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        write(dir.path(),path,"A\nb\nC\n");
        GitService service; QSignalSpy versions(&service,&GitService::versionsReady), errors(&service,&GitService::error);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        auto entry=service.status().entries[0]; service.diff(entry,false); QTRY_VERIFY(!service.busy());
        QCOMPARE(versions.size(),1); auto snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]);
        QCOMPARE(snapshot.left,QByteArray("a\nb\nc\n")); QCOMPARE(snapshot.right,QByteArray("A\nb\nC\n"));
        const auto m=GitDiff::compare(snapshot.left,snapshot.right); QCOMPARE(m.hunks.size(),2);
        const auto target=GitDiff::selected(m,0,0);
        service.applySelection(dir.path(),path,snapshot.left,GitDiff::patch(path,snapshot.left,target)); QTRY_VERIFY(!service.busy());
        QCOMPARE(git(dir.path(),{"show",":"+path}),QByteArray("A\nb\nc\n"));
        QFile disk(dir.filePath(path)); QVERIFY(disk.open(QIODevice::ReadOnly)); QCOMPARE(disk.readAll(),snapshot.right);
        // Stale snapshot cannot overwrite a different index version.
        service.applySelection(dir.path(),path,snapshot.left,GitDiff::patch(path,snapshot.left,snapshot.right)); QTRY_VERIFY(!service.busy());
        QCOMPARE(errors.size(),1); QCOMPARE(git(dir.path(),{"show",":"+path}),target);
        service.diff(service.status().entries[0],true); QTRY_VERIFY(!service.busy());
        snapshot=qvariant_cast<GitDiffSnapshot>(versions.last()[0]);
        QCOMPARE(snapshot.left,QByteArray("a\nb\nc\n")); QCOMPARE(snapshot.right,target);
        service.applySelection(dir.path(),path,target,GitDiff::patch(path,target,snapshot.left)); QTRY_VERIFY(!service.busy());
        QCOMPARE(git(dir.path(),{"show",":"+path}),snapshot.left);
        // New executable files, partial first stage, then unstage removal.
        write(dir.path(),"new.sh","first\nsecond"); QFile::setPermissions(dir.filePath("new.sh"),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
        service.refresh(); QTRY_VERIFY(!service.busy());
        service.applySelection(dir.path(),"new.sh",{},GitDiff::patch("new.sh",{},"first\n",false,true,"100755"),false); QTRY_VERIFY(!service.busy());
        QCOMPARE(git(dir.path(),{"show",":new.sh"}),QByteArray("first\n")); QVERIFY(git(dir.path(),{"ls-files","--stage","new.sh"}).startsWith("100755"));
        service.applySelection(dir.path(),"new.sh","first\n",GitDiff::patch("new.sh","first\n",{},true,false,"100755")); QTRY_VERIFY(!service.busy());
        QVERIFY(git(dir.path(),{"ls-files","new.sh"}).isEmpty()); QVERIFY(QFile::exists(dir.filePath("new.sh")));
    }
    void simplifiedDiff() {
        GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="example.cpp";
        snapshot.left="a\nb\nc\n"; snapshot.right="A\nb\nextra\nc\n";
        GitDiffView view; view.resize(600,500); view.displayVersions(snapshot);
        auto* original=view.findChild<QTextEdit*>("gitDiffOld"); auto* current=view.findChild<QTextEdit*>("gitDiffNew");
        QTRY_COMPARE(original->toPlainText(),QString("a\nb\n\nc\n"));
        QCOMPARE(original->document()->findBlockByNumber(2).userState(),-1);
        QCOMPARE(original->document()->findBlockByNumber(3).userState(),2);
        QCOMPARE(current->toPlainText(),QString::fromUtf8(snapshot.right));
        QVERIFY(original->isReadOnly() && current->isReadOnly()); QVERIFY(original->isVisible() && current->isVisible());
        QVERIFY(!view.findChild<QPushButton*>("gitDiffSave")); QVERIFY(!view.findChild<QPushButton*>("gitDiffRevert"));
        QVERIFY(!view.findChild<QPushButton*>("gitDiffOpen")); QVERIFY(!view.findChild<QPushButton*>("gitDiffStage"));
        QVERIFY(!view.findChild<QComboBox*>("gitDiffMode")); QVERIFY(!view.findChild<QCheckBox*>("gitDiffFold"));
        QVERIFY(view.findChildren<QLabel*>().isEmpty());
        view.resize(350,500); QVERIFY(original->isVisible() && current->isVisible());
        snapshot.special=true; snapshot.patch="Binary files differ"; view.displayVersions(snapshot);
        auto* patch=view.findChild<QPlainTextEdit*>("gitDiffText"); QVERIFY(patch->isVisible()); QCOMPARE(patch->toPlainText(),snapshot.patch);
    }
    void alignedPaddingAndCopy() {
        struct Case { QByteArray before,after; QString oldView,newView; };
        const QVector<Case> cases={
            {"a\nb\n","start\na\nb\nend\n","\na\nb\n\n","start\na\nb\nend\n"},
            {"start\na\nb\nend\n","a\nb\n","start\na\nb\nend\n","\na\nb\n\n"},
            {"a\nremove\nb","a\nb","a\nremove\nb","a\n\nb"},
            {"","new\n","\n","new\n"},
            {"gone\n","","gone\n","\n"}
        };
        for(const auto& test:cases) {
            GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="alignment.txt";
            snapshot.left=test.before; snapshot.right=test.after;
            GitDiffView view; view.resize(600,400); view.displayVersions(snapshot);
            auto* old=view.findChild<QTextEdit*>("gitDiffOld");
            auto* current=view.findChild<QTextEdit*>("gitDiffNew");
            QTRY_COMPARE(old->toPlainText(),test.oldView);
            QCOMPARE(current->toPlainText(),test.newView);
            QCOMPARE(old->document()->blockCount(),current->document()->blockCount());
            for(auto* edit:{old,current}) {
                edit->selectAll(); edit->copy();
                QCOMPARE(QApplication::clipboard()->text(),GitDiff::displayText(edit==old?test.before:test.after));
            }
        }
    }
    void nativeEditorBinding() {
        GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="unicode.cpp";
        snapshot.left=QByteArray::fromHex("61c2a0620a"); snapshot.right=QByteArray("X")+snapshot.left;
        QByteArray bytes=snapshot.right; QPlainTextEdit editor; editor.setPlainText(QString::fromUtf8(bytes)); editor.show();
        int changes=-1;
        GitDiffView::LiveDocument live; live.editor=&editor; live.read=[&] { return bytes; }; live.available=[] { return true; };
        live.replace=[&](const QByteArray& before,const QByteArray& after) { if(before!=bytes) return false; bytes=after; editor.setPlainText(QString::fromUtf8(bytes)); return true; };
        live.highlight=[&](const GitDiff::Model* model) { changes=model?model->hunks.size():0; };
        QObject::connect(&editor,&QPlainTextEdit::textChanged,[&] { auto text=editor.document()->toRawText(); text.replace(QChar(0x2029),'\n'); bytes=text.toUtf8(); });
        GitDiffView view; view.displayVersions(snapshot,live);
        QTRY_COMPARE(changes,1);
        QVERIFY(view.findChild<QTextEdit*>("gitDiffNew")->isHidden()); QVERIFY(view.findChild<QTextEdit*>("gitDiffOld")->isVisible());
        editor.setFocus(); auto cursor=editor.textCursor(); cursor.setPosition(0); editor.setTextCursor(cursor); QTest::keyClick(&editor,Qt::Key_Y);
        view.documentChanged(); QTRY_VERIFY(view.findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+yX"));
        QVERIFY(bytes.contains(QByteArray::fromHex("c2a0")));
        view.revertFile(); QTRY_COMPARE(bytes,snapshot.left);
        view.hide(); QCOMPARE(changes,0);
    }
    void saveNotificationsAndRefresh() {
        GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="save.cpp";
        snapshot.left="base\n"; snapshot.right="live unsaved\n";
        QByteArray bytes=snapshot.right,painted; bool ready=true; int renders=0;
        QPlainTextEdit editor; editor.show();
        GitDiffView::LiveDocument live; live.editor=&editor; live.read=[&] { return bytes; }; live.available=[&] { return ready; };
        live.highlight=[&](const GitDiff::Model* model) { if(model) { painted=GitDiff::join(model->right); ++renders; } else painted.clear(); };
        GitDiffView view; view.displayVersions(snapshot,live); QTRY_COMPARE(painted,bytes);
        const int initial=renders;
        view.refreshVersions(snapshot); QTest::qWait(180); QCOMPARE(renders,initial);
        // A save may clear the native overlay without changing any text.
        painted.clear(); view.documentChanged(); QCOMPARE(painted,bytes);
        // A slow save can also cross the availability poll. Its disk snapshot
        // must not replace the live document or schedule an identical comparison.
        ready=false; view.setOperationBusy(true);
        auto disk=snapshot; disk.right="older disk version\n"; view.refreshVersions(disk);
        QTest::qWait(180); QCOMPARE(painted,bytes);
        painted.clear(); const int beforeReady=renders;
        ready=true; view.setOperationBusy(false); QCOMPARE(painted,bytes);
        QTest::qWait(180); QCOMPARE(renders,beforeReady+1);
        // A cached model must never be restored onto newly edited bytes.
        bytes="new edit\n"; painted.clear(); view.documentChanged(); QVERIFY(painted.isEmpty());
        QTRY_COMPARE(painted,bytes);
    }
    void parser() {
        GitStatus status;
        QByteArray bytes("## main...origin/main [ahead 1]"); bytes += '\0';
        bytes += "MM file\nname.txt"; bytes += '\0';
        bytes += "R  renamed.txt"; bytes += '\0'; bytes += "old.txt"; bytes += '\0';
        bytes += "UU conflict.txt"; bytes += '\0'; bytes += "?? new.txt"; bytes += '\0';
        QVERIFY(GitService::parseStatus(bytes, status));
        QCOMPARE(status.entries.size(), 4); QVERIFY(status.valid);
        QVERIFY(status.entries[0].staged()); QVERIFY(status.entries[0].unstaged());
        QCOMPARE(status.entries[0].path, QString("file\nname.txt"));
        QCOMPARE(status.entries[1].paths(), QStringList({"renamed.txt", "old.txt"}));
        QVERIFY(status.entries[2].conflict()); QVERIFY(!status.entries[2].staged());
        QVERIFY(status.entries[3].untracked());
        QVERIFY(!GitService::parseStatus("invalid", status));
        QVERIFY(!GitService::parseStatus(QByteArray("## main\0R  target\0", 19), status));
    }
    void roundTrip() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); init(dir.path());
        const QString path = QString::fromUtf8(":!файл [1]\n.txt");
        write(dir.path(), path, "one\n"); write(dir.path(), "other.txt", "other\n");
        GitService service; QSignalSpy errors(&service, &GitService::error);
        QSignalSpy diffs(&service, &GitService::diffReady); QSignalSpy commits(&service, &GitService::committed);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QVERIFY(service.status().unborn); QCOMPARE(service.status().entries.size(), 2);
        QCOMPARE(service.status().currentBranch, QString("main")); QCOMPARE(service.status().branches, QStringList{"main"});
        auto entry = service.status().entries[0];
        for (const auto& value : service.status().entries) if (value.path == path) entry = value;
        service.diff(entry, false); QTRY_VERIFY(!service.busy());
        QCOMPARE(diffs.size(), 1); QVERIFY(diffs.last()[2].toString().contains("+one"));
        service.stage({path}); QTRY_VERIFY(!service.busy());
        QVERIFY(git(dir.path(), {"diff", "--cached", "--name-only", "-z"}).contains(QFile::encodeName(path)));
        service.unstage({path}); QTRY_VERIFY(!service.busy());
        QVERIFY(git(dir.path(), {"diff", "--cached", "--name-only"}).isEmpty());
        QVERIFY(QFile::exists((dir.path() + QLatin1Char('/') + path)));
        service.stage({path}); QTRY_VERIFY(!service.busy());
        service.commit("First commit\n\nMessage with Unicode: привет\n"); QTRY_VERIFY(!service.busy());
        QCOMPARE(commits.size(), 1); QCOMPARE(service.status().entries.size(), 1);
        QVERIFY(git(dir.path(), {"log", "-1", "--format=%B"}).contains("Message with Unicode"));
        // Same file in both sections; each diff must use the correct baseline.
        write(dir.path(), path, "two\n"); service.stage({path}); QTRY_VERIFY(!service.busy());
        write(dir.path(), path, "three\n"); service.refresh(); QTRY_VERIFY(!service.busy());
        for (const auto& value : service.status().entries) if (value.path == path) entry = value;
        QVERIFY(entry.staged() && entry.unstaged());
        service.diff(entry, true); QTRY_VERIFY(!service.busy());
        QVERIFY(diffs.last()[2].toString().contains("+two")); QVERIFY(!diffs.last()[2].toString().contains("+three"));
        service.diff(entry, false); QTRY_VERIFY(!service.busy());
        QVERIFY(diffs.last()[2].toString().contains("-two")); QVERIFY(diffs.last()[2].toString().contains("+three"));
        service.commit("Only index"); QTRY_VERIFY(!service.busy());
        QCOMPARE(commits.size(), 2);
        QVERIFY(git(dir.path(), {"show", "HEAD:" + path}) == "two\n");
        QFile disk((dir.path() + QLatin1Char('/') + path)); QVERIFY(disk.open(QIODevice::ReadOnly)); QCOMPARE(disk.readAll(), QByteArray("three\n"));
        service.stage({path}); QTRY_VERIFY(!service.busy()); service.unstage({path}); QTRY_VERIFY(!service.busy());
        QVERIFY(git(dir.path(), {"diff", "--cached", "--name-only"}).isEmpty());
        QCOMPARE(errors.size(), 0);
    }
    void renameDeleteAndFailures() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(), "old.txt", "base\n");
        git(dir.path(), {"add", "."}); git(dir.path(), {"commit", "-m", "base"});
        git(dir.path(), {"mv", "old.txt", "new.txt"});
        GitService service; QSignalSpy errors(&service, &GitService::error); QSignalSpy diffs(&service, &GitService::diffReady);
        QSignalSpy commitErrors(&service, &GitService::commitFailed);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.status().entries.size(), 1); const auto rename = service.status().entries[0];
        QCOMPARE(rename.index, 'R'); QCOMPARE(rename.originalPath, QString("old.txt"));
        service.diff(rename, true); QTRY_VERIFY(!service.busy()); QVERIFY(diffs.last()[2].toString().contains("rename"));
        service.unstage(rename.paths()); QTRY_VERIFY(!service.busy()); QVERIFY(QFile::exists(dir.filePath("new.txt")));
        service.stage({"old.txt", "new.txt"}); QTRY_VERIFY(!service.busy()); service.commit("rename"); QTRY_VERIFY(!service.busy());
        QVERIFY(QFile::remove(dir.filePath("new.txt"))); service.stage({"new.txt"}); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.status().entries[0].index, 'D');
        service.unstage({"new.txt"}); QTRY_VERIFY(!service.busy()); QVERIFY(!QFile::exists(dir.filePath("new.txt")));
        write(dir.path(), ".git/index.lock", "");
        service.stage({"new.txt"}); QTRY_VERIFY(!service.busy()); QCOMPARE(errors.size(), 1);
        QVERIFY(QFile::remove(dir.filePath(".git/index.lock")));
        service.stage({"new.txt"}); QTRY_VERIFY(!service.busy());
        write(dir.path(), ".git/hooks/pre-commit", "#!/bin/sh\necho hook-rejected >&2\nexit 1\n");
        QVERIFY(QFile::setPermissions(dir.filePath(".git/hooks/pre-commit"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        service.commit("rejected"); QTRY_VERIFY(!service.busy());
        QCOMPARE(errors.size(), 1); QCOMPARE(commitErrors.size(), 1); QVERIFY(commitErrors.last()[0].toString().contains("hook-rejected"));
        QVERIFY(service.status().entries[0].staged());
    }
    void repositoriesAndStaleResults() {
        QTemporaryDir first, second, outside, worktree; init(first.path()); init(second.path());
        QDir(first.path()).mkdir("nested");
        GitService service;
        service.setRoots({first.path(), first.filePath("nested"), second.path(), outside.path()}); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.repository(), first.path());
        service.setRoots({second.path()}); QTRY_VERIFY(!service.busy()); QCOMPARE(service.repository(), second.path());
        service.refresh(); service.setRoots({outside.path()});
        QTRY_VERIFY(!service.busy() && service.repository().isEmpty());
        QTest::qWait(50); QVERIFY(service.repository().isEmpty());
        write(first.path(), "a", "a\n"); git(first.path(), {"add", "."}); git(first.path(), {"commit", "-m", "base"});
        git(first.path(), {"worktree", "add", "--detach", worktree.path()});
        service.setRoots({worktree.path()}); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.repository(), worktree.path()); QVERIFY(service.status().branch.contains("HEAD"));
    }
    void branches() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(), "a", "main\n");
        git(dir.path(), {"add", "."}); git(dir.path(), {"commit", "-m", "main"});
        git(dir.path(), {"switch", "-c", "side"}); write(dir.path(), "a", "side\n");
        git(dir.path(), {"commit", "-am", "side"}); git(dir.path(), {"switch", "main"});
        GitService service; QSignalSpy errors(&service, &GitService::error);
        QSignalSpy switched(&service, &GitService::branchSwitchFinished);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.status().currentBranch, QString("main"));
        QCOMPARE(service.status().branches, QStringList({"main", "side"}));
        service.switchBranch("side"); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.status().currentBranch, QString("side")); QVERIFY(switched.last()[1].toBool());
        write(dir.path(), "a", "local\n"); service.switchBranch("main"); QTRY_VERIFY(!service.busy());
        QCOMPARE(errors.size(), 1); QVERIFY(!switched.last()[1].toBool());
        QCOMPARE(service.status().currentBranch, QString("side"));
        QFile file(dir.filePath("a")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("local\n")); file.close();
        write(dir.path(), "a", "side\n"); service.switchBranch("main"); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.status().currentBranch, QString("main"));
        const auto head = git(dir.path(), {"rev-parse", "HEAD"});
        service.switchBranch("missing"); QVERIFY(!service.busy()); QCOMPARE(errors.size(), 2);
        QCOMPARE(git(dir.path(), {"rev-parse", "HEAD"}), head);
        const QString unicode = QString::fromUtf8("feature/пример");
        git(dir.path(), {"branch", unicode}); service.refresh(); QTRY_VERIFY(!service.busy());
        QVERIFY(service.status().branches.contains(unicode));
        service.switchBranch(unicode); QTRY_VERIFY(!service.busy()); QCOMPARE(service.status().currentBranch, unicode);
    }
    void conflicts() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(), "a", "base\n");
        git(dir.path(), {"add", "."}); git(dir.path(), {"commit", "-m", "base"});
        git(dir.path(), {"checkout", "-b", "side"}); write(dir.path(), "a", "side\n");
        git(dir.path(), {"commit", "-am", "side"}); git(dir.path(), {"checkout", "main"});
        write(dir.path(), "a", "main\n"); git(dir.path(), {"commit", "-am", "main"}); git(dir.path(), {"merge", "side"}, 1);
        GitService service; QSignalSpy diffs(&service, &GitService::diffReady);
        service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QVERIFY(service.status().entries[0].conflict());
        service.diff(service.status().entries[0], false); QTRY_VERIFY(!service.busy()); QVERIFY(!diffs.isEmpty());
        const auto head = git(dir.path(), {"rev-parse", "HEAD"}); service.commit("blocked"); QVERIFY(!service.busy());
        QCOMPARE(git(dir.path(), {"rev-parse", "HEAD"}), head);
    }
    void commitErrorDialog() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(), "a.txt", "new\n");
        const QByteArray details = "hook-rejected\n<b>literal text</b>\n" + QByteArray(6000, 'x');
        write(dir.path(), ".git/hooks/pre-commit", "#!/bin/sh\nprintf '%s\\n' '" + details + "' >&2\nexit 1\n");
        QVERIFY(QFile::setPermissions(dir.filePath(".git/hooks/pre-commit"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        GitPanel panel; panel.resize(360, 500); panel.show(); panel.setRoots({dir.path()});
        QTRY_VERIFY(!panel.service()->busy()); panel.stagePaths(dir.path(), {"a.txt"}); QTRY_VERIFY(!panel.service()->busy());
        auto* message = panel.findChild<QPlainTextEdit*>("gitCommitMessage"); message->setPlainText("Keep this draft");
        panel.findChild<QPushButton*>("gitCommit")->click();
        QTRY_VERIFY(!panel.service()->busy());
        auto* dialog = panel.findChild<QDialog*>("gitCommitErrorDialog"); QVERIFY(dialog && dialog->isVisible());
        auto* text = dialog->findChild<QPlainTextEdit*>("gitCommitErrorText"); QVERIFY(text && text->isReadOnly());
        QVERIFY(text->toPlainText().contains(QString::fromUtf8(details))); QVERIFY(text->toPlainText().size() > 6000);
        text->selectAll(); QCOMPARE(text->textCursor().selectedText().size(), text->toPlainText().size());
        QApplication::clipboard()->clear(); dialog->findChild<QPushButton*>("gitCommitErrorCopy")->click();
        QCOMPARE(QApplication::clipboard()->text(), text->toPlainText());
        QVERIFY(!panel.findChild<QLabel*>("gitError")->isVisible());
        QCOMPARE(message->toPlainText(), QString("Keep this draft")); QVERIFY(panel.service()->status().entries[0].staged());
        if (qEnvironmentVariableIsSet("TURTLE_CODE_GIT_ERROR_PREVIEW")) {
            text->moveCursor(QTextCursor::Start);
            QVERIFY(dialog->grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_ERROR_PREVIEW")));
        }
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close)->click();
        QVERIFY(!dialog->isVisible());
    }
    void multipleRepositoriesAndDrafts() {
        QTemporaryDir dir; init(dir.path());
        write(dir.path(), "a.txt", "outer base\n"); git(dir.path(), {"add", "."}); git(dir.path(), {"commit", "-m", "base"});
        const auto nested=dir.filePath("vendor/nested"); QVERIFY(QDir().mkpath(nested)); init(nested);
        write(nested,"a.txt","inner base\n"); git(nested,{"add","."}); git(nested,{"commit","-m","base"});
        write(dir.path(),"a.txt","outer changed\n"); write(nested,"a.txt","inner changed\n");
        GitPanel panel; panel.resize(320,700); panel.show(); panel.setRoots({dir.path()});
        QTRY_VERIFY(!panel.service()->busy()); QCOMPARE(panel.service()->repositories().size(),2);
        QWidget *outer=nullptr,*inner=nullptr;
        for(auto* section:panel.findChildren<QWidget*>("gitRepository")) {
            if(section->property("repository").toString()==nested) inner=section;
            if(section->property("repository").toString()==dir.path()) outer=section;
        }
        QVERIFY(outer && inner);
        auto* outerMessage=outer->findChild<QPlainTextEdit*>("gitCommitMessage");
        auto* innerMessage=inner->findChild<QPlainTextEdit*>("gitCommitMessage");
        outerMessage->setPlainText("outer draft"); innerMessage->setPlainText("inner commit");
        QSignalSpy stage(&panel,&GitPanel::stageRequested);
        auto* tree=inner->findChild<QTreeWidget*>("gitFiles"); auto* row=tree->topLevelItem(1)->child(0); QVERIFY(row);
        QVERIFY(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,2)));
        QCOMPARE(stage.size(),1); QCOMPARE(stage[0][0].toString(),nested);
        panel.stagePaths(stage[0][0].toString(),stage[0][1].toStringList()); QTRY_VERIFY(!panel.service()->busy());
        QVERIFY(git(dir.path(),{"diff","--cached","--name-only"}).isEmpty());
        QCOMPARE(git(nested,{"diff","--cached","--name-only"}),QByteArray("a.txt\n"));
        innerMessage->setFocus(); QTest::keyClick(innerMessage,Qt::Key_Return,Qt::ControlModifier);
        QTRY_VERIFY(!panel.service()->busy() && innerMessage->toPlainText().isEmpty());
        QCOMPARE(outerMessage->toPlainText(),QString("outer draft"));
        QCOMPARE(git(nested,{"log","-1","--format=%s"}),QByteArray("inner commit\n"));
        panel.refresh(); QTRY_VERIFY(!panel.service()->busy());
        QCOMPARE(outerMessage->toPlainText(),QString("outer draft"));
        if(qEnvironmentVariableIsSet("TURTLE_CODE_GIT_PANEL_PREVIEW")) {
            QApplication::processEvents(); QVERIFY(panel.grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_PANEL_PREVIEW")));
        }
    }
    void discardKeepsIndex() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(),"a.txt","base\n");
        git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        write(dir.path(),"a.txt","staged\n"); git(dir.path(),{"add","a.txt"}); write(dir.path(),"a.txt","unstaged\n");
        GitService service; service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QSignalSpy finished(&service,&GitService::branchSwitchFinished); service.discard(service.status().entries[0]);
        QTRY_VERIFY(!service.busy()); QCOMPARE(finished.size(),1); QVERIFY(finished[0][1].toBool());
        QCOMPARE(git(dir.path(),{"show",":a.txt"}),QByteArray("staged\n"));
        QFile file(dir.filePath("a.txt")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),QByteArray("staged\n"));
        QVERIFY(service.status().entries[0].staged()); QVERIFY(!service.status().entries[0].unstaged());
    }
    void remoteOperations() {
        QTemporaryDir dir, remote, other; init(dir.path());
        git(remote.path(),{"init","--bare","-b","main"});
        write(dir.path(),"a.txt","base\n"); git(dir.path(),{"add","."}); git(dir.path(),{"commit","-m","base"});
        git(dir.path(),{"remote","add","origin",remote.path()}); git(dir.path(),{"push","-u","origin","main"});
        init(other.path()); git(other.path(),{"remote","add","origin",remote.path()});
        git(other.path(),{"fetch","origin"}); git(other.path(),{"reset","--hard","origin/main"});
        GitService service; service.setRoots({dir.path()}); QTRY_VERIFY(!service.busy());
        QSignalSpy errors(&service,&GitService::error);
        write(dir.path(),"a.txt","pushed\n"); service.stage({"a.txt"}); QTRY_VERIFY(!service.busy());
        service.commit("push test"); QTRY_VERIFY(!service.busy()); service.push(); QTRY_VERIFY(!service.busy());
        QCOMPARE(git(remote.path(),{"show","main:a.txt"}),QByteArray("pushed\n"));
        git(other.path(),{"fetch","origin"}); git(other.path(),{"reset","--hard","origin/main"});
        write(other.path(),"a.txt","pulled\n"); git(other.path(),{"add","."}); git(other.path(),{"commit","-m","pull test"}); git(other.path(),{"push","origin","main"});
        service.fetch(); QTRY_VERIFY(!service.busy());
        QSignalSpy pulled(&service,&GitService::branchSwitchFinished); service.pull(); QTRY_VERIFY(!service.busy());
        QCOMPARE(pulled.size(),1); QVERIFY(pulled[0][1].toBool()); QVERIFY(errors.isEmpty());
        QCOMPARE(git(dir.path(),{"show","HEAD:a.txt"}),QByteArray("pulled\n"));
    }
    void stableRefresh() {
        QTemporaryDir dir; init(dir.path());
        for (int i = 0; i < 100; ++i) write(dir.path(), QString("file%1.txt").arg(i, 3, 10, QLatin1Char('0')), "new\n");
        GitPanel panel; panel.resize(360, 500); panel.show(); panel.setRoots({dir.path()});
        QTRY_VERIFY(!panel.service()->busy());
        auto* tree = panel.findChild<QTreeWidget*>("gitFiles");
        auto* commit = panel.findChild<QPushButton*>("gitCommit"); QCOMPARE(commit->text(), QString("Commit"));
        panel.layout()->activate();
        const auto geometry = tree->geometry();
        auto* group = tree->topLevelItem(1); auto* selected = group->child(70);
        tree->setCurrentItem(selected); tree->scrollToItem(selected, QAbstractItemView::PositionAtCenter);
        const auto scroll = tree->verticalScrollBar()->value(); const auto name = selected->text(0);
        for (int i = 0; i < 3; ++i) {
            panel.refresh(); panel.layout()->activate();
            QCOMPARE(tree->geometry(), geometry);
            QTRY_VERIFY(!panel.service()->busy()); panel.layout()->activate();
            QCOMPARE(tree->geometry(), geometry); QCOMPARE(tree->currentItem(), selected);
            QCOMPARE(tree->verticalScrollBar()->value(), scroll);
        }
        write(dir.path(), "another.txt", "added\n"); panel.refresh(); QTRY_VERIFY(!panel.service()->busy());
        QCOMPARE(tree->currentItem()->text(0), name); QCOMPARE(tree->verticalScrollBar()->value(), scroll);
        tree->topLevelItem(1)->setExpanded(false);
        write(dir.path(), "extra.txt", "added\n"); panel.refresh(); QTRY_VERIFY(!panel.service()->busy());
        QVERIFY(!tree->topLevelItem(1)->isExpanded());
    }
    void panelAndDiff() {
        QTemporaryDir dir; init(dir.path()); write(dir.path(), "a.txt", "new\n");
        GitPanel panel; panel.resize(300, 600); panel.show(); panel.setRoots({dir.path()});
        QTRY_VERIFY(!panel.service()->busy());
        auto* tree = panel.findChild<QTreeWidget*>("gitFiles"); QVERIFY(tree);
        auto* message = panel.findChild<QPlainTextEdit*>("gitCommitMessage"); QVERIFY(message);
        auto* commit = panel.findChild<QPushButton*>("gitCommit"); QVERIFY(commit); QVERIFY(!commit->isEnabled());
        QSignalSpy stage(&panel, &GitPanel::stageRequested);
        auto* row = tree->topLevelItem(1)->child(0); QVERIFY(row);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem*, row), Q_ARG(int, 2)));
        QCOMPARE(stage.size(), 1); panel.stagePaths(stage[0][0].toString(), stage[0][1].toStringList());
        QTRY_VERIFY(!panel.service()->busy());
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1); message->setPlainText("UI commit"); QVERIFY(commit->isEnabled());
        panel.refresh(); QVERIFY(message->isEnabled()); QCOMPARE(message->toPlainText(), QString("UI commit"));
        QTRY_VERIFY(!panel.service()->busy()); QVERIFY(commit->isEnabled());
        commit->click(); QTRY_VERIFY(!panel.service()->busy()); QVERIFY(message->toPlainText().isEmpty());
        QVERIFY(panel.service()->status().entries.isEmpty());
        GitDiffView diff; diff.display(dir.filePath("a.txt"), false, "@@ -1 +1,2 @@\n base\n+new\n");
        auto* text = diff.findChild<QPlainTextEdit*>("gitDiffText"); QVERIFY(text && text->isReadOnly());
        QVERIFY(text->toPlainText().contains("+new"));

        QVERIFY(!diff.findChild<QPushButton*>("gitDiffOpen"));
    }
};
QTEST_MAIN(GitTests)
#include "GitTests.moc"
