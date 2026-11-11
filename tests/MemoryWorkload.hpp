#pragma once

#include "app/MainWindow.hpp"
#include "editor/EditorWidget.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QSettings>
#include <QTabWidget>
#include <QTest>
#include <QClipboard>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QPointer>
#include <cstdio>
#include <stdexcept>

// Runs inside the real MainWindow test process; the external runner measures /proc.
static int memoryWorkload(int argc, char** argv) {
    const auto require=[](bool ok,const char* message) {
        if(!ok) throw std::runtime_error(message);
    };
    try {
        require(argc==4,"Expected --memory-workload FILE CYCLES");
        bool valid=false;
        const int cycles=QString::fromLocal8Bit(argv[3]).toInt(&valid);
        require(valid && cycles>=3 && cycles<=100,"Invalid cycle count");
        const QString path=QString::fromLocal8Bit(argv[2]);
        const auto originalSize=QFileInfo(path).size();
        require(originalSize>0,"Missing fixture");
        QSettings().setValue("editor/recoveryEnabled",false);
        MainWindow window;window.resize(1000,700);window.show();
        auto* tabs=window.findChild<QTabWidget*>();require(tabs,"Missing tabs");
        const auto stage=[](int cycle,const char* name) {
            const auto line=QJsonDocument(QJsonObject{{"cycle",cycle},{"stage",QString::fromLatin1(name)}}).toJson(QJsonDocument::Compact);
            std::fwrite(line.constData(),1,static_cast<size_t>(line.size()),stdout);
            std::fputc('\n',stdout);std::fflush(stdout);
            // Keep the stage alive for the external memory sampler.
            QTest::qWait(200);
        };
        const auto idle=[&](EditorWidget* editor) {
            QElapsedTimer timer;timer.start();
            while(editor->isBusy() && timer.elapsed()<120000) QTest::qWait(10);
            require(!editor->isBusy(),"Timed out waiting for editor operation");
        };
        QTest::qWait(500);stage(0,"baseline");
        for(int cycle=1;cycle<=cycles;++cycle) {
            window.openPaths({path});
            require(tabs->count()==1,"Expected one open document");
            auto* editor=qobject_cast<EditorWidget*>(tabs->currentWidget());require(editor,"Missing editor");
            idle(editor);require(editor->byteSize()==static_cast<size_t>(originalSize),"Wrong loaded size");
            editor->setFocus();stage(cycle,"opened");
            QApplication::clipboard()->setText(QString(1024*1024,QLatin1Char('x'))+"\n");
            editor->pasteClipboard();idle(editor);
            const auto pastedSize=static_cast<size_t>(originalSize)+1024*1024+1;
            require(editor->byteSize()==pastedSize,"Paste failed");stage(cycle,"pasted");
            QTest::keyClick(editor,Qt::Key_A,Qt::ControlModifier);
            QTest::keyClick(editor,Qt::Key_Delete);idle(editor);
            require(editor->byteSize()==0,"Delete failed");stage(cycle,"deleted");
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);idle(editor);
            require(editor->byteSize()==pastedSize,"Undo delete failed");stage(cycle,"undo");
            QTest::keyClick(editor,Qt::Key_Y,Qt::ControlModifier);idle(editor);
            require(editor->byteSize()==0,"Redo delete failed");stage(cycle,"redo");
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);idle(editor);
            QTest::keyClick(editor,Qt::Key_Z,Qt::ControlModifier);idle(editor);
            require(editor->byteSize()==static_cast<size_t>(originalSize) && !editor->isModified(),"Undo did not restore saved state");
            QPointer<EditorWidget> weakEditor=editor;
            std::weak_ptr<Document> weakDocument=editor->document();
            tabs->tabCloseRequested(tabs->currentIndex());
            QApplication::clipboard()->clear();
            QElapsedTimer timer;timer.start();
            while((weakEditor || !weakDocument.expired()) && timer.elapsed()<30000) {
                QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QTest::qWait(20);
            }
            require(tabs->count()==0 && !weakEditor && weakDocument.expired(),"Closed document still owned after 30 seconds");
            QTest::qWait(1000);stage(cycle,"closed");
        }
        window.close();return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Memory workload failed: %s\n",error.what());return 1;
    }
}
