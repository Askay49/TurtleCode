#include "MainWindow.hpp"
#include "editor/EditorPaneWorkspace.hpp"
#include "editor/EditorWidget.hpp"
#include "document/FileState.hpp"
#include "git/GitPanel.hpp"
#include "app/I18n.hpp"
#include "app/IconDialogs.hpp"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QTabWidget>
#include <QStatusBar>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>

void MainWindow::executeVimEx(EditorWidget* editor,const VimEx::Invocation& command,std::function<void(QString)> completed){
    if(!editor || editor->isBusy() || closing_){completed(I18n::text("Vim: the document is busy"));return;}
    if (executeVimWorkspaceEx(editor, command, completed)) return;
    const auto name = command.definition.name;
    auto* primary = documentEditor(editor);
    auto* alternateEditor = alternateBuffer(editor);
    const auto alternate = alternateEditor ? alternateEditor->filePath() : QString{};
    const auto activate = [this, editor, completed](EditorWidget* target) {
        if (!target) { completed(QStringLiteral("E86: Buffer does not exist")); return; }
        const auto* primary = documentEditor(target);
        if (target != editor && (target->vimCommandExecuting() ||
            (target->isBusy() && primary && !primary->property("openingPath").isValid()))) {
            completed(I18n::text("Vim: the document is busy")); return;
        }
        target = activateEditor(target, true);
        if (!target) { completed(QStringLiteral("E86: Buffer does not exist")); return; }
        target->resetVimMode();
        editor->transferVimEx(target);
        completed({});
    };
    if (name == "buffers" || name == "ls" || name == "files") {
        QString output;
        for (int i = 0; i < tabs_->count(); ++i) if (auto* open = qobject_cast<EditorWidget*>(tabs_->widget(i))) {
            bool displayed = open == primary;
            for (auto* pane : paneWorkspace_->panes(true))
                if (pane->editor() && pane->editor()->document() == open->document()) { displayed = true; break; }
            output += QStringLiteral("%1 %2%3%4 %5\n").arg(open->property("vimBufferId").toULongLong())
                .arg(open == primary ? "%" : open == alternateEditor ? "#" : " ", displayed ? "a" : "h", open->isModified() ? "+" : " ",
                     open->filePath().isEmpty() ? I18n::text("Untitled") : open->filePath());
            if (output.size() > 4096) break;
        }
        if (command.bang) for (const auto& unloaded : unloadedVimBuffers_) {
            output += QStringLiteral("%1  u  %2\n").arg(unloaded.id)
                .arg(unloaded.path.isEmpty() ? I18n::text("Untitled") : unloaded.path);
            if (output.size() > 4096) break;
        }
        editor->reportVimExMessage(output.left(4096));
        completed({});
        return;
    }
    const bool deletion = name == "bdelete" || name == "bwipeout";
    if (deletion || name == "buffer" || name == "bnext" || name == "bprevious" ||
        name == "bNext" || name == "bfirst" || name == "blast") {
        EditorWidget* target = nullptr;
        auto unloaded = unloadedVimBuffers_.end();
        const auto argument = command.arguments.trimmed();
        if (name == "bnext" || name == "bprevious" || name == "bNext") {
            const int size = tabs_->count();
            if (size) target = qobject_cast<EditorWidget*>(tabs_->widget((tabs_->indexOf(primary) + (name == "bnext" ? 1 : size - 1)) % size));
        } else if (name == "bfirst" || name == "blast") {
            target = qobject_cast<EditorWidget*>(tabs_->widget(name == "bfirst" ? 0 : tabs_->count() - 1));
        } else if (argument.isEmpty() || argument == "%") {
            target = primary;
        } else if (argument == "#") {
            target = alternateEditor;
        } else {
            bool number = false;
            const auto id = argument.toULongLong(&number);
            QString error;
            const auto path = number ? QString{} : VimEx::fileArgument(argument, editor->filePath(), &error, alternate);
            if (!error.isEmpty()) { completed(error); return; }
            int matches = 0;
            for (int i = 0; i < tabs_->count(); ++i) if (auto* open = qobject_cast<EditorWidget*>(tabs_->widget(i))) {
                const bool exact = number ? open->property("vimBufferId").toULongLong() == id : open->filePath() == path;
                if (exact) { target = open; matches = 1; break; }
                if (!number && open->filePath().contains(argument)) { target = open; ++matches; }
            }
            if (matches > 1) { completed(QStringLiteral("E93: More than one match for buffer")); return; }
            if (!target) {
                unloaded = std::find_if(unloadedVimBuffers_.begin(), unloadedVimBuffers_.end(), [&](const auto& buffer) {
                    return number ? buffer.id == id : buffer.path == path;
                });
            }
        }
        if (unloaded != unloadedVimBuffers_.end()) {
            if (name == "bwipeout") {
                VimState::Session::instance().discardBufferMarks(nullptr, unloaded->path, unloaded->path);
                unloadedVimBuffers_.erase(unloaded);
                completed({});
            } else if (name == "bdelete") {
                completed({});
            } else if (unloaded->path.isEmpty()) {
                const auto id = unloaded->id;
                unloadedVimBuffers_.erase(unloaded);
                auto* fresh = createUntitledEditor();
                fresh->setProperty("vimBufferId", qulonglong(id));
                activate(fresh);
            } else {
                const auto path = unloaded->path;
                activate(openVimFile(path));
            }
            return;
        }
        if (!target) { completed(QStringLiteral("E86: Buffer does not exist")); return; }
        if (deletion) {
            if (target->isBusy() || renameInProgress_ || !replaceLockedEditors_.isEmpty()) {
                completed(I18n::text("Vim: the document is busy")); return;
            }
            if (target->isModified() && !command.bang) {
                completed(QStringLiteral("E89: No write since last change (add ! to override)")); return;
            }
            if (target == primary) {
                hideEditorView(editor);
                if (auto* next = activeEditor(); next && next->document() != editor->document()) editor->transferVimEx(next);
            }
            closingVimBuffer_ = target;
            wipeVimBuffer_ = name == "bwipeout";
            completed({});
            requestClose({target}, false, command.bang);
            return;
        }
        activate(target);
        return;
    }
    if (name == "enew") { activate(createUntitledEditor()); return; }
    if(name=="edit" || (name=="file" && !command.arguments.isEmpty())){
        QString error;auto path=VimEx::fileArgument(command.arguments,editor->filePath(),&error,alternate);if(!error.isEmpty()){completed(error);return;}
        if(path.isEmpty())path=editor->filePath();
        if(path.isEmpty()){completed("E32: No file name");return;}
        if(name=="file"){
            for(int i=0;i<tabs_->count();++i)if(auto* other=qobject_cast<EditorWidget*>(tabs_->widget(i));other && other!=primary && other->filePath()==path){completed("E95: Buffer with this name already exists");return;}
            if(path!=editor->filePath()){editor->updateFilePathAfterRename(path);editor->markDiskDiverged();tabs_->setTabText(tabs_->indexOf(primary),QFileInfo(path).fileName()+" •");tabs_->setTabToolTip(tabs_->indexOf(primary),path);updateWindowTitle();}completed({});return;
        }
        if(path==editor->filePath()){
            if(editor->isModified() && !command.bang){completed("E37: No write since last change (add ! to override)");return;}
            if(!QFileInfo::exists(path)){
                editor->setBusy(true);editor->setProperty("openingPath",path);editor->reloadBuffer(PieceTreeBuffer(),path);
                editor->setProperty("openingPath",QVariant{});editor->setBusy(false);completed({});return;
            }
            struct Loaded {std::shared_ptr<PieceTreeBuffer> buffer;QString error;QByteArray state;};
            editor->setBusy(true);editor->setProperty("openingPath",path);QPointer<EditorWidget> guard(editor);const auto cancelled=std::make_shared<std::atomic<bool>>(false);
            auto* watcher=new QFutureWatcher<Loaded>(this);
            connect(editor,&EditorWidget::vimExInterruptRequested,watcher,[cancelled]{cancelled->store(true);});
            connect(watcher,&QFutureWatcher<Loaded>::finished,this,[this,watcher,guard,path,completed]{const auto result=watcher->result();watcher->deleteLater();if(!guard)return;
                if(result.buffer){const auto position=guard->navigationPosition();guard->reloadBuffer(std::move(*result.buffer),path);guard->document()->diskState=result.state;guard->setNavigationPosition(position.x(),position.y());}
                guard->setProperty("openingPath",QVariant{});guard->setBusy(false);
                if (result.buffer) queueVimBufferAutocmd(guard, "BufReadPost");
                completed(result.error);
            });
            watcher->setFuture(QtConcurrent::run([path,cancelled]{Loaded result;result.buffer=std::make_shared<PieceTreeBuffer>();const auto before=diskFileState(path);std::string error;
                if(!result.buffer->loadFile(QFile::encodeName(path).toStdString(),&error,[cancelled]{return cancelled->load();})){result.buffer.reset();result.error=QString::fromStdString(error);}
                else if(diskFileState(path)!=before){result.buffer.reset();result.error=I18n::text("File changed while it was being loaded");}
                result.state=diskFileState(path);return result;}));return;
        }
        if (auto* target = openVimFile(path)) activate(target);
        else completed(QStringLiteral("E484: Cannot open file"));
        return;
    }
    const bool all=name=="qall";
    const bool close=name=="quit" || name=="close" || all;
    if(close){
        if(renameInProgress_ || !replaceLockedEditors_.isEmpty()){completed(I18n::text("Vim: the document is busy"));return;}
        if(all && gitPanel_ && gitPanel_->service()->writing()){completed(I18n::text("Wait for the Git operation to finish before closing."));return;}
        if (!all && closeEditorPane(editor)) {
            if (auto* next = activeEditor()) editor->transferVimEx(next);
            completed({});
            return;
        }
        QVector<QPointer<EditorWidget>> editors;
        if(all){for(int i=0;i<tabs_->count();++i)if(auto* open=qobject_cast<EditorWidget*>(tabs_->widget(i)))editors.append(open);}else editors.append(primary);
        for(const auto& open:editors)if(open && open->isModified() && !command.bang){completed("E37: No write since last change (add ! to override)");return;}
        for(const auto& open:editors)if(open && open->isBusy()){completed(I18n::text("Vim: the document is busy"));return;}
        // Complete before a successful close destroys its originating editor.
        editor->stopVimExecution();completed({});requestClose(editors,all,command.bang);return;
    }
    if(name!="write" && name!="update" && name!="wq" && name!="xit" && name!="saveas"){completed("E492: Not an editor command: "+name);return;}
    const bool exit=name=="wq" || name=="xit";
    QPointer<EditorWidget> guard(editor);
    const auto done=[this,guard,completed,exit](QString error){
        if(!error.isEmpty() || !guard){completed(error);return;}
        if (exit) {
            guard->stopVimExecution();
            if (guard->afterVimAutocmds([this, guard] {
                if (guard && !closeEditorPane(guard)) requestClose({documentEditor(guard)}, false, !guard->isModified());
            })) {
                completed({});
                return;
            }
        }
        completed({});if(exit && !closeEditorPane(guard))requestClose({documentEditor(guard)},false,true);
    };
    if((name=="update" || name=="xit") && !editor->isModified()){done({});return;}
    QString error;auto path=VimEx::fileArgument(command.arguments,editor->filePath(),&error,alternate);
    if(!error.isEmpty()){done(error);return;}
    if(path.isEmpty())path=editor->filePath();
    if(path.isEmpty()){done("E32: No file name");return;}
    if(command.arguments.trimmed().startsWith('!')){done(I18n::text("Vim: shell commands are not available yet"));return;}
    if(name=="saveas")for(int i=0;i<tabs_->count();++i)if(auto* other=qobject_cast<EditorWidget*>(tabs_->widget(i));other && other!=primary && other->filePath()==path){done("E95: Buffer with this name already exists");return;}
    const bool own=path==editor->filePath();
    const auto last=VimMotions::lastLine(editor->document()->buffer(),editor->document()->buffer().size() && editor->document()->buffer().byteAt(editor->byteSize()-1)=='\n')+1;
    const bool whole=name=="saveas" || (command.first==1 && command.last>=qint64(last));
    if(own && !whole && !command.bang){done("E140: Use ! to write partial buffer");return;}
    if(own && QFileInfo::exists(path) && !QFileInfo(path).isWritable() && !command.bang){done("E45: File is read-only (add ! to override)");return;}
    if(!own && QFileInfo::exists(path) && !command.bang){done("E13: File exists (add ! to override)");return;}
    if(own && diskFileState(path)!=editor->document()->diskState && !command.bang){done("E13: File changed on disk (add ! to override)");return;}
    const auto expected=diskFileState(path);
    if(whole && (own || editor->filePath().isEmpty() || name=="saveas")){
        editor->prepareEditorConfigSave(path,[this,guard,path,expected,done](bool ok){
            if(!ok || !guard){done(I18n::text("Vim: saving cancelled"));return;}
            writeEditor(guard,path,expected,[done](bool saved){done(saved?QString{}:I18n::text("Vim: write failed or was cancelled"));},false);
        });return;
    }
    // :write other-file and range writes do not rename or mark the source saved.
    const auto snapshot=std::make_shared<PieceTreeBuffer>(editor->snapshotBuffer());
    const auto begin=snapshot->lineStart(std::size_t(command.first-1));
    const auto end=std::size_t(command.last)<snapshot->lineCount()?snapshot->lineStart(std::size_t(command.last)):snapshot->size();
    const auto revision=editor->lspRevision();editor->setBusy(true);
    const auto cancelled=std::make_shared<std::atomic<bool>>(false);const auto percent=std::make_shared<std::atomic<int>>(0);
    auto* progress=new IconDialogs::ProgressDialog(I18n::text("Saving…"),I18n::text("Cancel"),0,100,this);
    progress->setWindowModality(Qt::NonModal);progress->setMinimumDuration(300);
    connect(progress,&QProgressDialog::canceled,progress,[cancelled]{cancelled->store(true);});
    connect(editor,&QObject::destroyed,progress,[cancelled]{cancelled->store(true);});
    connect(editor,&EditorWidget::vimExInterruptRequested,progress,&QProgressDialog::cancel);
    connect(editor,&EditorWidget::vimExInterruptRequested,progress,[cancelled]{cancelled->store(true);});
    auto* timer=new QTimer(progress);timer->setInterval(50);
    connect(timer,&QTimer::timeout,progress,[percent,progress,cancelled]{if(progress->wasCanceled())cancelled->store(true);else progress->setValue(percent->load());});timer->start();
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher,guard,path,own,revision,done,progress]{
        progress->deleteLater();
        const auto error=watcher->result();watcher->deleteLater();if(guard)guard->setBusy(false);
        if(error.isEmpty()){
            statusBar()->showMessage(I18n::text("Saved"),2000);
            if(guard && own && revision==guard->lspRevision()){guard->document()->diskState=diskFileState(path);guard->markDiskDiverged();}
        }
        done(error);
    });
    watcher->setFuture(QtConcurrent::run([snapshot,path,expected,begin,end,cancelled,percent]{
        if(diskFileState(path)!=expected)return QStringLiteral("E13: File changed while saving");
        QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return file.errorString();
        for(auto at=begin;at<end;at+=64*1024){
            if(cancelled->load()){file.cancelWriting();return I18n::text("Vim: saving cancelled");}
            const auto bytes=snapshot->slice(at,std::min<std::size_t>(64*1024,end-at));if(file.write(bytes.data(),qint64(bytes.size()))!=qint64(bytes.size()))return file.errorString();
            percent->store(int((at-begin)*99/std::max<std::size_t>(1,end-begin)));
        }
        if(cancelled->load()){file.cancelWriting();return I18n::text("Vim: saving cancelled");}
        if(diskFileState(path)!=expected){file.cancelWriting();return QStringLiteral("E13: File changed while saving");}
        return file.commit()?QString{}:file.errorString();
    }));
}
