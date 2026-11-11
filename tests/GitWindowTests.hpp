#pragma once
#include "git/GitPanel.hpp"
#include "app/ShortcutManager.hpp"
#include "git/GitDiffView.hpp"
#include <QToolButton>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QTextBlock>
#include <QClipboard>
#include <QHBoxLayout>
#include <QProcess>
#include <QMenu>
static void diffScrollAlignmentTests() {
    QWidget host; host.resize(1100,500); auto* layout=new QHBoxLayout(&host);
    auto* editor=new EditorWidget(&host); auto* view=new GitDiffView(&host);
    layout->addWidget(editor); layout->addWidget(view); host.show();
    GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="scroll.txt";
    snapshot.right="inserted at start\n";
    for(int i=0;i<120;++i) {
        const auto line="line "+QByteArray::number(i)+"\n";
        snapshot.left+=line;
        if(i<20 || i>=40) snapshot.right+=line;
    }
    editor->loadBuffer(PieceTreeBuffer(snapshot.right.toStdString()),"/tmp/scroll.txt");
    GitDiffView::LiveDocument live; live.editor=editor; live.available=[] { return true; };
    live.read=[editor] { return QByteArray::fromStdString(editor->snapshotBuffer().text()); };
    int alignmentUpdates=0;
    live.align=[editor,&alignmentUpdates](const QVector<int>& rows) { ++alignmentUpdates; editor->setDiffAlignment(rows); };
    live.highlight=[editor](const GitDiff::Model* model) {
        QVector<EditorWidget::DiffHighlight> highlights;
        if (model) for (const auto& row : model->rows) if (row.changed && row.right>=0)
            highlights.append({row.right,0,int(GitDiff::displayText(model->right[row.right]).size())});
        editor->setDiffHighlights(highlights);
    };
    QObject::connect(editor,&EditorWidget::contentChanged,view,&GitDiffView::documentChanged);
    view->displayVersions(snapshot,live);
    auto* old=view->findChild<QTextEdit*>("gitDiffOld");
    assert(waitFor([&] { return editor->visualRowForLine(21)==41; }));
    editor->verticalScrollBar()->setValue(25); QApplication::processEvents();
    assert(old->cursorRect(QTextCursor(old->document()->findBlockByNumber(25))).top()==int(old->document()->documentMargin()));
    const int target=50;
    const int y=old->cursorRect(QTextCursor(old->document()->findBlockByNumber(target))).top()+
        old->verticalScrollBar()->value()-int(old->document()->documentMargin());
    old->verticalScrollBar()->setValue(y); QApplication::processEvents();
    assert(editor->verticalScrollBar()->value()==target);
    editor->goToLine(22); QApplication::processEvents();
    assert(editor->navigationPosition().x()==22);
    QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
    assert(editor->selectedText()==QString::fromUtf8(snapshot.right));
    assert(editor->document()->buffer().text()==snapshot.right.toStdString());
    // Typing must not remove/reinstall padding or move the viewport between
    // the synchronous edit and the asynchronous comparison result.
    editor->setNavigationPosition(46,3);
    editor->verticalScrollBar()->setValue(editor->visualRowForLine(40));
    const auto caretY=[&] { return editor->viewport()->mapFromGlobal(editor->cursorScreenPosition()).y(); };
    const int stableY=caretY(), stableTop=editor->verticalScrollBar()->value();
    QVector<int> scrollEvents;
    const auto connection=QObject::connect(editor->verticalScrollBar(),&QScrollBar::valueChanged,
        editor,[&](int value) { scrollEvents.append(value); });
    const auto waitComparison=[&](int previous) { assert(waitFor([&] { return alignmentUpdates>previous; })); };
    bool alreadyChanged=false;
    const auto lineColor=[&] {
        const int height=editor->fontMetrics().height()+3;
        return editor->viewport()->grab().toImage().pixelColor(editor->viewport()->width()-15,
            (editor->visualRowForLine(45)-editor->verticalScrollBar()->value())*height+height-1);
    };
    for (const auto key : {Qt::Key_X,Qt::Key_Y,Qt::Key_Backspace}) {
        const auto beforeColor=lineColor();
        const int previous=alignmentUpdates;
        QTest::keyClick(editor,key);
        if (alreadyChanged) assert(lineColor()==beforeColor);
        assert(editor->visualRowForLine(45)==65);
        assert(caretY()==stableY && editor->verticalScrollBar()->value()==stableTop);
        waitComparison(previous);
        assert(caretY()==stableY && editor->verticalScrollBar()->value()==stableTop);
        if (alreadyChanged) assert(lineColor()==beforeColor);
        alreadyChanged=true;
    }
    int beforeBurst=alignmentUpdates;
    QTest::keyClicks(editor,"fast typing");
    assert(caretY()==stableY && editor->visualRowForLine(45)==65);
    waitComparison(beforeBurst); assert(caretY()==stableY);
    for (int value : scrollEvents) assert(value==stableTop);
    const int height=editor->fontMetrics().height()+3;
    int previous=alignmentUpdates;
    QTest::keyClick(editor,Qt::Key_Return);
    assert(caretY()==stableY+height);
    waitComparison(previous); assert(caretY()==stableY+height);
    previous=alignmentUpdates;
    QTest::keyClick(editor,Qt::Key_Backspace);
    assert(caretY()==stableY);
    waitComparison(previous); assert(caretY()==stableY);
    previous=alignmentUpdates;
    editor->documentUndo(); const int undoY=caretY();
    waitComparison(previous); assert(caretY()==undoY);
    previous=alignmentUpdates;
    editor->documentUndo(true); const int redoY=caretY();
    waitComparison(previous); assert(caretY()==redoY);
    QObject::disconnect(connection);
    // Cursor reveal and bottom scrolling use visual rows as well.
    editor->goToLine(100); QApplication::processEvents();
    const int cursorY=editor->viewport()->mapFromGlobal(editor->cursorScreenPosition()).y();
    assert(cursorY>=0 && cursorY<=editor->viewport()->height());
    view->hide(); assert(editor->visualRowForLine(21)==21);
}
static void gitWindowTests(QTemporaryDir& directory) {
    diffScrollAlignmentTests();
    if (QStandardPaths::findExecutable("git").isEmpty()) return;
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null"); qputenv("GIT_CONFIG_NOSYSTEM", "1");
    const auto root = directory.filePath("repository"); assert(QDir().mkpath(root));
    const auto command = [&](const QStringList& args) {
        QProcess process; process.setWorkingDirectory(root); process.start("git", args);
        assert(process.waitForFinished(10000)); assert(process.exitCode() == 0); return process.readAllStandardOutput();
    };
    command({"init", "-b", "main"}); command({"config", "user.name", "Git Window Test"});
    command({"config", "user.email", "test@example.invalid"}); command({"config", "commit.gpgsign", "false"});
    const auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path); assert(file.open(QIODevice::WriteOnly)); assert(file.write(bytes) == bytes.size());
    };
    const auto read = [](const QString& path) {
        QFile file(path); assert(file.open(QIODevice::ReadOnly)); return file.readAll();
    };
    const auto path = root + "/hello.cpp";
    write(path, "int main() {\n    return 0;\n}\n");
    command({"add", "."}); command({"commit", "-m", "base"}); command({"branch", "side"});
    write(path, "int main() {\n    return 42;\n}\n");
    assert(I18n::saveLanguage("ru")); assert(I18n::initialize()); ThemeManager::instance().initialize();
    MainWindow window; window.resize(1250, 780); window.show(); window.openPaths({root});
    auto* panel = window.findChild<GitPanel*>(); assert(panel);
    assert(waitFor([&] { return !panel->service()->busy() && panel->service()->repository() == root; }));
    auto* button = window.findChild<QToolButton*>("gitActivity"); assert(button); button->click();
    assert(waitFor([&] { return !panel->service()->busy(); })); assert(panel->isVisible());
    auto* tree = panel->findChild<QTreeWidget*>("gitFiles"); assert(tree->topLevelItem(1)->childCount() == 1);
    bool firstBindingChecked=false;
    QObject::connect(panel->service(),&GitService::versionsReady,&window,[&](const GitDiffSnapshot& snapshot) {
        if(firstBindingChecked || snapshot.staged || snapshot.entry.path!="hello.cpp") return;
        auto* firstDiff=window.findChild<GitDiffView*>(); assert(firstDiff);
        // Inspect immediately, while the file load can still be pending.
        assert(firstDiff->nativeEditor());
        assert(firstDiff->findChild<QTextEdit*>("gitDiffNew")->isHidden());
        firstBindingChecked=true;
    });
    auto* row = tree->topLevelItem(1)->child(0);
    assert(QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem*, row), Q_ARG(int, 0)));
    assert(waitFor([&] { return !panel->service()->busy() && window.findChild<GitDiffView*>(); }));
    auto* diff = window.findChild<GitDiffView*>(); assert(diff->isVisible()); assert(firstBindingChecked);
    assert(waitFor([&] { return diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+    return 42;"); }));
    auto* editor = window.findChild<EditorWidget*>(); assert(editor);
    assert(waitFor([&] { return !editor->isBusy(); }));
    const auto saveWithShortcut=[&] {
        window.activateWindow(); editor->setFocus(); QApplication::processEvents();
        QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);
    };
    if (qEnvironmentVariableIsSet("TURTLE_CODE_GIT_PREVIEW")) {
        QApplication::processEvents(); assert(window.grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_PREVIEW")));
    }
    assert(diff->nativeEditor()==editor);
    assert(diff->mapToGlobal(QPoint(0,0)).x() < editor->mapToGlobal(QPoint(0,0)).x());
    diff->hide();
    assert(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,0)));
    assert(waitFor([&] { return !panel->service()->busy() && diff->isVisible(); }));
    assert(diff->nativeEditor()==editor);
    assert(window.findChildren<EditorWidget*>().size()==1);
    auto* original=diff->findChild<QTextEdit*>("gitDiffOld"); assert(original && original->isReadOnly());
    assert(diff->findChild<QTextEdit*>("gitDiffNew")->isHidden());
    assert(!diff->findChild<QPushButton*>("gitDiffOpen")); assert(!diff->findChild<QPushButton*>("gitDiffStage"));
    assert(!diff->findChild<QComboBox*>("gitDiffMode"));
    // Insertions pad the original pane; deletions pad the real editable view.
    // Neither alignment nor copying may insert blank lines into the document.
    const auto replaceForAlignment=[&](const QString& text) {
        editor->setFocus(); QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
        QApplication::clipboard()->setText(text); editor->pasteClipboard();
    };
    replaceForAlignment("// new line\nint main() {\n    return 42;\n}\n");
    assert(waitFor([&] { return original->document()->firstBlock().userState()==-1; }));
    assert(original->document()->findBlockByNumber(1).text()=="int main() {");
    assert(original->document()->findBlockByNumber(1).userState()==0);
    assert(editor->visualRowForLine(1)==1);
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    if(qEnvironmentVariableIsSet("TURTLE_CODE_GIT_ALIGNMENT_PREVIEW")) {
        QApplication::processEvents(); assert(window.grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_ALIGNMENT_PREVIEW")));
    }
    editor->documentUndo();
    assert(waitFor([&] { return original->document()->firstBlock().userState()==0; }));
    replaceForAlignment("int main() {\n}\n");
    assert(waitFor([&] { return editor->visualRowForLine(1)==2; }));
    assert(editor->document()->buffer().text()=="int main() {\n}\n");
    const int rowHeight=editor->fontMetrics().height()+3;
    QTest::mouseClick(editor->viewport(),Qt::LeftButton,Qt::NoModifier,
        QPoint(editor->lineNumberAreaWidth()+12,2*rowHeight+rowHeight/2));
    assert(editor->navigationPosition()==QPoint(2,1));
    QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
    assert(editor->selectedText()=="int main() {\n}\n");
    editor->documentUndo();
    assert(waitFor([&] { return editor->visualRowForLine(1)==1 &&
        diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+    return 42;"); }));
    assert(!editor->isModified());
    // Hiding the comparison removes its spacers from the editor.
    replaceForAlignment("int main() {\n}\n");
    assert(waitFor([&] { return editor->visualRowForLine(1)==2; }));
    diff->hide(); assert(editor->visualRowForLine(1)==1);
    editor->documentUndo();
    assert(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,0)));
    assert(waitFor([&] { return diff->isVisible() && !panel->service()->busy() &&
        diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+    return 42;"); }));
    // A quick save can complete between the diff's availability polls. Its
    // unchanged-content notification must preserve the actual painted ranges.
    const auto diffPixel=[&] {
        const int height=editor->fontMetrics().height()+3;
        return editor->viewport()->grab().toImage().pixelColor(editor->viewport()->width()-20,height+height/2);
    };
    assert(waitFor([&] { const auto c=diffPixel(); return c.green()>c.red()+10; }));
    const auto beforeSave=diffPixel();
    for(int attempt=0;attempt<2;++attempt) {
        editor->setFocus(); saveWithShortcut(); assert(editor->isBusy());
        assert(waitFor([&] { return !editor->isBusy(); }));
        assert(diffPixel()==beforeSave);
        assert(read(path)=="int main() {\n    return 42;\n}\n");
    }
    // Manually restoring the original text must clear both sides without Save.
    editor->setFocus(); editor->setNavigationPosition(2,12);
    QTest::keyClick(editor,Qt::Key_Delete); QTest::keyClick(editor,Qt::Key_Delete); QTest::keyClick(editor,Qt::Key_0);
    assert(editor->document()->buffer().text()=="int main() {\n    return 0;\n}\n");
    assert(waitFor([&] { return diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().isEmpty(); }));
    assert(waitFor([&] { const auto c=diffPixel(); return c.green()<=c.red()+10; }));
    assert(original->extraSelections().isEmpty());
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    editor->setNavigationPosition(2,12); QTest::keyClick(editor,Qt::Key_Delete);
    QTest::keyClick(editor,Qt::Key_4); QTest::keyClick(editor,Qt::Key_2);
    assert(editor->document()->buffer().text()=="int main() {\n    return 42;\n}\n");
    assert(waitFor([&] { const auto c=diffPixel(); return c.green()>c.red()+10; }));
    saveWithShortcut(); assert(waitFor([&] { return !editor->isBusy() && !editor->isModified(); }));
    editor->setFocus(); editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_Z);
    assert(editor->isModified()); assert(editor->document()->buffer().text().starts_with("zint"));
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    assert(waitFor([&] { return diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+zint"); }));
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    assert(waitFor([&] { return !editor->isModified(); }));
    editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_X); assert(editor->isModified());
    assert(waitFor([&] { return diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+xint"); }));
    // File-tree Revert restores and saves the working file, preserves the index,
    // and remains undoable through the native editor.
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    assert(!editor->isModified());
    diff->hide();
    row=tree->topLevelItem(1)->child(0); assert(row); tree->scrollToItem(row);
    const auto point=tree->visualItemRect(row).center();
    assert(QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,point)));
    auto* fileMenu=tree->findChild<QMenu*>("gitFileMenu"); assert(fileMenu && fileMenu->isVisible());
    auto* revert=fileMenu->findChild<QAction*>("gitFileRevert"); assert(revert && revert->isEnabled());
    const auto diskBeforeRevert=read(path),indexBeforeRevert=command({"show",":hello.cpp"});
    revert->trigger(); fileMenu->close();
    assert(waitFor([&] { return !panel->service()->busy() && !editor->isBusy() && !editor->isModified() && editor->document()->buffer().text()==indexBeforeRevert.toStdString(); }));
    assert(!editor->isModified()); assert(diff->nativeEditor()==editor);
    assert(read(path)==indexBeforeRevert); assert(command({"show",":hello.cpp"})==indexBeforeRevert);
    editor->setFocus(); QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    assert(editor->document()->buffer().text()==diskBeforeRevert.toStdString()); assert(editor->isModified());
    // Restore a saved working-tree change for the following Stage/Cancel checks.
    saveWithShortcut();
    assert(waitFor([&] { return !editor->isBusy() && !editor->isModified() && !panel->service()->busy() && tree->topLevelItem(1)->childCount()==1; }));
    editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_X); assert(editor->isModified());
    // Cancel must preserve the buffer, working file, and index.
    bool asked = false;
    QTimer cancel; cancel.setInterval(1);
    QObject::connect(&cancel, &QTimer::timeout, [&] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            asked = true; dialog->button(QMessageBox::Cancel)->click();
        }
    });
    cancel.start();
    row=tree->topLevelItem(1)->child(0);
    assert(QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem*, row), Q_ARG(int, 2)));
    cancel.stop(); assert(asked); assert(editor->isModified()); assert(command({"diff", "--cached", "--name-only"}).isEmpty());
    // Save is asynchronous; Stage must run after it and use the saved revision.
    QTimer save; save.setInterval(1);
    QObject::connect(&save, &QTimer::timeout, [] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Save)->click();
    });
    save.start();
    row = tree->topLevelItem(1)->child(0);
    assert(QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem*, row), Q_ARG(int, 2)));
    assert(waitFor([&] { return !editor->isBusy() && !panel->service()->busy() && !panel->service()->status().entries.isEmpty() && panel->service()->status().entries[0].staged(); }));
    save.stop(); assert(!editor->isModified());
    assert(command({"show", ":hello.cpp"}) == read(path));
    const auto onlyMain = root + "/main-only.txt"; write(onlyMain, "Only on main\n");
    panel->stagePaths(root, {"main-only.txt"}); assert(waitFor([&] { return !panel->service()->busy(); }));
    window.openPaths({onlyMain});
    assert(waitFor([&] { for (auto* item : window.findChildren<EditorWidget*>()) if (item->isBusy()) return false; return true; }));
    auto* message = panel->findChild<QPlainTextEdit*>("gitCommitMessage"); message->setPlainText("Commit from main window");
    auto* commit = panel->findChild<QPushButton*>("gitCommit"); assert(waitFor([&] { return commit->isEnabled(); })); commit->click();
    assert(waitFor([&] { return !panel->service()->busy(); })); assert(panel->service()->status().entries.isEmpty());
    assert(command({"show", "HEAD:hello.cpp"}) == read(path)); assert(message->toPlainText().isEmpty());
    // Discard also restores an unsaved buffer when disk already equals the index.
    editor->setFocus(); editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_X);
    assert(editor->isModified());
    GitEntry discardEntry; discardEntry.path="hello.cpp"; discardEntry.worktree='M';
    asked=false; cancel.start();
    emit panel->discardRequested(root,discardEntry);
    cancel.stop(); assert(asked && editor->isModified());
    const auto unchangedIndex=command({"show",":hello.cpp"});
    QTimer confirmDiscard; confirmDiscard.setInterval(1);
    QObject::connect(&confirmDiscard,&QTimer::timeout,[] {
        if(auto* dialog=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click();
    });
    confirmDiscard.start(); emit panel->discardRequested(root,discardEntry); confirmDiscard.stop();
    assert(waitFor([&] { return !panel->service()->busy() && !editor->isBusy() && !editor->isModified(); }));
    assert(editor->document()->buffer().text()==unchangedIndex.toStdString());
    assert(read(path)==unchangedIndex && command({"show",":hello.cpp"})==unchangedIndex);
    // Selecting a staged comparison must keep the real editable document.
    assert(waitFor([&] { return !panel->service()->busy(); }));
    GitEntry stagedEntry; stagedEntry.path="hello.cpp"; panel->service()->diff(stagedEntry,true);
    assert(waitFor([&] { return diff->isVisible() && diff->staged() && !panel->service()->busy(); }));
    assert(!window.findChild<QPlainTextEdit*>("gitIndexPreview"));
    assert(diff->nativeEditor()==editor); assert(diff->findChild<QTextEdit*>("gitDiffNew")->isHidden());
    const auto indexBeforeEdit=command({"show",":hello.cpp"});
    editor->setFocus(); editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_Z);
    assert(editor->isModified()); assert(editor->document()->buffer().text()=="z"+indexBeforeEdit.toStdString());
    assert(waitFor([&] { return diff->findChild<QPlainTextEdit*>("gitDiffText")->toPlainText().contains("+z"+QString::fromUtf8(indexBeforeEdit).section('\n',0,0)); }));
    assert(command({"show",":hello.cpp"})==indexBeforeEdit);
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    assert(waitFor([&] { return !editor->isModified(); }));
    editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_X);
    assert(!diff->findChild<QPushButton*>("gitDiffSave"));
    assert(!diff->findChild<QPushButton*>("gitDiffRevert"));
    editor->setFocus(); saveWithShortcut();
    assert(waitFor([&] { return !editor->isBusy() && !editor->isModified(); }));
    assert(read(path)=="x"+indexBeforeEdit); assert(command({"show",":hello.cpp"})==indexBeforeEdit);
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    saveWithShortcut();
    assert(waitFor([&] { return !editor->isBusy() && !editor->isModified(); }));
    assert(read(path)==indexBeforeEdit);
    diff->hide();
    // The repository selector and add-folder command are gone.
    assert(!panel->findChild<QComboBox*>("gitRepositories"));
    for (const auto& item : ShortcutManager::instance().commands()) assert(item.id != "file.addFolder");
    auto* branches = panel->findChild<QComboBox*>("gitBranches"); assert(branches);
    assert(branches->currentData().toString() == "main"); assert(branches->findData("side") >= 0);
    bool oldHint = false;
    for (auto* label : panel->findChildren<QLabel*>()) oldHint |= label->text().contains(QString::fromUtf8("Нажмите на файл"));
    assert(!oldHint);
    // Selecting a branch refreshes the real editor's clean buffer.
    assert(QMetaObject::invokeMethod(branches, "activated", Qt::DirectConnection, Q_ARG(int, branches->findData("side"))));
    assert(waitFor([&] { return !panel->service()->busy() && !editor->isBusy(); }));
    assert(panel->service()->status().currentBranch == "side");
    assert(waitFor([&] { return window.findChild<QTabWidget*>()->count() == 1; }));
    assert(!QFileInfo::exists(onlyMain));
    assert(editor->document()->buffer().text() == "int main() {\n    return 0;\n}\n");
    assert(!diff->isVisible());
    // Cancel before switching keeps unsaved text and the current branch.
    QTest::keyClick(editor, Qt::Key_Y); assert(editor->isModified()); asked = false; cancel.start();
    assert(QMetaObject::invokeMethod(branches, "activated", Qt::DirectConnection, Q_ARG(int, branches->findData("main"))));
    cancel.stop(); assert(asked); assert(editor->isModified()); assert(panel->service()->status().currentBranch == "side");
    // Save first; if Git refuses the transition, retain both the saved text and branch.
    save.start();
    assert(QMetaObject::invokeMethod(branches, "activated", Qt::DirectConnection, Q_ARG(int, branches->findData("main"))));
    assert(waitFor([&] { return !panel->service()->busy() && !editor->isBusy() && !editor->isModified(); }));
    save.stop();
    assert(panel->service()->status().currentBranch == "side");
    assert(editor->document()->buffer().text() == read(path).toStdString());
    assert(panel->findChild<QLabel*>("gitError")->isVisible());
    window.close(); assert(waitFor([&] { return !window.isVisible(); }));
}
