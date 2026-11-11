#pragma once

#include "app/Codicons.hpp"
#include "app/FontSettings.hpp"
#include "app/I18n.hpp"
#include "app/ThemeManager.hpp"
#include <QDockWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QAction>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <functional>

// Presents both hierarchical DocumentSymbol and flat SymbolInformation replies.
class OutlinePanel final : public QDockWidget {
public:
    explicit OutlinePanel(QWidget* parent) : QDockWidget(I18n::text("Outline"),parent) {
        setObjectName("outlinePanel");
        setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea);
        setFeatures(QDockWidget::DockWidgetClosable|QDockWidget::DockWidgetMovable);
        auto* title=new QWidget(this);
        auto* titleLayout=new QHBoxLayout(title); titleLayout->setContentsMargins(6,2,4,2);
        titleLayout->addWidget(new QLabel(windowTitle(),title)); titleLayout->addStretch();
        auto* close=new QToolButton(title); close->setObjectName("outlineClose");
        close->setIcon(Codicons::icon("close")); close->setAutoRaise(true);
        close->setToolTip(I18n::text("Hide outline")); close->setAccessibleName(close->toolTip());
        titleLayout->addWidget(close); setTitleBarWidget(title);
        connect(close,&QToolButton::clicked,this,&QWidget::close);
        auto* content=new QWidget(this);
        auto* layout=new QVBoxLayout(content); layout->setContentsMargins(6,6,6,6);
        filter_=new QLineEdit(content); filter_->setObjectName("outlineFilter");
        filter_->setPlaceholderText(I18n::text("Filter symbols…"));
        auto* clear=filter_->addAction(Codicons::icon("close"),QLineEdit::TrailingPosition);
        clear->setToolTip(I18n::text("Clear filter")); clear->setVisible(false);
        connect(clear,&QAction::triggered,filter_,&QLineEdit::clear);
        connect(filter_,&QLineEdit::textChanged,clear,[clear](const QString& text){clear->setVisible(!text.isEmpty());});
        layout->addWidget(filter_);
        status_=new QLabel(content); status_->setObjectName("outlineStatus");
        status_->setWordWrap(true); layout->addWidget(status_);
        tree_=new QTreeWidget(content); tree_->setObjectName("outlineTree");
        tree_->setHeaderHidden(true); tree_->setColumnCount(1);
        tree_->setUniformRowHeights(true); tree_->setIndentation(14);
        tree_->setTextElideMode(Qt::ElideRight); layout->addWidget(tree_);
        setWidget(content); setMinimumWidth(180);
        connect(filter_,&QLineEdit::textChanged,this,[this]{filterItems();});
        connect(tree_,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* item,int){activate(item);});
        connect(tree_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* item,int){activate(item);});
        connect(&ThemeManager::instance(),&ThemeManager::themeChanged,this,[this] {
            for(int i=0;i<tree_->topLevelItemCount();++i) styleItem(tree_->topLevelItem(i));
        });
        applyFontSettings();
        showMessage(I18n::text("Open a file to see its outline."));
    }
    void applyFontSettings() {
        const auto size = FontSettings::font(FontSettings::Area::Tree).pointSizeF();
        auto widgets = findChildren<QWidget*>();
        widgets.prepend(this);
        for(auto* child : widgets) {
            auto childFont = child->font();
            childFont.setPointSizeF(size);
            child->setFont(childFont);
        }
    }
    std::function<void(int,int)> navigate;
    std::function<void()> onClosed;
    void showMessage(const QString& text) {
        tree_->clear(); tree_->setEnabled(false);
        status_->setText(text); status_->show();
    }
    void pending(bool clear) {
        if(clear) tree_->clear();
        tree_->setEnabled(false);
        status_->setText(I18n::text("Updating outline…")); status_->show();
    }
    void setSymbols(const QJsonArray& symbols,const QString& path) {
        tree_->clear(); int budget=20000;
        append(symbols,nullptr,path,0,budget);
        tree_->setEnabled(true); tree_->expandAll();
        status_->setText(I18n::text("No symbols in this file."));
        status_->setVisible(tree_->topLevelItemCount()==0);
        filterItems();
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if(onClosed) onClosed();
        QDockWidget::closeEvent(event);
    }
