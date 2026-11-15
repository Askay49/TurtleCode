#pragma once
#include "app/CommandPalette.hpp"
#include <QInputMethodQueryEvent>
#include "VimExWindowTests.hpp"
#include "VimExExtendedWindowTests.hpp"
#include "VimNormalWindowTests.hpp"
#include "VimSearchWindowTests.hpp"
static void vimWindowTests(QTemporaryDir& directory) {
    using Mode=VimController::Mode;
    QSettings().setValue("editor/vimMode",false);QSettings().setValue("editor/recoveryEnabled",false);
    ThemeManager::instance().initialize();
    const auto path=directory.filePath("vim.txt");{QFile file(path);assert(file.open(QIODevice::WriteOnly));file.write("alpha beta\ngamma");}
    MainWindow first,second;first.resize(1100,720);first.show();second.show();first.openPaths({path});second.openPaths({path});
    auto* editor=first.findChild<EditorWidget*>();auto* other=second.findChild<EditorWidget*>();assert(editor && other);
    assert(waitFor([&]{return !editor->isBusy() && !other->isBusy();}));
    auto* toggle=first.findChild<QAction*>("settings.vimMode");auto* otherToggle=second.findChild<QAction*>("settings.vimMode");
    auto* indicator=first.findChild<QLabel*>("vimModeIndicator");assert(toggle && otherToggle && indicator);
    assert(!editor->vimEnabled() && indicator->isHidden());
    first.activateWindow();editor->setFocus();QTest::qWait(30);toggle->trigger();
    assert(editor->vimEnabled() && other->vimEnabled() && otherToggle->isChecked());
    assert(QSettings().value("editor/vimMode").toBool());
    assert(indicator->text()=="NORMAL" && !indicator->isHidden());
    QInputMethodQueryEvent normalCaret(Qt::ImCursorRectangle);QApplication::sendEvent(editor,&normalCaret);
    assert(normalCaret.value(Qt::ImCursorRectangle).toRect().width()>2);
    first.grab().save("/tmp/turtle-vim-normal.png");
    QTest::keyClick(editor,Qt::Key_I);assert(editor->vimMode()==Mode::Insert && indicator->text()=="INSERT");
    QInputMethodQueryEvent insertCaret(Qt::ImCursorRectangle);QApplication::sendEvent(editor,&insertCaret);
    assert(insertCaret.value(Qt::ImCursorRectangle).toRect().width()==2);
    QTest::keyClicks(editor,"Z");assert(editor->document()->buffer().text().starts_with("Zalpha"));
    first.grab().save("/tmp/turtle-vim-insert.png");
    second.activateWindow();other->setFocus();QTest::qWait(30);
    assert(editor->vimMode()==Mode::Insert && other->vimMode()==Mode::Normal && !indicator->isEnabled());
    first.activateWindow();editor->setFocus();QTest::qWait(30);assert(indicator->text()=="INSERT" && indicator->isEnabled());
    QTimer::singleShot(0,[&]{auto* palette=dynamic_cast<CommandPalette*>(QApplication::activeModalWidget());assert(palette);palette->reject();});
    QTest::keyClick(editor,Qt::Key_P,Qt::ControlModifier|Qt::ShiftModifier);
    assert(editor->vimMode()==Mode::Insert);
    auto* settings=qobject_cast<QAction*>(ShortcutManager::instance().target("settings.editor",&first,editor));assert(settings);
    QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);auto* box=dialog->findChild<QCheckBox*>("vimModeEnabled");assert(box && box->isChecked());box->setChecked(false);dialog->reject();});
    settings->trigger();assert(editor->vimEnabled() && editor->vimMode()==Mode::Insert);
    QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());assert(dialog);dialog->findChild<QCheckBox*>("vimModeEnabled")->setChecked(false);auto* buttons=dialog->findChild<QDialogButtonBox*>();assert(buttons);buttons->button(QDialogButtonBox::Save)->click();});
    settings->trigger();assert(!editor->vimEnabled() && !other->vimEnabled() && !toggle->isChecked() && indicator->isHidden());
    first.activateWindow();editor->setFocus();QTest::qWait(30);
    QTimer::singleShot(0,[&]{auto* palette=dynamic_cast<CommandPalette*>(QApplication::activeModalWidget());assert(palette);auto* input=palette->findChild<QLineEdit*>("commandPaletteInput");assert(input);input->setText("settings.vimMode");QTest::keyClick(input,Qt::Key_Return);});
    QTest::keyClick(editor,Qt::Key_P,Qt::ControlModifier|Qt::ShiftModifier);
    assert(editor->vimEnabled() && other->vimEnabled() && editor->vimMode()==Mode::Normal);
    QTest::keyClick(editor,Qt::Key_V);assert(editor->vimMode()==Mode::Visual && !editor->selectedText().isEmpty() && indicator->text()=="VISUAL");
    first.activateWindow();editor->setFocus();QTest::qWait(30);
    QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);
    assert(waitFor([&]{return !editor->isBusy() && !editor->isModified();}));
    assert(editor->vimMode()==Mode::Visual && !editor->selectedText().isEmpty());
    // Native terminal input keeps its own scope while Vim is enabled.
    first.findChild<QAction*>("view.terminal")->trigger();auto* terminal=first.findChild<TerminalPanel*>()->terminal();
    terminal->setFocus();QTest::qWait(20);assert(!indicator->isEnabled());
    QTest::keyClicks(terminal,"echo vim-test");QTest::keyClick(terminal,Qt::Key_Return);
    assert(editor->vimMode()==Mode::Visual && !editor->isModified());
    first.activateWindow();editor->setFocus();QTest::qWait(30);QTest::keyClick(editor,Qt::Key_Escape);
    editor->setNavigationPosition(1,1);const auto original=editor->document()->buffer().text();
    QTest::keyClicks(editor,"ciw");assert(editor->vimMode()==Mode::Insert && indicator->text()=="INSERT");
    QTest::keyClicks(editor,"modal");QTest::keyClick(editor,Qt::Key_Escape);QTest::keyClick(editor,Qt::Key_U);
    assert(editor->document()->buffer().text()==original && !editor->isModified());
    QTest::keyClick(editor,Qt::Key_R,Qt::ControlModifier);assert(editor->document()->buffer().text()!=original && editor->vimMode()==Mode::Normal);
    QTest::keyClick(editor,Qt::Key_U);assert(editor->document()->buffer().text()==original && !editor->isModified());
    QTest::keyClick(editor,Qt::Key_D);assert(indicator->text().contains("d"));
    editor->showFindBar();auto* find=editor->findChild<QLineEdit*>("fileFindInput");assert(find);
    QTest::keyClicks(find,"word");QTest::keyClick(find,Qt::Key_Escape);QTest::keyClick(editor,Qt::Key_W);
    assert(editor->document()->buffer().text()==original && editor->vimMode()==Mode::Normal && indicator->text()=="NORMAL");
    // Shared state and cross-file navigation use the real tab/focus routing.
    VimState::Session::instance().clear();editor->setNavigationPosition(1,2);QTest::keyClicks(editor,"mA\"ayiw");
    auto* tabs=first.findChild<QTabWidget*>();assert(tabs);
    const auto betaPath=directory.filePath("vim-beta.txt");{QFile file(betaPath);assert(file.open(QIODevice::WriteOnly));file.write("beta\nsecond");}
    first.openPaths({betaPath});auto* beta=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(beta && beta!=editor);
    assert(waitFor([&]{return !beta->isBusy();}));beta->setFocus();QTest::qWait(20);
    QTest::keyClicks(beta,"\"aP");assert(waitFor([&]{return !beta->isBusy();}));assert(beta->byteSize()>11);QTest::keyClick(beta,Qt::Key_U);assert(waitFor([&]{return !beta->isBusy();}));
    QTest::keyClicks(beta,"`A");assert(tabs->currentWidget()==editor && editor->navigationPosition()==QPoint(1,2));
    QTest::keyClick(editor,Qt::Key_O,Qt::ControlModifier);assert(tabs->currentWidget()==beta);
    beta->setNavigationPosition(2,2);QTest::keyClicks(beta,"mB");
    QPointer<EditorWidget> closed=beta;QMetaObject::invokeMethod(tabs,"tabCloseRequested",Qt::DirectConnection,Q_ARG(int,tabs->indexOf(beta)));QTest::qWait(20);assert(waitFor([&]{return closed.isNull();}));
    editor->setFocus();QTest::keyClicks(editor,"`B");beta=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(beta && beta!=editor);
    assert(waitFor([&]{return !beta->isBusy();}));assert(beta->navigationPosition()==QPoint(2,2));
    QTest::keyClicks(beta,"qc`A");assert(tabs->currentWidget()==editor);QTest::keyClicks(editor,"i!");QTest::keyClick(editor,Qt::Key_Escape);QTest::keyClick(editor,Qt::Key_Q);QTest::keyClick(editor,Qt::Key_U);assert(editor->document()->buffer().text()==original);
    tabs->setCurrentWidget(beta);beta->setFocus();QTest::qWait(20);QTest::keyClicks(beta,"@c");
    assert(waitFor([&]{for(auto* item:first.findChildren<EditorWidget*>())if(item->isBusy() || item->isVimReplaying())return false;return true;}));
    assert(tabs->currentWidget()==editor && editor->document()->buffer().text()!=original);QTest::keyClick(editor,Qt::Key_U);assert(editor->document()->buffer().text()==original);
    QTest::keyClicks(editor,"qf`B");assert(tabs->currentWidget()==beta);QTest::keyClicks(beta,"i?");QTest::keyClick(beta,Qt::Key_Escape);QTest::keyClick(beta,Qt::Key_Q);QTest::keyClick(beta,Qt::Key_U);
    closed=beta;QMetaObject::invokeMethod(tabs,"tabCloseRequested",Qt::DirectConnection,Q_ARG(int,tabs->indexOf(beta)));QTest::qWait(20);assert(closed.isNull());
    editor->setFocus();QTest::keyClicks(editor,"@f");assert(waitFor([&]{for(auto* item:first.findChildren<EditorWidget*>())if(item->isBusy() || item->isVimReplaying())return false;return true;}));
    beta=qobject_cast<EditorWidget*>(tabs->currentWidget());assert(beta && beta!=editor && beta->document()->buffer().text()=="beta\ns?econd");QTest::keyClick(beta,Qt::Key_U);assert(beta->document()->buffer().text()=="beta\nsecond");
    tabs->setCurrentWidget(editor);editor->setFocus();QTest::qWait(20);
    QTest::keyClick(editor,Qt::Key_V,Qt::ControlModifier);QTest::keyClick(editor,Qt::Key_J);assert(indicator->text()=="V-BLOCK");
    const auto rectangle=editor->viewport()->grab().toImage();assert(rectangle.pixelColor(rectangle.width()/2,normalCaret.value(Qt::ImCursorRectangle).toRect().height()/2)==ThemeManager::instance().color("editor.background"));
    first.grab().save("/tmp/turtle-vim-block.png");QTest::keyClick(editor,Qt::Key_Escape);
    QTest::keyClicks(editor,"R");assert(editor->vimMode()==Mode::Replace && indicator->text()=="REPLACE");first.grab().save("/tmp/turtle-vim-replace.png");QTest::keyClick(editor,Qt::Key_Escape);
    QTest::keyClicks(editor,"qd");assert(indicator->text().contains("recording @d"));QTest::keyClick(editor,Qt::Key_Q);
    vimExWindowTests(first,editor,directory);
    vimSearchWindowTests(first,editor);
    vimExExtendedWindowTests(first,editor,directory);
    vimNormalWindowTests(first,editor);
    MainWindow restored;restored.show();restored.openPaths({path});auto* fresh=restored.findChild<EditorWidget*>();assert(fresh);
    assert(waitFor([&]{return !fresh->isBusy();}));assert(fresh->vimEnabled() && fresh->vimMode()==Mode::Normal);
    closeDiscard(restored);closeDiscard(first);closeDiscard(second);
}
