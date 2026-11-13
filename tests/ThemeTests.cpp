#include "app/ThemeManager.hpp"
#include "app/SettingsSpinBox.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QSettings>
#include <QLabel>
#include <QTreeWidget>
#include <QListWidget>
#include <QTableWidget>
#include <QFileDialog>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QPainter>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QTimer>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <cassert>

class ItemGeometryProbe final : public QStyledItemDelegate {
public:
    static void check(QAbstractItemView& view,const QModelIndex& index) {
        view.ensurePolished();
        ItemGeometryProbe probe;
        QStyleOptionViewItem option;option.initFrom(&view);
        option.widget=&view;option.rect=QRect(0,0,350,32);
        option.decorationSize=QSize(16,16);
        probe.initStyleOption(&option,index);
        QRect expectedIcon,expectedText;
        for(const auto state:{QStyle::State(QStyle::State_Enabled),QStyle::State_Enabled|QStyle::State_MouseOver,
                              QStyle::State_Enabled|QStyle::State_Selected|QStyle::State_HasFocus}) {
            option.state=state;
            const auto text=view.style()->subElementRect(QStyle::SE_ItemViewItemText,&option,&view);
            QImage image(option.rect.size(),QImage::Format_ARGB32);image.fill(Qt::black);
            QPainter painter(&image);probe.paint(&painter,option,index);painter.end();
            QRect icon;
            for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
                if(image.pixelColor(x,y)==QColor("#ff00ff"))icon=icon.united(QRect(x,y,1,1));
            assert(!icon.isEmpty());
            if(expectedIcon.isEmpty()){expectedIcon=icon;expectedText=text;}
            else {
                if(icon!=expectedIcon || text!=expectedText)
                    qWarning()<<view.metaObject()->className()<<"row moved"<<expectedIcon<<icon<<expectedText<<text;
                assert(icon==expectedIcon && text==expectedText);
            }
        }
    }
};

static void stableItemGeometry() {
    QPixmap marker(16,16);marker.fill(QColor("#ff00ff"));QIcon icon;
    for(const auto mode:{QIcon::Normal,QIcon::Active,QIcon::Selected,QIcon::Disabled})
        for(const auto state:{QIcon::Off,QIcon::On})icon.addPixmap(marker,mode,state);
    QWidget workspace;workspace.setObjectName("workspaceBackground");
    QFileDialog dialog(&workspace);dialog.setOption(QFileDialog::DontUseNativeDialog);
    QWidget git(&workspace);git.setObjectName("gitPanel");
    QWidget debug(&workspace);debug.setObjectName("debugPanel");
    for(QWidget* parent:{static_cast<QWidget*>(nullptr),&workspace,static_cast<QWidget*>(&dialog),&git,&debug}) {
        QTreeWidget tree(parent);tree.setColumnCount(1);
        auto* row=new QTreeWidgetItem(&tree,QStringList{"sample.cpp"});row->setIcon(0,icon);
        ItemGeometryProbe::check(tree,tree.model()->index(0,0));
        QListWidget list(parent);auto* entry=new QListWidgetItem(icon,"sample.cpp",&list);
        Q_UNUSED(entry);ItemGeometryProbe::check(list,list.model()->index(0,0));
        QTableWidget table(1,1,parent);table.setItem(0,0,new QTableWidgetItem(icon,"sample.cpp"));
        ItemGeometryProbe::check(table,table.model()->index(0,0));
    }
}