private:
    static const char* icon(int kind) {
        switch(kind) {
        case 2: case 3: case 4: return "symbol-namespace";
        case 5: return "symbol-class";
        case 6: return "symbol-method";
        case 7: return "symbol-property";
        case 8: return "symbol-field";
        case 9: return "symbol-constructor";
        case 10: return "symbol-enum";
        case 11: return "symbol-interface";
        case 12: return "symbol-method";
        case 14: return "symbol-constant";
        case 22: return "symbol-enum-member";
        case 23: return "symbol-struct";
        case 24: return "symbol-event";
        case 26: return "symbol-type-parameter";
        default: return "symbol-variable";
        }
    }
    void append(const QJsonArray& symbols,QTreeWidgetItem* parent,const QString& path,int depth,int& budget) {
        if(depth>64) return;
        for(const auto& value:symbols) {
            if(budget--<=0) return;
            const auto symbol=value.toObject();
            const auto location=symbol.value("location").toObject();
            if(!location.isEmpty() && QUrl(location.value("uri").toString()).toLocalFile()!=path) continue;
            auto range=symbol.value("selectionRange").toObject();
            if(range.isEmpty()) range=location.isEmpty()?symbol.value("range").toObject():location.value("range").toObject();
            const auto start=range.value("start").toObject();
            if(!start.contains("line") || !start.contains("character")) continue;
            const auto name=symbol.value("name").toString();
            if(name.isEmpty()) continue;
            const auto detail=symbol.value("detail").toString();
            auto* item=parent?new QTreeWidgetItem(parent):new QTreeWidgetItem(tree_);
            const auto label=name+(detail.isEmpty()?QString{}:"  "+detail);
            item->setText(0,label);
            item->setData(0,Qt::UserRole+1,symbol.value("kind").toInt());
            styleItem(item);
            item->setToolTip(0,label+"\n"+I18n::text("Line %1").arg(start.value("line").toInt()+1));
            item->setData(0,Qt::UserRole,QPoint(start.value("line").toInt()+1,start.value("character").toInt()+1));
            append(symbol.value("children").toArray(),item,path,depth+1,budget);
        }
    }
    void styleItem(QTreeWidgetItem* item) {
        const int kind=item->data(0,Qt::UserRole+1).toInt();
        const char* role=(kind==6 || kind==9 || kind==12)?"syntax.function"
            :(kind==5 || kind==10 || kind==11 || kind==23 || kind==26)?"syntax.type"
            :kind==14?"syntax.constant":"syntax.variable";
        const auto color=ThemeManager::instance().color(role);
        item->setForeground(0,color); item->setIcon(0,Codicons::icon(icon(kind),color));
        for(int i=0;i<item->childCount();++i) styleItem(item->child(i));
    }
    void activate(QTreeWidgetItem* item) {
        if(!tree_->isEnabled() || !navigate) return;
        const auto position=item->data(0,Qt::UserRole).toPoint();
        navigate(position.x(),position.y());
    }
    bool filterItem(QTreeWidgetItem* item) {
        bool match=item->text(0).contains(filter_->text(),Qt::CaseInsensitive);
        for(int i=0;i<item->childCount();++i) match=filterItem(item->child(i))||match;
        item->setHidden(!match);
        if(match && !filter_->text().isEmpty()) item->setExpanded(true);
        return match;
    }
    void filterItems() {
        for(int i=0;i<tree_->topLevelItemCount();++i) filterItem(tree_->topLevelItem(i));
    }
    QTreeWidget* tree_;
    QLineEdit* filter_;
    QLabel* status_;
};
