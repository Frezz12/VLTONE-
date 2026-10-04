#include "ScrollMotion.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
int failures=0;
void check(bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what);failures+=!ok;}
void wait(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
void wheel(QWidget* target,QPoint pixels,QPoint angles,Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
    const QPointF local=target->rect().center();
    QWheelEvent event(local,target->mapToGlobal(local),pixels,angles,Qt::NoButton,modifiers,Qt::NoScrollPhase,false);
    QApplication::sendEvent(target,&event);
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDir.path());
    QCoreApplication::setOrganizationName("VltTest");QCoreApplication::setApplicationName("ScrollMotion");
    ui::ScrollMotion::install();
    auto& prefs=ui::ScrollPreferences::instance();
    prefs.setEnabled(true);prefs.setReducedMotion(false);prefs.setSpeed(100);prefs.setStrength(65);
    QWidget canvas;canvas.resize(600,400);canvas.show();wait(30);
    QPointF position{1000,1000};
    auto scroll=[&](QPointF delta,bool precise=false) {
        ui::ScrollMotion::scroll(&canvas,delta,precise,[&]{return position;},
            [&](QPointF p){position={std::clamp(p.x(),0.,2000.),std::clamp(p.y(),0.,2000.)};});
    };
    for(int fps:{30,60,120}) {
        ui::FrameClock::instance().setPreference(ui::FrameMode::Fixed,fps);
        position={1000,1000};scroll({100,100});
        check(position.x()>1000 && position.x()<1100,"wheel responds before the first animation frame");
        double previous=position.x();bool monotonic=true;int frames=0;
        for(int i=0;i<12;++i){wait(20);monotonic &= position.x()>=previous&&position.x()<=1100;if(position.x()!=previous)++frames;previous=position.x();}
        check(monotonic && position==QPointF(1100,1100) && frames>=2,
              "30/60/120 FPS navigation settles exactly without overshooting either axis");
    }
    position={1000,1000};scroll({100,0});scroll({100,0});wait(220);
    check(position==QPointF(1200,1000),"repeated wheel input preserves the full requested distance");
    position={1000,1000};scroll({100,0});const double reversal=position.x();scroll({-60,0});
    check(position.x()<reversal,"a direction reversal moves immediately");
    wait(220);check(std::abs(position.x()-(reversal-60))<1e-6,"reversal cancels the previous destination");
    position={1000,1000};scroll({100,0});const auto pressed=position;
    QKeyEvent key(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&key);wait(220);
    check(position==pressed,"keyboard input cancels pending navigation without a delayed jump");
    scroll({100,0});const auto external=position+QPointF(300,0);position=external;wait(220);
    check(position==external,"programmatic navigation wins over a pending animation");
    scroll({100,0});const auto direct=position+QPointF(24,7);scroll({24,7},true);wait(220);
    check(position==direct,"precision touchpad input stays direct and cannot accumulate a second inertia tail");

    prefs.setEnabled(false);position={500,500};scroll({100,100});
    check(position==QPointF(600,600),"disabling smoothing makes movement immediate");
    prefs.setEnabled(true);prefs.setStrength(0);scroll({100,0});
    check(position==QPointF(700,600),"zero smoothing has the same immediate behavior");
    prefs.setStrength(65);prefs.setReducedMotion(true);scroll({100,0});
    check(position==QPointF(800,600),"Reduce motion disables spatial animation");
    prefs.setReducedMotion(false);
    prefs.setSpeed(50);position={1000,1000};scroll({100,0});wait(170);const auto slow=position.x();wait(200);
    prefs.setSpeed(200);position={1000,1000};scroll({100,0});wait(170);
    check(slow<1100 && position.x()==1100,"the speed preference changes deceleration time");
    prefs.setSpeed(100);prefs.setStrength(20);position={1000,1000};scroll({100,0});const auto crisp=position.x();
    ui::ScrollMotion::cancel(&canvas);prefs.setStrength(80);position={1000,1000};scroll({100,0});
    check(crisp>position.x(),"the smoothing preference changes how much movement decelerates");
    ui::ScrollMotion::cancel(&canvas);
    prefs.setStrength(65);prefs.setSpeed(100);
    check(QSettings().value("ui/smoothScroll").toBool() && QSettings().value("ui/scrollSpeed").toInt()==100 &&
          QSettings().value("ui/scrollSmoothing").toInt()==65 && !QSettings().value("ui/reduceMotion").toBool(),
          "all navigation preferences persist for the next launch");

    QListWidget list;list.resize(400,300);
    for(int i=0;i<500;++i)list.addItem(QString::number(i));
    list.show();wait(30);
    auto* bar=list.verticalScrollBar();
    const int expected=QApplication::wheelScrollLines()*bar->singleStep();
    wheel(list.viewport(),{},QPoint(0,-120));
    check(bar->value()>0 && bar->value()<expected,"native list wheels start softly");
    wait(220);check(bar->value()==expected && list.verticalScrollMode()==QAbstractItemView::ScrollPerPixel,
                   "native lists use pixels and preserve one wheel notch's distance");
    const int before=bar->value();wheel(list.viewport(),QPoint(0,-24),{});
    check(bar->value()==before+24,"native precision scrolling keeps its exact pixel distance");
    bar->setValue(bar->maximum()-2);wheel(list.viewport(),{},QPoint(0,-120));wait(220);
    check(bar->value()==bar->maximum(),"motion stops at the scroll range boundary");

    QPointF integerPosition{500,500};QWidget integerCanvas;integerCanvas.resize(200,200);integerCanvas.show();wait(20);
    for(int i=0;i<20;++i)
        ui::ScrollMotion::scroll(&integerCanvas,{.25,0},true,[&]{return integerPosition;},
            [&](QPointF p){integerPosition={std::round(p.x()),std::round(p.y())};});
    check(integerPosition.x()==505,"fractional high-resolution wheel distances survive integer scrollbars");
    auto* transient=new QWidget;transient->resize(200,200);transient->show();
    QPointF discarded{100,100};
    ui::ScrollMotion::scroll(transient,{100,0},false,[&]{return discarded;},[&](QPointF p){discarded=p;});
    delete transient;wait(220);
    check(true,"closing a scrolling window releases its frame callback safely");
    return failures ? 1 : 0;
}
