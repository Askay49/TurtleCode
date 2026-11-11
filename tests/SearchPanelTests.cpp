#include "search/SearchPanel.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    Q_ASSERT(directory.isValid());
    const QString path = directory.filePath("sample.txt");
    {
        QFile file(path);
        Q_ASSERT(file.open(QIODevice::WriteOnly));
        Q_ASSERT(file.write("foo foo\nbar foo\n") == 16);
    }

    SearchPanel panel;
    panel.resize(440, 500);
    panel.show();
    panel.setRoots({directory.path()});
    auto* query = panel.findChild<QLineEdit*>("projectSearchInput");
    auto* replacement = panel.findChild<QLineEdit*>("projectReplaceInput");
    auto* toggle = panel.findChild<QToolButton*>("projectReplaceToggle");
    auto* button = panel.findChild<QToolButton*>("projectReplaceButton");
    auto* results = panel.findChild<QTreeWidget*>("projectSearchResults");
    Q_ASSERT(query && replacement && toggle && button && results);
    query->setText("foo");
    QElapsedTimer timer;
    timer.start();
    while ((results->topLevelItemCount() != 1 || results->topLevelItem(0)->childCount() != 3) &&
           timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    Q_ASSERT(results->topLevelItemCount() == 1);
    auto* fileItem = results->topLevelItem(0);
    Q_ASSERT(fileItem->childCount() == 3);
    toggle->click();
    replacement->setText("baz");
    Q_ASSERT(fileItem->child(0)->text(0).contains(QStringLiteral("foo foo")));
    fileItem->child(1)->setCheckState(0, Qt::Unchecked);
    Q_ASSERT(fileItem->checkState(0) == Qt::PartiallyChecked);
    Q_ASSERT(button->isEnabled());

    bool finished = false;
    ProjectReplaceResult applied;
    QObject::connect(&panel, &SearchPanel::replaceRequested, &panel,
                     [&](const ProjectReplaceRequest& request) {
        Q_ASSERT(request.matches.size() == 3 && request.selected.count(true) == 2);
        panel.startReplace(request);
    });
    QObject::connect(&panel, &SearchPanel::replaceFinished, &panel,
                     [&](const ProjectReplaceResult& result) {
        applied = result;
        finished = true;
    });
    button->click();
    timer.restart();
    while (!finished && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    Q_ASSERT(finished && applied.matchesReplaced == 2 && applied.filesChanged == 1);
    QFile finalFile(path);
    Q_ASSERT(finalFile.open(QIODevice::ReadOnly));
    Q_ASSERT(finalFile.readAll() == "baz foo\nbar baz\n");

    const QString manyPath = directory.filePath("many.txt");
    {
        QFile file(manyPath);
        Q_ASSERT(file.open(QIODevice::WriteOnly));
        QByteArray contents;
        for (int index = 0; index < 5000; ++index) contents += "foo ";
        Q_ASSERT(file.write(contents) == contents.size());
    }
    SearchPanel manyPanel;
    manyPanel.resize(440, 500);
    manyPanel.show();
    manyPanel.setRoots({directory.path()});
    auto* manyQuery = manyPanel.findChild<QLineEdit*>("projectSearchInput");
    auto* manyReplacement = manyPanel.findChild<QLineEdit*>("projectReplaceInput");
    auto* manyToggle = manyPanel.findChild<QToolButton*>("projectReplaceToggle");
    auto* manyButton = manyPanel.findChild<QToolButton*>("projectReplaceButton");
    auto* manyResults = manyPanel.findChild<QTreeWidget*>("projectSearchResults");
    Q_ASSERT(manyQuery && manyReplacement && manyToggle && manyButton && manyResults);
    manyQuery->setText("foo");
    timer.restart();
    while (manyResults->topLevelItemCount() != 2 && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    Q_ASSERT(manyResults->topLevelItemCount() == 2);
    QTreeWidgetItem* manyItem = nullptr;
    for (int index = 0; index < manyResults->topLevelItemCount(); ++index)
        if (manyResults->topLevelItem(index)->toolTip(0) == manyPath)
            manyItem = manyResults->topLevelItem(index);
    Q_ASSERT(manyItem && manyItem->childCount() <= 51);
    manyToggle->click();
    const QString originalLabel = manyItem->child(0)->text(0);
    for (int index = 0; index < 200; ++index)
        manyReplacement->setText(QStringLiteral("replacement%1").arg(index));
    Q_ASSERT(manyItem->child(0)->text(0) == originalLabel);
    Q_ASSERT(manyButton->isEnabled());
    auto* more = manyItem->child(manyItem->childCount() - 1);
    Q_ASSERT(more->text(0).contains(QStringLiteral("Show next")));
    Q_EMIT manyResults->itemClicked(more, 0);
    QApplication::processEvents();
    Q_ASSERT(manyItem->childCount() > 200);
    return 0;
}