int main(int argc, char** argv) {
    QTemporaryDir directory;
    assert(directory.isValid());
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    QApplication app(argc, argv);
    app.setOrganizationName("TurtleCodeTests"); app.setApplicationName("ThemeTests");
    auto& manager = ThemeManager::instance(); manager.initialize();
    stableItemGeometry();
    assert(manager.color("editor.background") == QColor("#1e1e1e"));
    int switched = 0;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        assert(dialog);
        auto* combo = dialog->findChild<QComboBox*>();
        assert(combo && combo->count() == 11);
        for (int i = 0; i < combo->count(); ++i) {
            combo->setCurrentIndex(i);
            QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, i));
            QApplication::processEvents();
            assert(manager.color("editor.background").isValid());
            QImage image(dialog->size(), QImage::Format_ARGB32);
            dialog->render(&image);
            assert(image.pixelColor(2, 2) == manager.color("window.background"));
            assert(QSettings().value("appearance/theme").toString() == combo->itemData(i).toString());
            ++switched;
        }
        dialog->reject();
    });
    manager.showSettings(nullptr);
    assert(switched == 11);
    // Loading a saved selection restores the same palette.
    manager.initialize();
    assert(manager.color("editor.background") == QColor("#1a1b26"));
    assert(manager.color("syntax.parameter") == QColor("#e0af68"));
    assert(manager.color("syntax.property") == QColor("#7dcfff"));
    assert(manager.color("syntax.builtinType") == QColor("#bb9af7"));
    assert(manager.color("syntax.type") == QColor("#c0caf5"));
    assert(manager.color("syntax.constant") == QColor("#ff9e64"));
    assert(manager.color("syntax.number") == QColor("#ff9e64"));
    assert(manager.captureStyle("variable.parameter", "text.primary").color==QColor("#e0af68"));
    assert(manager.captureStyle("function.method.call", "text.primary").color==QColor("#7aa2f7"));
    assert(manager.captureStyle("comment.documentation", "text.primary").fontStyles==2);
    assert(manager.captureStyle("keyword.return", "text.primary").fontStyles==2);
    assert(manager.captureStyle("keyword.conditional", "text.primary").fontStyles==0);
    assert(manager.semanticStyle("parameter", {}).color==QColor("#e0af68"));
    assert(manager.semanticStyle("property", {}).color==QColor("#7dcfff"));
    assert(!manager.semanticStyle("variable", {}).color.isValid());
    assert(manager.semanticStyle("variable", {"static"}).color==QColor("#ff9e64"));
    assert(manager.semanticStyle("function", {"defaultLibrary"}).color==QColor("#2ac3de"));
    assert(manager.semanticStyle("class", {"deprecated"}).fontStyles==8);
    assert(manager.color("text.primary")==QColor("#a9b1d6"));
    assert(manager.color("editor.currentLine")==QColor("#1e202e"));
    assert(manager.color("syntax.comment")==QColor("#51597d"));
    assert(manager.color("syntax.keyword")==QColor("#bb9af7"));
    assert(manager.color("editor.selection")==QColor("#2b2f41"));

    // Saving settings reapplies the current theme while a dialog is open.
    // Repeated refreshes must preserve colors after font changes and polish.
    {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* label = new QLabel("Settings refresh", &window);
        auto* tree = new QTreeWidget(&window);
        tree->setHeaderLabel("Files");
        auto* item = new QTreeWidgetItem(tree, {"main.cpp"});
        tree->setCurrentItem(item);
        auto* spin = new SettingsSpinBox(&window);
        spin->setRange(6,96); spin->setValue(18);
        layout->addWidget(label); layout->addWidget(tree); layout->addWidget(spin);
        window.resize(400, 300); window.show();
        QApplication::processEvents();
        const auto stylesheet = app.styleSheet();
        for (int i = 0; i < 4; ++i) {
            QDialog dialog(&window); dialog.show();
            manager.refreshAppearance();
            QFont font = tree->font(); font.setPointSize(12 + i); tree->setFont(font);
            dialog.close();
            QApplication::processEvents();
            assert(app.styleSheet() == stylesheet);
            QImage image(window.size(), QImage::Format_ARGB32);
            window.render(&image);
            assert(image.pixelColor(2, 2) == manager.color("window.background"));
            assert(label->palette().color(QPalette::WindowText) == manager.color("text.primary"));
            assert(tree->palette().color(QPalette::Text) == manager.color("text.primary"));
            const QRect row = tree->visualItemRect(item);
            const auto treeImage = tree->viewport()->grab().toImage();
            assert(treeImage.pixelColor(row.right()-20, row.center().y()) == manager.color("editor.selection"));
            QStyleOptionSpinBox option;
            option.initFrom(spin); option.rect = spin->rect();
            const auto spinImage = spin->grab().toImage();
            if (qEnvironmentVariableIsSet("TURTLE_CODE_SPIN_PREVIEW")) window.grab().save(qEnvironmentVariable("TURTLE_CODE_SPIN_PREVIEW"));
            for (const auto control : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
                const QRect button = spin->style()->subControlRect(QStyle::CC_SpinBox, &option, control, spin);
                assert(button.width() >= 14 && button.height() >= 14);
                int ink = 0;
                const auto foreground = manager.color("text.primary");
                for (int y=button.center().y()-5; y<=button.center().y()+5; ++y)
                    for (int x=button.center().x()-5; x<=button.center().x()+5; ++x)
                        if ((spinImage.pixelColor(x,y).red()-foreground.red())*(spinImage.pixelColor(x,y).red()-foreground.red())
                            + (spinImage.pixelColor(x,y).green()-foreground.green())*(spinImage.pixelColor(x,y).green()-foreground.green())
                            + (spinImage.pixelColor(x,y).blue()-foreground.blue())*(spinImage.pixelColor(x,y).blue()-foreground.blue()) < 12000) ++ink;
                assert(ink > 2); // Each button must contain a visible glyph.
            }
        }
    }

    const auto themeDir=QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/themes";
    assert(QDir().mkpath(themeDir));
    const auto first=themeDir+"/one.json",second=themeDir+"/two.json";
    for(const auto& path:{first,second}) {
        QFile file(path); assert(file.open(QIODevice::WriteOnly));
        assert(file.write(QJsonDocument(QJsonObject{{"formatVersion",1},{"name",QFileInfo(path).baseName()},
            {"colors",QJsonObject{{"editor.background","#123456"}}}}).toJson())>0);
    }
    QSettings().setValue("appearance/theme","custom-one"); manager.initialize();
    assert(manager.color("editor.background")==QColor("#123456"));
    QTimer::singleShot(0,[&] {
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); assert(dialog);
        auto* combo=dialog->findChild<QComboBox*>();
        auto* remove=dialog->findChild<QPushButton*>("deleteCustomTheme");
        assert(combo && remove && combo->count()==13 && remove->isEnabled());
        auto select=[&](const QString& id) {
            const auto index=combo->findData(id); assert(index>=0); combo->setCurrentIndex(index);
            QMetaObject::invokeMethod(combo,"activated",Qt::DirectConnection,Q_ARG(int,index));
        };
        select("tokyo-night"); assert(!remove->isEnabled());
        select("custom-one"); assert(remove->isEnabled());
        int answer=QMessageBox::Cancel, confirmations=0, errors=0;
        QTimer responder;
        QObject::connect(&responder,&QTimer::timeout,[&] {
            auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if(!box)return;
            if(box->windowTitle()=="Delete theme") { ++confirmations; box->button(static_cast<QMessageBox::StandardButton>(answer))->click(); }
            else { assert(box->windowTitle()=="Cannot delete theme"); ++errors; box->accept(); }
        }); responder.start(5);
        remove->click();
        assert(confirmations==1 && QFile::exists(first) && combo->count()==13);
        assert(combo->currentData().toString()=="custom-one");
        // A filesystem error must preserve the active theme and catalog.
        assert(QFile::rename(first,first+".backup")); answer=QMessageBox::Yes;
        remove->click();
        assert(errors==1 && combo->count()==13 && manager.color("editor.background")==QColor("#123456"));
        assert(QFile::rename(first+".backup",first));
        remove->click();
        assert(!QFile::exists(first) && QFile::exists(second) && combo->count()==12);
        assert(combo->currentData().toString()=="cpp-dark" && !remove->isEnabled());
        assert(QSettings().value("appearance/theme").toString()=="cpp-dark");
        assert(manager.color("editor.background")==QColor("#1e1e1e"));
        select("custom-two"); remove->click();
        assert(!QFile::exists(second) && combo->count()==11 && !remove->isEnabled());
        responder.stop(); dialog->reject();
    });
    manager.showSettings(nullptr);
    manager.initialize();
    assert(manager.color("editor.background")==QColor("#1e1e1e"));
    assert(QDir(themeDir).entryList({"*.json"},QDir::Files).isEmpty());
}
