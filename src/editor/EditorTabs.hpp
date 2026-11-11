#pragma once

#include <QTabWidget>
#include <QTabBar>
#include <QEvent>
#include <QResizeEvent>
#include <algorithm>

// Keep the header gutter and the real tab bar as separate, non-overlapping
// widgets. QTabWidget's document mode otherwise lays its bar across the gutter.
class EditorTabs final : public QTabWidget {
public:
    explicit EditorTabs(QWidget* parent=nullptr) : QTabWidget(parent) {
        setObjectName("editorTabs");
        gutter_=new QWidget(this);gutter_->setObjectName("tabGutter");gutter_->hide();
        tabBar()->installEventFilter(this);
    }
    ~EditorTabs() override { tabBar()->removeEventFilter(this); }

    void setHeaderGutter(int width,int bottom) {
        gutterWidth_=std::max(0,width);headerBottom_=std::max(0,bottom);
        layoutHeader();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QTabWidget::resizeEvent(event);layoutHeader();
    }
    bool event(QEvent* event) override {
        const bool result=QTabWidget::event(event);
        if(event->type()==QEvent::LayoutRequest || event->type()==QEvent::Show || event->type()==QEvent::StyleChange)
            layoutHeader();
        return result;
    }
    bool eventFilter(QObject* watched,QEvent* event) override {
        if(watched==tabBar() && (event->type()==QEvent::Move || event->type()==QEvent::Resize || event->type()==QEvent::Show))
            layoutHeader();
        return QTabWidget::eventFilter(watched,event);
    }
    void tabInserted(int index) override { QTabWidget::tabInserted(index);layoutHeader(); }
    void tabRemoved(int index) override { QTabWidget::tabRemoved(index);layoutHeader(); }

private:
    void layoutHeader() {
        if(!gutter_ || updating_) return;
        if(count()==0) {
            // With no documents QTabWidget owns the empty tab bar's geometry.
            // Forcing a full-width bar here fights its layout and keeps posting
            // LayoutRequest/Resize events while an empty workspace is open.
            gutterWidth_=0;
            headerBottom_=0;
            gutter_->hide();
            return;
        }
        auto* bar=tabBar();
        const QSize hint=bar->sizeHint();
        if(hint.isEmpty()) return;
        updating_=true;
        const QRect barGeometry(gutterWidth_,0,std::max(0,width()-gutterWidth_),hint.height());
        const QRect gutterGeometry(0,0,gutterWidth_,headerBottom_);
        if(bar->geometry()!=barGeometry) bar->setGeometry(barGeometry);
        if(gutter_->geometry()!=gutterGeometry) gutter_->setGeometry(gutterGeometry);
        gutter_->show();
        updating_=false;
    }
    QWidget* gutter_{nullptr};
    int gutterWidth_{0};
    int headerBottom_{0};
    bool updating_{false};
};
