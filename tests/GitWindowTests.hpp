#pragma once
#include "git/GitPanel.hpp"
#include "app/ShortcutManager.hpp"
#include "git/GitDiffView.hpp"
#include "git/GitDiffSourcePane.hpp"
#include <QToolButton>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QClipboard>
#include <QHBoxLayout>
#include <QProcess>
#include <QMenu>
#include <QWheelEvent>
static void diffScrollAlignmentTests() {
    QWidget host; host.resize(1100,500); auto* layout=new QHBoxLayout(&host);
    auto* editor=new EditorWidget(&host); auto* view=new GitDiffView(&host);
    layout->addWidget(editor); host.show();
    GitDiffSnapshot snapshot; snapshot.repository="/tmp"; snapshot.entry.path="scroll.txt";
    snapshot.right="inserted at start\n";
    for(int i=0;i<120;++i) {
        const auto line="line "+QByteArray::number(i)+"\n";
        snapshot.left+=line;
        if(i==100) snapshot.right+="modified\n";
        else if(i<20 || i>=40) snapshot.right+=line;
    }
    editor->loadBuffer(PieceTreeBuffer(snapshot.right.toStdString()),"/tmp/scroll.txt");
    GitDiffView::LiveDocument live; live.editor=editor; live.available=[] { return true; };
    live.revision=[editor] { return editor->document()->buffer().contentRevision(); };
    live.read=[editor] { return QByteArray::fromStdString(editor->snapshotBuffer().text()); };
    int alignmentUpdates=0;
    live.align=[editor,&alignmentUpdates](const LineAlignment& alignment) { ++alignmentUpdates; editor->setDiffAlignment(alignment); };
    live.highlight=[editor](const GitDiff::Model* model) {
        QVector<EditorWidget::DiffHighlight> highlights;
        if (model) for (const auto& hunk : model->hunks) for (int i=0;i<hunk.rightCount;++i) {
            const int line=hunk.right+i;
            highlights.append({line,0,int(GitDiff::displayText(model->right[line]).size())});
        }
        editor->setDiffHighlights(highlights);
    };
    QObject::connect(editor,&EditorWidget::contentChanged,view,&GitDiffView::documentChanged);
    QObject::connect(editor,&EditorWidget::diffRowsInserted,view,&GitDiffView::insertPadding);
    view->displayVersions(snapshot,live,editor);
    auto* old=view->findChild<GitDiffSourcePane*>("gitDiffOld");
    auto* shared=view->findChild<QScrollBar*>("gitDiffScroll");
    auto* overview=view->findChild<QWidget*>("gitDiffOverview");
    assert(waitFor([&] { return editor->visualRowForLine(21)==41; }));
    assert(old && shared && overview && view->isAncestorOf(editor));
    assert(old->viewport()->mapToGlobal(QPoint{}).y()==editor->viewport()->mapToGlobal(QPoint{}).y());
    assert(!old->verticalScrollBar()->isVisible() && !editor->verticalScrollBar()->isVisible() && shared->isVisible());
    assert(shared->mapToGlobal(QPoint{}).x()>editor->mapToGlobal(QPoint(editor->width()-1,0)).x());
    const auto rowTop=[&](int row) { return old->rowRect(row).top(); };
    const int firstTextTop=rowTop(0)+old->verticalScrollBar()->value();
    assert(firstTextTop==0);
    const auto marks=overview->grab().toImage();
    const auto markerColor=[&](int row) { return marks.pixelColor(marks.width()-2,qRound(row*marks.height()/122.0)); };
    const auto& theme=ThemeManager::instance();
    assert(markerColor(0).green()>markerColor(0).red());
    assert(markerColor(30)==theme.color("diagnostic.error"));
    assert(markerColor(101)==theme.color("accent"));
    QTest::mouseClick(overview,Qt::LeftButton,Qt::NoModifier,QPoint(3,overview->height()*101/122));
    assert(shared->value()>60 && shared->value()==editor->verticalScrollBar()->value());
    shared->setValue(shared->maximum());
    const int bottom=editor->verticalScrollBar()->value();
    assert(bottom==shared->maximum());
    assert(rowTop(bottom)==firstTextTop);
    editor->verticalScrollBar()->setValue(25); QApplication::processEvents();
    assert(shared->value()==25);
    assert(rowTop(25)==firstTextTop);
    const QPoint wheelPos(60,50);
    QWheelEvent wheel(wheelPos,old->viewport()->mapToGlobal(wheelPos),{},QPoint(0,-120),
                      Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(old->viewport(),&wheel);
    assert(shared->value()>25 && shared->value()==editor->verticalScrollBar()->value());
    const int afterOldWheel=shared->value();
    QWheelEvent nativeWheel(wheelPos,editor->viewport()->mapToGlobal(wheelPos),{},QPoint(0,-120),
                            Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(editor->viewport(),&nativeWheel);
    assert(shared->value()>afterOldWheel && shared->value()==editor->verticalScrollBar()->value());
    const int target=50;
    const int y=rowTop(target)+old->verticalScrollBar()->value()-firstTextTop;
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
    // A new row before the first pair shifts both maps before the worker runs.
    editor->setNavigationPosition(1,1);
    previous=alignmentUpdates;
    QTest::keyClick(editor,Qt::Key_Return);
    assert(alignmentUpdates==previous);
    assert(old->lineAtRow(0)==-1 && old->lineAtRow(1)==-1 && old->lineAtRow(2)==0);
    assert(editor->visualRowForLine(2)==2);
    waitComparison(previous);
    assert(old->lineAtRow(2)==0);
    previous=alignmentUpdates;
    editor->documentUndo();
    assert(alignmentUpdates==previous);
    assert(old->lineAtRow(2)==0 && editor->visualRowForLine(1)==2);
    waitComparison(previous);
    assert(old->lineAtRow(1)==0 && editor->visualRowForLine(1)==1);
    // Cursor reveal and bottom scrolling use visual rows as well.
    editor->goToLine(100); QApplication::processEvents();
    const int cursorY=editor->viewport()->mapFromGlobal(editor->cursorScreenPosition()).y();
    assert(cursorY>=0 && cursorY<=editor->viewport()->height());
    QTest::mouseClick(old->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(60,50));
    assert(old->hasFocus());
    view->hide(); assert(editor->visualRowForLine(21)==21);
    assert(editor->hasFocus());
    assert(editor->parentWidget()==&host && editor->verticalScrollBarPolicy()==Qt::ScrollBarAsNeeded);
}
static void gitWindowTests(QTemporaryDir& directory) {
    assert(I18n::saveLanguage("ru")); assert(I18n::initialize()); ThemeManager::instance().initialize();
    diffScrollAlignmentTests();
    if (QStandardPaths::findExecutable("git").isEmpty()) return;
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null"); qputenv("GIT_CONFIG_NOSYSTEM", "1");
    const auto root = directory.filePath("repository"); assert(QDir().mkpath(root));
    const auto command = [&](const QStringList& args, const QByteArray& input = {}) {
        QProcess process; process.setWorkingDirectory(root); process.start("git", args);
        if (!input.isEmpty()) {
            assert(process.waitForStarted());
            process.write(input);
            process.closeWriteChannel();
        }
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
    MainWindow window; window.resize(1250, 780); window.openPaths({root});
    auto* panel = window.findChild<GitPanel*>(); assert(panel);
    assert(waitFor([&] { return !panel->service()->busy() && panel->service()->repository() == root; }));
    auto* badge = window.findChild<QLabel*>("gitCount");
    assert(badge && badge->text() == "1" && !badge->isHidden());
    assert(!panel->isUiInitialized());
    window.show();
    auto* button = window.findChild<QToolButton*>("gitActivity"); assert(button); button->click();
    assert(panel->isUiInitialized());
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
    auto* editor = window.findChild<EditorWidget*>(); assert(editor);
    auto* patchPreview=diff->findChild<QPlainTextEdit*>("gitDiffText"); assert(patchPreview);
    const auto diffPixel=[&](int row=1) {
        const int height=editor->fontMetrics().height()+3;
        return editor->viewport()->grab().toImage().pixelColor(editor->viewport()->width()-20,row*height+height/2);
    };
    const auto hasChange=[&](int row=1) {
        const auto color=diffPixel(row); return color.green()>color.red()+10;
    };
    assert(waitFor([&] { return !editor->isBusy(); }));
    assert(waitFor([&] { return hasChange(); }));
    assert(!patchPreview->isVisible() && patchPreview->toPlainText().isEmpty());
    const auto saveWithShortcut=[&] {
        window.activateWindow(); editor->setFocus(); QApplication::processEvents();
        QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);
    };
    if (qEnvironmentVariableIsSet("TURTLE_CODE_GIT_PREVIEW")) {
        QApplication::processEvents(); assert(window.grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_PREVIEW")));
    }
    assert(diff->nativeEditor()==editor);
    assert(diff->isAncestorOf(editor));
    assert(!qobject_cast<QDockWidget*>(diff));
    assert(editor->findChild<QWidget*>("editorMiniMap")->isHidden());
    assert(diff->mapToGlobal(QPoint(0,0)).x() < editor->mapToGlobal(QPoint(0,0)).x());
    auto* closeDiff=diff->findChild<QToolButton*>("gitDiffClose");
    assert(closeDiff && closeDiff->isVisible());
    assert(closeDiff->mapTo(diff,closeDiff->rect().topRight()).x()>=diff->width()-6);
    window.activateWindow();
    editor->setFocus();
    QTest::mouseClick(closeDiff,Qt::LeftButton);
    assert(diff->isHidden() && editor->isVisible() && !diff->isAncestorOf(editor));
    assert(editor->hasFocus());
    assert(editor->findChild<QWidget*>("editorMiniMap")->isVisible()==editor->miniMapEnabled());
    assert(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,0)));
    assert(waitFor([&] { return !panel->service()->busy() && diff->isVisible(); }));
    assert(diff->nativeEditor()==editor);
    assert(window.findChildren<EditorWidget*>().size()==1);
    auto* original=diff->findChild<GitDiffSourcePane*>("gitDiffOld"); assert(original);
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
    assert(waitFor([&] { return original->lineAtRow(0)==-1 && original->lineAtRow(1)==0; }));
    assert(editor->visualRowForLine(1)==1);
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    if(qEnvironmentVariableIsSet("TURTLE_CODE_GIT_ALIGNMENT_PREVIEW")) {
        QApplication::processEvents(); assert(window.grab().save(qEnvironmentVariable("TURTLE_CODE_GIT_ALIGNMENT_PREVIEW")));
    }
    editor->documentUndo();
    assert(waitFor([&] { return original->lineAtRow(0)==0; }));
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
    assert(waitFor([&] { return editor->visualRowForLine(1)==1 && hasChange(); }));
    assert(!editor->isModified());
    // Hiding the comparison removes its spacers from the editor.
    replaceForAlignment("int main() {\n}\n");
    assert(waitFor([&] { return editor->visualRowForLine(1)==2; }));
    diff->hide(); assert(editor->visualRowForLine(1)==1);
    editor->documentUndo();
    assert(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,0)));
    assert(waitFor([&] { return diff->isVisible() && !panel->service()->busy() && hasChange(); }));
    // A quick save can complete between the diff's availability polls. Its
    // unchanged-content notification must preserve the actual painted ranges.
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
    assert(waitFor([&] { return !hasChange(); }));
    assert(original->viewport()->grab().toImage().pixelColor(original->viewport()->width()-10,
        original->rowRect(1).center().y())==ThemeManager::instance().color("editor.background"));
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    editor->setNavigationPosition(2,12); QTest::keyClick(editor,Qt::Key_Delete);
    QTest::keyClick(editor,Qt::Key_4); QTest::keyClick(editor,Qt::Key_2);
    assert(editor->document()->buffer().text()=="int main() {\n    return 42;\n}\n");
    assert(waitFor([&] { return hasChange(); }));
    saveWithShortcut(); assert(waitFor([&] { return !editor->isBusy() && !editor->isModified(); }));
    editor->setFocus(); editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_Z);
    assert(editor->isModified()); assert(editor->document()->buffer().text().starts_with("zint"));
    assert(read(path)=="int main() {\n    return 42;\n}\n");
    assert(waitFor([&] { return hasChange(0); }));
    QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);
    assert(waitFor([&] { return !editor->isModified(); }));
    assert(waitFor([&] { return !hasChange(0); }));
    editor->setNavigationPosition(1,1); QTest::keyClick(editor,Qt::Key_X); assert(editor->isModified());
    assert(waitFor([&] { return hasChange(0); }));
    row=tree->topLevelItem(1)->child(0); assert(row); tree->scrollToItem(row);
    const auto patchPoint=tree->visualItemRect(row).center();
    assert(QMetaObject::invokeMethod(tree,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,patchPoint)));
    auto* patchMenu=tree->findChild<QMenu*>("gitFileMenu"); assert(patchMenu && patchMenu->isVisible());
    auto* openPatch=patchMenu->findChild<QAction*>("gitFilePatch"); assert(openPatch && openPatch->isEnabled());
    openPatch->trigger(); patchMenu->close();
    assert(waitFor([&] { return patchPreview->isVisible() && patchPreview->toPlainText().contains("+    return 42;"); }));
    assert(!patchPreview->toPlainText().contains("+xint"));
    assert(editor->isModified() && editor->document()->buffer().text().starts_with("xint"));
    assert(QMetaObject::invokeMethod(tree,"itemClicked",Qt::DirectConnection,Q_ARG(QTreeWidgetItem*,row),Q_ARG(int,0)));
    assert(waitFor([&] { return diff->nativeEditor()==editor && !patchPreview->isVisible() && hasChange(0); }));
    assert(patchPreview->toPlainText().isEmpty());
    // File-tree Revert restores and saves the working file, preserves the index,
    // and remains undoable through the native editor.
    editor->setFocus();
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
    // The same file appears in both groups with different bases and one working buffer.
    const auto stagedText = read(path);
    editor->setNavigationPosition(2,1);
    QTest::keyClick(editor,Qt::Key_X);
    saveWithShortcut();
    assert(waitFor([&] {
        const auto entries = panel->service()->status().entries;
        return !editor->isBusy() && !panel->service()->busy() && entries.size() == 1 &&
               entries[0].staged() && entries[0].unstaged();
    }));
    const auto workingText = read(path);
    assert(workingText != stagedText);
    const auto headText = command({"show", "HEAD:hello.cpp"});
    const auto openFromGit = [&](int group, const QString& filename) {
        auto* item = tree->topLevelItem(group)->child(0);
        assert(item && item->text(0) == filename);
        assert(QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection,
                                         Q_ARG(QTreeWidgetItem*, item), Q_ARG(int, 0)));
    };
    const auto readSource = [&] {
        QTest::keyClick(original, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(original, Qt::Key_C, Qt::ControlModifier);
        return QApplication::clipboard()->text();
    };
    for (const bool staged : {true, false}) {
        openFromGit(staged ? 0 : 1, "hello.cpp");
        assert(waitFor([&] {
            return diff->isVisible() && diff->staged() == staged && diff->nativeEditor() == editor &&
                   editor->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff;
        }));
        assert(readSource() == QString::fromUtf8(staged ? headText : stagedText));
        assert(editor->document()->buffer().text() == workingText.toStdString());
        assert(command({"show", ":hello.cpp"}) == stagedText);
    }
    editor->documentUndo();
    saveWithShortcut();
    assert(waitFor([&] { return !editor->isBusy() && !editor->isModified() && !panel->service()->busy(); }));
    assert(read(path) == stagedText);
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
    assert(waitFor([&] { return hasChange(0); }));
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

    // Opening Git from an existing editor split wraps only the active pane.
    auto* splitRight=window.findChild<QAction*>("editor.splitRight"); assert(splitRight && splitRight->isEnabled());
    splitRight->trigger();
    auto* workspace=window.findChild<EditorPaneWorkspace*>();
    assert(workspace && workspace->panes().size()==2);
    auto* activePane=workspace->activePane();
    auto* liveView=activePane->editor();
    auto* sibling=workspace->panes().first();
    assert(liveView && sibling!=activePane && sibling->editor()->isVisible());
    auto* editorHost=activePane->viewContainer();
    panel->service()->diff(discardEntry,false);
    assert(waitFor([&] { return diff->isVisible() && diff->nativeEditor()==liveView &&
                                liveView->verticalScrollBarPolicy()==Qt::ScrollBarAlwaysOff; }));
    assert(diff->parentWidget()==activePane && diff->isAncestorOf(editorHost));
    assert(sibling->editor()->isVisible() && !diff->isAncestorOf(sibling));
    window.activateWindow();
    QApplication::processEvents();
    QTest::mouseClick(sibling->editor()->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(55,12));
    assert(waitFor([&] { return diff->isHidden() && workspace->activePane()==sibling; }));
    assert(editorHost->parentWidget()==activePane && liveView->isVisible());

    const auto conflictPath = root + "/conflict.txt";
    const auto hash = [&](const QByteArray& value) { return command({"hash-object", "-w", "--stdin"}, value).trimmed(); };
    const QByteArray ours = "ours\r\ncommon\r\n";
    const QByteArray markers = "<<<<<<< HEAD\r\nours\r\n=======\r\ntheirs\r\n>>>>>>> side\r\ncommon\r\n";
    command({"update-index", "--index-info"},
            "100644 " + hash("base\r\ncommon\r\n") + " 1\tconflict.txt\n" +
            "100644 " + hash(ours) + " 2\tconflict.txt\n" +
            "100644 " + hash("theirs\r\ncommon\r\n") + " 3\tconflict.txt\n");
    write(conflictPath, markers);
    panel->service()->refresh();
    assert(waitFor([&] {
        return !panel->service()->busy() && !panel->service()->status().entries.isEmpty() &&
               panel->service()->status().entries[0].conflict();
    }));
    openFromGit(3, "conflict.txt");
    assert(waitFor([&] {
        return diff->isVisible() && diff->path() == conflictPath && diff->nativeEditor() &&
               original->lineAtRow(1) == 0 && original->lineAtRow(5) == 1;
    }));
    assert(readSource() == GitDiff::displayText(ours));
    auto* conflictEditor = qobject_cast<EditorWidget*>(diff->nativeEditor());
    assert(conflictEditor && conflictEditor->document()->buffer().text() == markers.toStdString());
    assert(read(conflictPath) == markers);
    window.close(); assert(waitFor([&] { return !window.isVisible(); }));
}
