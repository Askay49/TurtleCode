#include "editor/EditorWidget.hpp"
#include "app/ThemeManager.hpp"

#include <QApplication>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QRegion>
#include <QCompleter>
#include <QAbstractItemView>
#include <QTest>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QClipboard>
#include <QProgressDialog>
#include <QScrollBar>
#include <algorithm>
#include <QSettings>

namespace {
class PaintRecorder final : public QObject {
public:
    QRegion painted;
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Paint) painted += static_cast<QPaintEvent*>(event)->region();
        return QObject::eventFilter(object, event);
    }
};
void load(EditorWidget& editor, const char* text) {
    editor.loadBuffer(PieceTreeBuffer(text), QStringLiteral("sample.txt"));
    editor.resize(800, 400);
    editor.show();
    editor.setFocus();
    QApplication::processEvents();
}
}

#include "SmartEditingTests.hpp"

int main(int argc, char** argv) {
    QTemporaryDir smartSettings;
    const bool smart=argc>1 && QString::fromLocal8Bit(argv[1])=="--smart-editing";
    if(smart)qputenv("XDG_CONFIG_HOME",smartSettings.path().toUtf8());
    QApplication app(argc, argv);
    if(smart){app.setOrganizationName("TurtleCodeTests");app.setApplicationName("SmartEditing");smartEditingTests();return 0;}
    {
        EditorWidget editor;
        load(editor,"adc + value\nadc adc_state my_adc adc2 ADC (adc)\nvalue value\n");
        const auto original=editor.document()->buffer().text();
        const int height=editor.fontMetrics().height()+3;
        const auto pixel=[&](int row,const QString& prefix) {
            return editor.viewport()->grab().toImage().pixelColor(
                editor.lineNumberAreaWidth()+14+editor.fontMetrics().horizontalAdvance(prefix),row*height+height-3);
        };
        const auto highlight=QColor::fromRgb(ThemeManager::instance().color("editor.occurrence").rgb());
        editor.setNavigationPosition(1,2);
        if(qEnvironmentVariableIsSet("TURTLE_CODE_OCCURRENCE_SCREENSHOT"))
            editor.grab().save(qEnvironmentVariable("TURTLE_CODE_OCCURRENCE_SCREENSHOT"));
        Q_ASSERT(pixel(1,"")==highlight);
        Q_ASSERT(pixel(1,"adc adc_state my_adc adc2 ADC (")==highlight);
        for(const auto& prefix:QStringList{"adc ","adc adc_state my_","adc adc_state my_adc ","adc adc_state my_adc adc2 "})
            Q_ASSERT(pixel(1,prefix)!=highlight);
        Q_ASSERT(pixel(2,"")!=highlight);
        PaintRecorder recorder;editor.viewport()->installEventFilter(&recorder);
        QApplication::processEvents();recorder.painted=QRegion();
        QTest::keyClick(&editor,Qt::Key_End);QApplication::processEvents();
        Q_ASSERT(recorder.painted.contains(editor.viewport()->rect()));
        // Moving the caret changes and clears the decoration immediately.
        editor.setNavigationPosition(1,9);
        Q_ASSERT(pixel(1,"")!=highlight && pixel(2,"")==highlight);
        editor.setNavigationPosition(1,5); // '+'
        Q_ASSERT(pixel(1,"")!=highlight && pixel(2,"")!=highlight);
        // Explicit fragments follow the same substring rules as Ctrl+D.
        editor.revealMatch(2,5,3);
        Q_ASSERT(pixel(1,"adc adc_state my_")==highlight);
        editor.revealMatch(1,4,1); // whitespace-only selections do not highlight
        Q_ASSERT(pixel(1,"")!=highlight);
        editor.revealMatch(1,1,3);
        Q_ASSERT(pixel(1,"")==highlight);
        Q_ASSERT(editor.document()->buffer().text()==original && !editor.isModified());
        load(editor,"датчик +\nдатчик датчик_state\n");
        editor.setNavigationPosition(1,3);
        Q_ASSERT(pixel(1,"")==highlight && pixel(1,"датчик ")!=highlight);
        // Exceptional lines and large selections do not trigger full scans.
        editor.loadBuffer(PieceTreeBuffer(std::string(128*1024,'a')),"long.txt");
        Q_ASSERT(!editor.viewport()->grab().isNull());
    }
    {
        std::string text;
        for (int i=1;i<=300;++i)
            text += (i==80 || i==220) ? "needle\n" : "context\n";
        EditorWidget editor; load(editor,text.c_str());
        const auto checkCentered = [&](int line) {
            auto* scroll=editor.verticalScrollBar();
            const int expected=std::clamp(editor.visualRowForLine(line-1)-scroll->pageStep()/2,
                                          scroll->minimum(),scroll->maximum());
            Q_ASSERT(editor.navigationPosition().x()==line);
            Q_ASSERT(scroll->value()==expected);
        };
        // Explicit jumps center in both directions, including already visible
        // targets, while document boundaries clamp to the available context.
        editor.goToLine(220); checkCentered(220);
        editor.goToLine(80); checkCentered(80);
        editor.goToLine(81); checkCentered(81);
        editor.goToLine(1); checkCentered(1);
        editor.goToLine(300); checkCentered(300);
        editor.revealMatch(220,1,6); checkCentered(220);
        Q_ASSERT(editor.selectedText()=="needle");
        editor.revealMatch(80,1,6); checkCentered(80);
        // Diff spacer rows must be included in the centering calculation.
        QVector<int> rows;
        for(int i=0;i<300;++i){rows.append(-1);rows.append(i);}
        editor.setDiffAlignment(rows);
        editor.revealMatch(220,1,6); checkCentered(220);
        editor.setDiffAlignment({});
        editor.goToLine(1);
        editor.showFindBar();
        auto* query=editor.findChild<QLineEdit*>("fileFindInput");
        auto* count=editor.findChild<QLabel*>("fileFindCount");
        Q_ASSERT(query && count);
        query->setText("needle");
        QElapsedTimer timer;timer.start();
        while(count->text()!="0/2" && timer.elapsed()<5000) {
            QApplication::processEvents();QThread::msleep(1);
        }
        Q_ASSERT(count->text()=="0/2");
        editor.findNext(); checkCentered(80);
        editor.findNext(); checkCentered(220);
        editor.findNext(true); checkCentered(80);
        Q_ASSERT(editor.selectedText()=="needle");
        // Ordinary arrow movement does not recenter the viewport.
        const int top=editor.verticalScrollBar()->value();
        editor.setFocus();QTest::keyClick(&editor,Qt::Key_Down);
        Q_ASSERT(editor.verticalScrollBar()->value()==top);
    }
    {
        EditorWidget editor;
        std::string text;
        for (int i=0;i<2000;++i) {
            switch (i%8) {
            case 0: text+="void updateDocument(int index) {\n"; break;
            case 1: text+="    if (index >= 0) {\n"; break;
            case 2: text+="        refresh(index);\n"; break;
            case 3: text+="        repaint(); // refresh visible code\n"; break;
            case 4: text+="    }\n"; break;
            case 5: text+="    return;\n"; break;
            case 6: text+="}\n"; break;
            default: text+="\n"; break;
            }
        }
        load(editor,text.c_str());
        editor.setMiniMapEnabled(true); QApplication::processEvents();
        auto* map=editor.findChild<QWidget*>("editorMiniMap");
        assert(map && map->isVisible() && editor.miniMapEnabled());
        assert(map->width()==112 && map->height()==editor.viewport()->height());
        assert(map->geometry().left()==editor.viewport()->geometry().right()+1);
        const auto bytes=editor.document()->buffer().text();
        if (qEnvironmentVariableIsSet("TURTLE_CODE_MINIMAP_SCREENSHOT"))
            editor.grab().save(qEnvironmentVariable("TURTLE_CODE_MINIMAP_SCREENSHOT"));
        const auto cursor=editor.navigationPosition();
        QTest::mouseClick(map,Qt::LeftButton,Qt::NoModifier,QPoint(30,map->height()-2));
        assert(editor.verticalScrollBar()->value()>1900);
        assert(editor.navigationPosition()==cursor && editor.document()->buffer().text()==bytes);
        QTest::mousePress(map,Qt::LeftButton,Qt::NoModifier,QPoint(30,map->height()-2));
        QMouseEvent drag(QEvent::MouseMove,QPointF(30,1),QPointF(map->mapToGlobal(QPoint(30,1))),
                         Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(map,&drag);
        QTest::mouseRelease(map,Qt::LeftButton,Qt::NoModifier,QPoint(30,1));
        assert(editor.verticalScrollBar()->value()==0);
        const int withMap=editor.viewport()->width();
        editor.verticalScrollBar()->setValue(1000);
        editor.setMiniMapEnabled(false); QApplication::processEvents();
        assert(editor.verticalScrollBar()->value()==1000);
        assert(!map->isVisible() && !editor.miniMapEnabled());
        assert(editor.viewport()->width()==withMap+112);
        editor.setMiniMapEnabled(true); QApplication::processEvents();
        assert(editor.viewport()->width()==withMap);
        assert(editor.verticalScrollBar()->value()==1000);
        editor.resize(500,500); QApplication::processEvents();
        assert(map->geometry().left()==editor.viewport()->geometry().right()+1);
        assert(map->height()==editor.viewport()->height());
        // Rendering stays bounded even with a very long single line; reload and
        // edits must discard the previous thumbnail.
        editor.loadBuffer(PieceTreeBuffer(std::string(1024*1024,'x')),"long.txt");
        assert(!map->grab().isNull());
        editor.loadBuffer(PieceTreeBuffer(""),"empty.txt");
        const auto empty=map->grab().toImage();
        QTest::keyClick(&editor,Qt::Key_A);
        const auto edited=map->grab().toImage();
        assert(edited!=empty);
        editor.documentUndo(); assert(map->grab().toImage()==empty);
        editor.documentUndo(true); assert(map->grab().toImage()==edited);
    }
    {
        EditorWidget editor; load(editor,"alpha\nbeta\ngamma\n");
        const auto bytes=editor.document()->buffer().text();
        editor.setDiffAlignment({-1,0,-1,-1,1,2,3,-1});
        assert(editor.visualRowForLine(0)==1 && editor.visualRowForLine(1)==4);
        assert(editor.documentLineForVisualRow(2)==1);
        const int height=editor.fontMetrics().height()+3;
        editor.setNavigationPosition(2,1);
        assert(editor.viewport()->mapFromGlobal(editor.cursorScreenPosition()).y()>=4*height);
        QTest::mouseClick(editor.viewport(),Qt::LeftButton,Qt::NoModifier,
            QPoint(editor.lineNumberAreaWidth()+12,5*height+height/2));
        assert(editor.navigationPosition()==QPoint(3,1));
        assert(editor.document()->buffer().text()==bytes);
        // Copy and typing operate on document lines, never on spacers.
        QTest::keyClick(&editor,Qt::Key_A,Qt::ControlModifier);
        assert(editor.selectedText()==QString::fromStdString(bytes));
        editor.setNavigationPosition(2,1); QTest::keyClick(&editor,Qt::Key_X);
        assert(editor.document()->buffer().text()=="alpha\nxbeta\ngamma\n");
        assert(editor.visualRowForLine(1)==4);
        editor.documentUndo(); assert(editor.document()->buffer().text()==bytes);
        assert(editor.visualRowForLine(1)==4);
        // Splitting a line consumes an adjacent spacer instead of shifting
        // every following real line; joins, paste and undo keep a complete map.
        editor.setNavigationPosition(1,3);
        QTest::keyClick(&editor,Qt::Key_Return);
        assert(editor.visualRowForLine(1)==2 && editor.visualRowForLine(2)==4);
        QTest::keyClick(&editor,Qt::Key_Backspace);
        assert(editor.document()->buffer().text()==bytes);
        assert(editor.visualRowForLine(1)==4);
        QTest::keyClick(&editor,Qt::Key_A,Qt::ControlModifier);
        QApplication::clipboard()->setText("one\ntwo\nthree\nfour\n"); editor.pasteClipboard();
        const auto checkMap=[&] {
            int previous=-1;
            for (int line=0;line<int(editor.document()->buffer().lineCount());++line) {
                const int row=editor.visualRowForLine(line); assert(row>previous);
                assert(editor.documentLineForVisualRow(row)==line); previous=row;
            }
        };
        checkMap(); editor.documentUndo(); checkMap();
        assert(editor.document()->buffer().text()==bytes);
        editor.documentUndo(true); checkMap(); editor.documentUndo();
        editor.setDiffAlignment({0,-1,1,2,3}); editor.setDiffAlignment({});
        assert(editor.visualRowForLine(1)==1);
    }
    {
        EditorWidget editor; load(editor,"alpha\nbeta\ngamma\n");
        editor.setNavigationPosition(2,3);
        const int height=editor.fontMetrics().height()+3;
        const auto rowColor=[&](int line) {
            return editor.viewport()->grab().toImage().pixelColor(editor.viewport()->width()-15,
                editor.visualRowForLine(line)*height+height-1);
        };
        const auto plain=rowColor(1), plainInactive=rowColor(2);
        editor.setDiffHighlights({{1,0,4},{2,0,5}});
        const auto changed=rowColor(1), changedInactive=rowColor(2);
        const auto characterColor=[&] {
            return editor.viewport()->grab().toImage().pixelColor(
                editor.lineNumberAreaWidth()+12+editor.fontMetrics().horizontalAdvance('b')/2,2*height-1);
        };
        const auto markedCharacter=characterColor();
        assert(changed!=plain && markedCharacter!=changed);
        QTest::keyClick(&editor,Qt::Key_X);
        assert(characterColor()==markedCharacter);
        assert(rowColor(1)==changed); // Still colored before any async result.
        editor.documentUndo(); assert(rowColor(1)==changed);
        QTest::keyClick(&editor,Qt::Key_Return);
        assert(rowColor(1)==changedInactive && rowColor(2)==changed && rowColor(3)==changedInactive);
        editor.documentUndo(); assert(rowColor(1)==changed && rowColor(2)==changedInactive);
        editor.documentUndo(true); assert(rowColor(1)==changedInactive && rowColor(2)==changed);
        editor.setDiffHighlights({}); assert(rowColor(1)==plainInactive);
        // Opening a different document must discard the old diff colors.
        editor.setDiffHighlights({{1,0,2}});
        load(editor,"new\ndocument\n"); editor.setNavigationPosition(2,1);
        assert(rowColor(1)==plain);
    }
    {
        EditorWidget editor; load(editor,"alpha\r\nbeta\r\n");
        QTest::keyClick(&editor,Qt::Key_End);
        assert(editor.navigationPosition()==QPoint(1,6));
        QTest::keyClick(&editor,Qt::Key_Home,Qt::ShiftModifier);
        QTest::keyClicks(&editor,"alpha");
        assert(editor.document()->buffer().text()=="alpha\r\nbeta\r\n");
        QTest::keyClick(&editor,Qt::Key_End); QTest::keyClick(&editor,Qt::Key_Backspace); QTest::keyClick(&editor,Qt::Key_A);
        assert(editor.document()->buffer().text()=="alpha\r\nbeta\r\n");
        QTest::keyClick(&editor,Qt::Key_Right); assert(editor.navigationPosition()==QPoint(2,1));
        QTest::keyClick(&editor,Qt::Key_Left); assert(editor.navigationPosition()==QPoint(1,6));
        QTest::keyClick(&editor,Qt::Key_Delete);
        assert(editor.document()->buffer().text()=="alphabeta\r\n");
        editor.documentUndo(); assert(editor.document()->buffer().text()=="alpha\r\nbeta\r\n");
        editor.setNavigationPosition(2,1); QTest::keyClick(&editor,Qt::Key_Backspace);
        assert(editor.document()->buffer().text()=="alphabeta\r\n");
        editor.documentUndo(); assert(editor.document()->buffer().text()=="alpha\r\nbeta\r\n");
        editor.setNavigationPosition(1,100); assert(editor.navigationPosition()==QPoint(1,6));
    }
    {
        struct Case {const char* input;int line;bool copy;int direction;const char* expected;int target;};
        for(const auto& test:std::vector<Case>{
            {"a\nb\nc",2,true,-1,"a\nb\nb\nc",2},
            {"a\nb\nc",2,true,1,"a\nb\nb\nc",3},
            {"a\nb\nc",2,false,-1,"b\na\nc",1},
            {"a\nb\nc",2,false,1,"a\nc\nb",3},
            {"a\nb",2,true,1,"a\nb\nb",3},
            {"a\nb",2,true,-1,"a\nb\nb",2},
            {"a\nb",1,false,1,"b\na",2},
            {"a\nb",2,false,-1,"b\na",1},
            {"a\nb\n",3,true,1,"a\nb\n\n",4},
            {"a\nb\n",2,false,1,"a\n\nb",3},
            {"",1,true,1,"\n",2},
            {"single",1,true,-1,"single\nsingle",1},
            {"a\r\nb\r\nc",2,false,1,"a\r\nc\r\nb",3},
            {"a\r\nb",2,true,1,"a\r\nb\r\nb",3},
            {"\xef\xbb\xbfодин\nдва",1,false,1,"\xef\xbb\xbfдва\nодин",2},
            {"\xef\xbb\xbfодин\nдва",1,true,1,"\xef\xbb\xbfодин\nодин\nдва",2}
        }) {
            EditorWidget editor;load(editor,test.input);editor.setNavigationPosition(test.line,1);
            QTest::keyClick(&editor,test.direction<0?Qt::Key_Up:Qt::Key_Down,
                test.copy?Qt::AltModifier|Qt::ShiftModifier:Qt::AltModifier);
            Q_ASSERT(editor.document()->buffer().text()==test.expected);
            Q_ASSERT(editor.navigationPosition().x()==test.target);
            QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);
            Q_ASSERT(editor.document()->buffer().text()==test.input);
            QTest::keyClick(&editor,Qt::Key_Y,Qt::ControlModifier);
            Q_ASSERT(editor.document()->buffer().text()==test.expected);
        }
    }
    {
        EditorWidget editor;load(editor,"a\nb\nc");
        editor.moveLines(-1);Q_ASSERT(!editor.document()->history().canUndo());
        editor.setNavigationPosition(3,1);editor.moveLines(1);Q_ASSERT(!editor.document()->history().canUndo());
        editor.setNavigationPosition(2,1);editor.setBusy(true);editor.copyLines(1);
        Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc");editor.setBusy(false);
    }
    {
        // A selection ending at the next line's column zero excludes that line.
        EditorWidget editor;load(editor,"a\nb\nc\nd");editor.setNavigationPosition(2,1);
        QTest::keyClick(&editor,Qt::Key_Down,Qt::ShiftModifier);
        QTest::keyClick(&editor,Qt::Key_Down,Qt::ShiftModifier);
        Q_ASSERT(editor.selectedText()=="b\nc\n");editor.moveLines(-1);
        Q_ASSERT(editor.document()->buffer().text()=="b\nc\na\nd");
        Q_ASSERT(editor.selectedText()=="b\nc\n");editor.moveLines(1);
        Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc\nd");
        editor.copyLines(1);Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc\nb\nc\nd");
        Q_ASSERT(editor.selectedText()=="b\nc\n");
        QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text()=="a\nb\nc\nd");
    }
    {
        // Reverse selection and UTF-8 byte columns survive movement.
        EditorWidget editor;load(editor,"aa\nпривет😀\nzz");editor.setNavigationPosition(2,5);
        QTest::keyClick(&editor,Qt::Key_Left,Qt::ShiftModifier);
        QTest::keyClick(&editor,Qt::Key_Left,Qt::ShiftModifier);
        const auto selected=editor.selectedText();editor.copyLines(-1);
        Q_ASSERT(editor.selectedText()==selected && editor.navigationPosition().x()==2);
        editor.moveLines(1);Q_ASSERT(editor.selectedText()==selected && editor.navigationPosition().x()==3);
    }
    {
        EditorWidget editor;load(editor,"aa\nbb\ncc");
        QTest::keyClick(&editor,Qt::Key_Down,Qt::ControlModifier|Qt::ShiftModifier);
        Q_ASSERT(editor.cursorCount()==2);editor.copyLines(1);
        Q_ASSERT(editor.document()->buffer().text()=="aa\nbb\naa\nbb\ncc" && editor.cursorCount()==2);
        QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text()=="aa\nbb\ncc" && editor.cursorCount()==2);
    }
    {
        EditorWidget editor;const auto original=std::string(1100000,'x')+"\ntail";load(editor,original.c_str());
        editor.copyLines(1);Q_ASSERT(editor.isBusy());
        const auto wait=[&]{QElapsedTimer timer;timer.start();while(editor.isBusy() && timer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}Q_ASSERT(!editor.isBusy());};
        wait();Q_ASSERT(editor.document()->buffer().text()==std::string(1100000,'x')+"\n"+original);
        QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);wait();Q_ASSERT(editor.document()->buffer().text()==original);
    }
    {
        EditorWidget editor; load(editor,"abc\ndef\nghi\n");
        SemanticHighlightResult result; result.valid=true;
        for(int line=0;line<3;++line) result.lines[line].append({line,0,3,"parameter",{}});
        const auto revision=editor.lspRevision(),generation=editor.semanticGeneration();
        editor.setSemanticHighlighting(result,revision,generation);
        Q_ASSERT(editor.semanticTokenCount()==3);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.semanticTokenCount()==3 && editor.semanticSnapshot()[0].first().start==1);
        editor.setSemanticHighlighting(result,revision,generation);
        Q_ASSERT(editor.semanticTokenCount()==3 && editor.semanticSnapshot()[0].first().start==1); // A delayed result cannot repaint old positions.
        QTest::keyClick(&editor,Qt::Key_Return);
        Q_ASSERT(editor.semanticSnapshot().contains(2) && editor.semanticSnapshot().contains(3));
        Q_ASSERT(editor.semanticSnapshot()[2].first().line==2);
        QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);
        Q_ASSERT(editor.semanticTokenCount()==3 && editor.semanticSnapshot().contains(0) && editor.semanticSnapshot().contains(2));
    }
    {
        EditorWidget editor; load(editor,"void pin(int gpio) { return; }\n");
        SemanticHighlightResult result; result.valid=true;
        result.lines[0].append({0,13,4,"parameter",{}});
        editor.setSemanticHighlighting(result,editor.lspRevision(),editor.semanticGeneration());
        editor.setNavigationPosition(1,15); QTest::keyClick(&editor,Qt::Key_X);
        auto tokens=editor.semanticSnapshot()[0];
        Q_ASSERT(!tokens.isEmpty() && tokens.first().start==13);
        int covered=0; for(const auto& token:tokens) covered+=token.length;
        Q_ASSERT(covered==5); // Newly inserted character inside the parameter inherits its color.
        QTest::keyClick(&editor,Qt::Key_Backspace);
        tokens=editor.semanticSnapshot().value(0);
        covered=0; for(const auto& token:tokens) covered+=token.length;
        Q_ASSERT(covered==4);
        QTemporaryDir directory; Q_ASSERT(directory.isValid());
        Q_ASSERT(editor.saveFile(directory.filePath("sample.cpp")));
        Q_ASSERT(!editor.semanticSnapshot()[0].isEmpty());
        editor.loadBuffer(PieceTreeBuffer("int unrelated;\n"),"other.cpp");
        Q_ASSERT(editor.semanticTokenCount()==0);
    }
    {
        EditorWidget editor;load(editor,"base\n");
        const QString payload=QString(600000,QLatin1Char('x'))+QString::fromUtf8("😀\n");
        QApplication::clipboard()->setText(payload);QTest::keyClick(&editor,Qt::Key_V,Qt::ControlModifier);
        Q_ASSERT(editor.isBusy());QElapsedTimer timer;timer.start();
        while(editor.isBusy() && timer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}
        Q_ASSERT(!editor.isBusy() && editor.document()->buffer().size()==payload.toUtf8().size()+5);
        QTest::keyClick(&editor,Qt::Key_A,Qt::ControlModifier);QTest::keyClick(&editor,Qt::Key_C,Qt::ControlModifier);
        // The first copy remains below the byte threshold; verify UTF-8 paste fidelity.
        Q_ASSERT(QApplication::clipboard()->text()==payload+"base\n");
        QApplication::clipboard()->setText(QString(1100000,QLatin1Char('y')));
        QTest::keyClick(&editor,Qt::Key_V,Qt::ControlModifier);Q_ASSERT(editor.isBusy());
        auto dialogs=editor.findChildren<QProgressDialog*>();Q_ASSERT(!dialogs.isEmpty());
        auto* progress=dialogs.back();progress->cancel();
        timer.restart();while(editor.isBusy() && timer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}
        Q_ASSERT(editor.document()->buffer().size()==payload.toUtf8().size()+5);
        editor.setNavigationPosition(1,600003);
        Q_ASSERT(editor.navigationPosition()==QPoint(1,600003) && editor.lspPosition()==QPoint(0,600002));
        QTest::keyClick(&editor,Qt::Key_Left);
        Q_ASSERT(editor.navigationPosition()==QPoint(1,600001)); // One emoji is one grapheme and two UTF-16 units.
        editor.setNavigationPosition(1,1); // Large first line uses bounded painting and hit-testing.
        QImage image(editor.size(),QImage::Format_ARGB32);editor.render(&image);
    }
    {
        EditorWidget editor;
        load(editor, "locked\n");
        editor.setBusy(true);
        QTest::keyClick(&editor, Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text() == "locked\n");
        editor.setBusy(false);
    }
    {
        EditorWidget editor;
        load(editor, "foo foo\nfoo foo\n");
        QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
        Q_ASSERT(editor.cursorCount() == 2);
        QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
        Q_ASSERT(editor.cursorCount() == 3);
        QTest::keyClick(&editor, Qt::Key_X);
        const auto replaced = editor.document()->buffer().text();
        Q_ASSERT(std::count(replaced.begin(), replaced.end(), 'x') == 3);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text() == "foo foo\nfoo foo\n");
    }
    {
        EditorWidget editor;
        const char* original="adc adc_state my_adc adc2 ADC\nadc (adc) adc\n";
        load(editor,original);
        // Starting at a caret selects whole identifiers, and repeated Ctrl+D
        // must retain that rule after the initial word becomes a selection.
        for(int i=0;i<8;++i)QTest::keyClick(&editor,Qt::Key_D,Qt::ControlModifier);
        Q_ASSERT(editor.cursorCount()==4);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text()=="x adc_state my_adc adc2 ADC\nx (x) x\n");
        editor.documentUndo();Q_ASSERT(editor.document()->buffer().text()==original);
        // Explicit selection of a complete identifier follows the same rule.
        editor.revealMatch(1,1,3);
        editor.selectNextOccurrence();Q_ASSERT(editor.cursorCount()==2);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text()=="x adc_state my_adc adc2 ADC\nx (adc) adc\n");
    }
    {
        EditorWidget editor;load(editor,"adc_state adc_other adc\n");
        // Deliberately selecting a fragment still supports substring editing.
        editor.revealMatch(1,1,3);
        editor.selectNextOccurrence();Q_ASSERT(editor.cursorCount()==2);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text()=="x_state x_other adc\n");
    }
    {
        EditorWidget editor;load(editor,"adc жadc adcж adc\u0301 𐐀adc adc𐐀 (adc)\n");
        for(int i=0;i<8;++i)editor.selectNextOccurrence();
        Q_ASSERT(editor.cursorCount()==2);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text()=="x жadc adcж adc\u0301 𐐀adc adc𐐀 (x)\n");
        // The initial word expansion also handles multibyte identifiers.
        load(editor,"датчик датчик_state датчик\n");
        editor.selectNextOccurrence();Q_ASSERT(editor.cursorCount()==2);
        QTest::keyClick(&editor,Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text()=="x датчик_state x\n");
    }
    {
        EditorWidget editor;
        load(editor, "abc\ndef\nghi\n");
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        Q_ASSERT(editor.cursorCount() == 3);
        QTest::keyClick(&editor, Qt::Key_Right);
        QTest::keyClick(&editor, Qt::Key_X);
        const auto text = editor.document()->buffer().text();
        Q_ASSERT(std::count(text.begin(), text.end(), 'x') == 3);
        Q_ASSERT(text == "axbc\ndxef\ngxhi\n");
        QTest::keyClick(&editor, Qt::Key_Backspace);
        const auto afterDelete = editor.document()->buffer().text();
        Q_ASSERT(std::count(afterDelete.begin(), afterDelete.end(), 'x') == 0);
        Q_ASSERT(afterDelete == "abc\ndef\nghi\n");
    }
    {
        EditorWidget editor;
        load(editor, "abc\ndef\nghi\n");
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_Return);
        Q_ASSERT(editor.document()->buffer().text() == "\nabc\n\ndef\n\nghi\n");
    }
    {
        EditorWidget editor;
        load(editor, "abc\ndef\n");
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_Right, Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text() == "xbc\nxef\n");
    }
    {
        EditorWidget editor;
        load(editor, "foo foo\n");
        QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_Left);
        QTest::keyClick(&editor, Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text() == "xfoo xfoo\n");
    }
    {
        EditorWidget editor;
        load(editor, "abc\ndef\nghi\n");
        const int rowHeight = editor.fontMetrics().height() + 3;
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::AltModifier,
                          QPoint(40, rowHeight + rowHeight / 2));
        Q_ASSERT(editor.cursorCount() == 2);
    }
    {
        EditorWidget editor;
        load(editor, "alpha_beta мир! value\n");
        const auto metrics = editor.fontMetrics();
        const int textX = metrics.horizontalAdvance(QStringLiteral("2")) + 24 + 12;
        const int y = (metrics.height() + 3) / 2;
        QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                           QPoint(textX + metrics.horizontalAdvance(QStringLiteral("alpha_")), y));
        Q_ASSERT(editor.selectedText() == QStringLiteral("alpha_beta"));
        QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                           QPoint(textX + metrics.horizontalAdvance(QStringLiteral("alpha_beta м")), y));
        Q_ASSERT(editor.selectedText() == QString::fromUtf8("мир"));
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(textX, y));
        Q_ASSERT(editor.selectedText().isEmpty());
    }
    {
        EditorWidget editor;
        load(editor, "one\ntwo\nthree");
        const int rowHeight = editor.fontMetrics().height() + 3;
        const QPoint secondLine(50, rowHeight + rowHeight / 2);
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, secondLine);
        QTest::keyClick(&editor, Qt::Key_L, Qt::ControlModifier);
        Q_ASSERT(editor.selectedText() == QStringLiteral("two\n"));
        QTest::keyClick(&editor, Qt::Key_L, Qt::ControlModifier);
        Q_ASSERT(editor.selectedText() == QStringLiteral("two\nthree"));
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, secondLine);
        QTest::mouseDClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, secondLine);
        QTest::mousePress(editor.viewport(), Qt::LeftButton, Qt::NoModifier, secondLine);
        Q_ASSERT(editor.selectedText() == QStringLiteral("two\n"));
        QTest::mouseRelease(editor.viewport(), Qt::LeftButton, Qt::NoModifier, secondLine);
    }
    {
        EditorWidget editor;
        load(editor, "foo FOO foo\nbar foo\n");
        editor.showFindBar();
        auto* toggle = editor.findChild<QToolButton*>("fileReplaceToggle");
        auto* replacementField = editor.findChild<QLineEdit*>("fileReplaceInput");
        Q_ASSERT(toggle && replacementField);
        auto* replaceRow = replacementField->parentWidget();
        Q_ASSERT(!toggle->isChecked() && !replaceRow->isVisible());
        toggle->click();
        Q_ASSERT(toggle->isChecked() && replaceRow->isVisible());
        toggle->click();
        Q_ASSERT(!toggle->isChecked() && !replaceRow->isVisible());
        editor.showFindBar(true);
        Q_ASSERT(toggle->isChecked() && replaceRow->isVisible());
        auto* query = editor.findChild<QLineEdit*>("fileFindInput");
        auto* replacement = editor.findChild<QLineEdit*>("fileReplaceInput");
        Q_ASSERT(query && replacement);
        query->setText("foo");
        editor.findNext();
        Q_ASSERT(editor.selectedText() == "foo");
        editor.findNext();
        Q_ASSERT(editor.selectedText() == "FOO");
        replacement->setText("x");
        auto* replaceAll = [&editor] {
            for (auto* button : editor.findChildren<QToolButton*>())
                if (button->text() == "Replace All") return button;
            return static_cast<QToolButton*>(nullptr);
        }();
        Q_ASSERT(replaceAll);
        replaceAll->click();
        Q_ASSERT(editor.document()->buffer().text() == "x x x\nbar x\n");
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text() == "foo FOO foo\nbar foo\n");
        query->setText("[a-z]+ ");
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->objectName() == "fileFindRegex") button->click();
        editor.findNext();
        Q_ASSERT(editor.selectedText() == "bar ");
    }
    {
        EditorWidget editor;
        load(editor, "foo foobar foo\n");
        editor.showFindBar(true);
        auto* query = editor.findChild<QLineEdit*>("fileFindInput");
        auto* replacement = editor.findChild<QLineEdit*>("fileReplaceInput");
        Q_ASSERT(query && replacement);
        query->setText("foo");
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->objectName() == "fileFindWholeWord") button->click();
        editor.findNext();
        replacement->setText("Z");
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->text() == "Replace") button->click();
        Q_ASSERT(editor.document()->buffer().text() == "Z foobar foo\n");
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text() == "foo foobar foo\n");
    }
    {
        EditorWidget editor;
        load(editor, "item-12 item-34\nпрефикс item-56\n");
        editor.showFindBar(true);
        auto* query = editor.findChild<QLineEdit*>("fileFindInput");
        auto* replacement = editor.findChild<QLineEdit*>("fileReplaceInput");
        Q_ASSERT(query && replacement);
        query->setText("item-(\\d+)");
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->objectName() == "fileFindRegex") button->click();
        replacement->setText("number:$1");
        editor.findNext();
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->text() == "Replace All") button->click();
        Q_ASSERT(editor.document()->buffer().text() == "number:12 number:34\nпрефикс number:56\n");
    }
    {
        std::string original;
        std::string expected;
        for (int i = 0; i < 4'000; ++i) {
            original += "foo and foo\n";
            expected += "longer and longer\n";
        }
        EditorWidget editor;
        editor.loadBuffer(PieceTreeBuffer(original), QStringLiteral("many.txt"));
        editor.resize(800, 400);
        editor.show();
        editor.showFindBar(true);
        auto* query = editor.findChild<QLineEdit*>("fileFindInput");
        auto* replacement = editor.findChild<QLineEdit*>("fileReplaceInput");
        Q_ASSERT(query && replacement);
        query->setText("foo");
        replacement->setText("longer");
        editor.findNext();
        for (auto* button : editor.findChildren<QToolButton*>())
            if (button->text() == "Replace All") button->click();
        QElapsedTimer operationTimer;operationTimer.start();
        while(editor.isBusy() && operationTimer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}
        Q_ASSERT(!editor.isBusy());
        Q_ASSERT(editor.document()->buffer().text() == expected);
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        operationTimer.restart();while(editor.isBusy() && operationTimer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}
        Q_ASSERT(!editor.isBusy() && editor.document()->buffer().text() == original);
        QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
        operationTimer.restart();while(editor.isBusy() && operationTimer.elapsed()<10000){QApplication::processEvents();QThread::msleep(1);}
        Q_ASSERT(!editor.isBusy() && editor.document()->buffer().text() == expected);
    }
    {
        std::string largeText;
        largeText.reserve(2'300'000);
        for (int i = 0; i < 150'000; ++i) largeText += "something else\n";
        largeText += "needle\n";
        EditorWidget editor;
        editor.loadBuffer(PieceTreeBuffer(largeText), QStringLiteral("large.txt"));
        editor.resize(800, 400);
        editor.show();
        editor.showFindBar();
        auto* query = editor.findChild<QLineEdit*>("fileFindInput");
        auto* count = editor.findChild<QLabel*>("fileFindCount");
        Q_ASSERT(query && count);
        query->setText("needle");
        QElapsedTimer timer;
        timer.start();
        while (count->text() != "0/1" && timer.elapsed() < 10'000) {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        Q_ASSERT(count->text() == "0/1");
        editor.findNext();
        Q_ASSERT(editor.selectedText() == "needle");
    }
    {
        EditorWidget editor;
        load(editor, "int café = 1;\nca\n");
        editor.goToLine(2);
        QTest::keyClick(&editor, Qt::Key_End);
        Q_ASSERT(editor.lspPosition() == QPoint(1, 2));
        const QJsonObject start{{"line", 1}, {"character", 0}};
        const QJsonObject end{{"line", 1}, {"character", 2}};
        const QJsonObject item{
            {"label", QStringLiteral("café")},
            {"textEdit", QJsonObject{{"range", QJsonObject{{"start", start}, {"end", end}}},
                                     {"newText", QStringLiteral("café")}}}
        };
        editor.applyCompletion(item);
        Q_ASSERT(editor.document()->buffer().text() == "int café = 1;\ncafé\n");
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text() == "int café = 1;\nca\n");
    }

    {
        EditorWidget editor;
        load(editor, "fo");
        QTest::keyClick(&editor, Qt::Key_End);
        editor.showCompletions(QJsonArray{QJsonObject{{"label", "foobar"}}});
        auto* completer = editor.findChild<QCompleter*>();
        Q_ASSERT(completer && completer->popup()->isVisible());
        QTest::keyClick(completer->popup(), Qt::Key_Return);
        Q_ASSERT(editor.document()->buffer().text() == "foobar");
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text() == "fo");
        editor.showCompletions(QJsonArray{QJsonObject{{"label", "foobar"}}});
        QTest::keyClick(completer->popup(), Qt::Key_Escape);
        Q_ASSERT(editor.document()->buffer().text() == "fo");
        Q_ASSERT(!completer->popup()->isVisible());
        editor.showCompletions(QJsonArray{QJsonObject{{"label", "foobar"}}});
        QTest::keyClick(completer->popup(), Qt::Key_Left);
        Q_ASSERT(!completer->popup()->isVisible());
    }
    {
        EditorWidget editor;
        load(editor, "// 😀\nfo\n");
        editor.goToLine(2); QTest::keyClick(&editor, Qt::Key_End);
        auto point=[](int line,int character) { return QJsonObject{{"line",line},{"character",character}}; };
        auto range=[&](int line,int start,int end) { return QJsonObject{{"start",point(line,start)},{"end",point(line,end)}}; };
        editor.applyCompletion(QJsonObject{{"label","foo"},
            {"textEdit",QJsonObject{{"range",range(1,0,2)},{"newText","foo"}}},
            {"additionalTextEdits",QJsonArray{QJsonObject{{"range",range(0,0,0)},{"newText","#include <x>\n"}}}}});
        Q_ASSERT(editor.document()->buffer().text()=="#include <x>\n// 😀\nfoo\n");
        QTest::keyClick(&editor,Qt::Key_Z,Qt::ControlModifier);
        Q_ASSERT(editor.document()->buffer().text()=="// 😀\nfo\n");
        // A UTF-16 range that splits an emoji must be rejected atomically.
        editor.applyCompletion(QJsonObject{{"label","bad"},{"textEdit",QJsonObject{{"range",range(0,4,5)},{"newText","bad"}}}});
        Q_ASSERT(editor.document()->buffer().text()=="// 😀\nfo\n");
    }

    {
        EditorWidget editor;
        load(editor, "abc\ndef\nghi\n");
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        Q_ASSERT(editor.cursorCount() == 3);
        QApplication::processEvents();
        PaintRecorder recorder; editor.viewport()->installEventFilter(&recorder);
        const int rowHeight = editor.fontMetrics().height() + 3;
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(42, rowHeight / 2));
        QApplication::processEvents();
        Q_ASSERT(editor.cursorCount() == 1);
        Q_ASSERT(recorder.painted.contains(editor.viewport()->rect()));
        QTest::keyClick(&editor, Qt::Key_X);
        const auto text = editor.document()->buffer().text();
        Q_ASSERT(std::count(text.begin(), text.end(), 'x') == 1);
    }
    {
        EditorWidget editor;
        load(editor, "foo foo\nfoo foo\n");
        QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
        QTest::keyClick(&editor, Qt::Key_D, Qt::ControlModifier);
        Q_ASSERT(editor.cursorCount() == 3);
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(100, editor.viewport()->height()-20));
        Q_ASSERT(editor.cursorCount() == 1);
        Q_ASSERT(editor.selectedText().isEmpty());
        QTest::keyClick(&editor, Qt::Key_X);
        Q_ASSERT(editor.document()->buffer().text() == "foo foo\nfoo foo\nx");
        const int rowHeight = editor.fontMetrics().height() + 3;
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::AltModifier, QPoint(42,rowHeight/2));
        Q_ASSERT(editor.cursorCount() == 2);
        QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(42,rowHeight+rowHeight/2));
        Q_ASSERT(editor.cursorCount() == 1);
    }
    for (const bool multipleCursors : {false, true}) {
        for (const int column : {3, 4}) {
            EditorWidget editor;
            load(editor, "abcd\nefgh\n");
            if (multipleCursors) {
                QTest::keyClick(&editor, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
                Q_ASSERT(editor.cursorCount() == 2);
            }
            const auto metrics = editor.fontMetrics();
            const int textX = metrics.horizontalAdvance(QStringLiteral("2")) + 24 + 12;
            const int x = textX + metrics.horizontalAdvance(QStringLiteral("abcd").left(column));
            QTest::mouseClick(editor.viewport(), Qt::LeftButton, Qt::NoModifier,
                              QPoint(x, (metrics.height() + 3) / 2));
            Q_ASSERT(editor.cursorCount() == 1);
            Q_ASSERT(editor.navigationPosition() == QPoint(1, column + 1));
            QTest::keyClick(&editor, Qt::Key_Backspace);
            Q_ASSERT(editor.document()->buffer().text() == (column == 4 ? "abc\nefgh\n" : "abd\nefgh\n"));
            // The click anchor must not become a selection after deleting a character.
            Q_ASSERT(editor.selectedText().isEmpty());
            QTest::keyClick(&editor, Qt::Key_Backspace);
            Q_ASSERT(editor.document()->buffer().text() == (column == 4 ? "ab\nefgh\n" : "ad\nefgh\n"));
            Q_ASSERT(editor.selectedText().isEmpty());
        }
    }
    return 0;
}
