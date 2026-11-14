#pragma once
#include <QElapsedTimer>
#include "search/SearchPanel.hpp"
static void projectScaleWindowTests(QTemporaryDir& temporary) {
    const auto root=temporary.filePath("many-files");assert(QDir().mkpath(root));
    for(int i=0;i<650;++i){QFile file(root+QString("/file%1.txt").arg(i,4,10,QChar('0')));assert(file.open(QIODevice::WriteOnly));file.write("foo\n");}
    MainWindow window;window.show();window.openPaths({root});
    auto* tree=window.findChild<ProjectExplorer*>();assert(tree);
    QElapsedTimer heartbeat;heartbeat.start();qint64 gap=0;QTimer timer;timer.setInterval(5);
    QObject::connect(&timer,&QTimer::timeout,&window,[&]{gap=std::max(gap,heartbeat.restart());});timer.start();
    const auto pager=[&](qint64 offset)->QTreeWidgetItem* {
        auto* parent=tree->topLevelItem(0);if(!parent)return nullptr;
        for(int i=0;i<parent->childCount();++i){auto* item=parent->child(i);if(item->data(0,Qt::UserRole+10).isValid() && item->data(0,Qt::UserRole+10).toLongLong()==offset)return item;}return nullptr;
    };
    assert(waitFor([&]{return pager(500)!=nullptr;}));
    assert(tree->topLevelItem(0)->childCount()==501);
    Q_EMIT tree->itemClicked(pager(500),0);
    assert(waitFor([&]{return pager(0)!=nullptr && tree->topLevelItem(0)->childCount()==151;}));
    assert(gap<1000);window.grab().save("/tmp/turtle-large-project.png");
    auto* panel=window.findChild<SearchPanel*>();assert(panel);
    auto* input=panel->findChild<QLineEdit*>("projectSearchInput");
    auto* results=panel->findChild<QTreeWidget*>("projectSearchResults");
    input->setText("foo");
    assert(waitFor([&]{return results->topLevelItemCount()==501;}));
    auto* first=results->topLevelItem(0);first->setCheckState(0,Qt::Unchecked);
    auto* next=results->topLevelItem(500);assert(next->data(0,Qt::UserRole+6).isValid());
    Q_EMIT results->itemClicked(next,0);
    assert(waitFor([&]{return results->topLevelItemCount()==151;}));
    auto* previous=results->topLevelItem(150);assert(previous->data(0,Qt::UserRole+6).toLongLong()==0);
    Q_EMIT results->itemClicked(previous,0);
    assert(waitFor([&]{return results->topLevelItemCount()==501;}));
    assert(results->topLevelItem(0)->checkState(0)==Qt::Unchecked);
    closeDiscard(window);
}
