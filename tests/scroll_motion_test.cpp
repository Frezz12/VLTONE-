#include "ScrollMotion.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
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
void checkInputCadence(int fps, bool zoom) {
    ui::FrameClock::instance().setPreference(ui::FrameMode::Fixed,fps);
    QWidget presented, withheld;
    presented.resize(400,300);presented.show();wait(30);
    // Both receive the same timed input. Only one receives presentation
    // callbacks; the other models a GUI that handles input between late frames.
    QPointF a=zoom ? QPointF(100,16) : QPointF(1000,1000), b=a;
    const QPointF initial=a;
    const QPointF step=zoom ? QPointF(std::log(1.01),std::log(1.005)) : QPointF(4,2);
    const auto feed=[&](QWidget& owner,QPointF& value) {
        const auto read=[&value]{return value;};
        const auto write=[&value](QPointF p){value=p;};
        if(zoom) ui::ScrollMotion::zoom(&owner,{std::exp(step.x()),std::exp(step.y())},false,read,write);
        else ui::ScrollMotion::scroll(&owner,step,false,read,write);
    };
    const auto units=[&](QPointF value) {
        return zoom ? QPointF(std::log(value.x()/initial.x())/step.x(),std::log(value.y()/initial.y())/step.y())
                    : QPointF((value.x()-initial.x())/step.x(),(value.y()-initial.y())/step.y());
    };
    double maxError=0;
    constexpr int inputs=32;
    for(int i=0;i<inputs;++i) {
        feed(presented,a);feed(withheld,b);
        const auto error=units(a)-units(b);
        maxError=std::max({maxError,std::abs(error.x()),std::abs(error.y())});
        wait(8);
    }
    const auto progress=units(b);
    std::printf("INPUT_CADENCE kind=%s fps=%d max_error_notches=%.5f withheld_progress=%.3f/%.3f requested=%d\n",
                zoom ? "zoom" : "scroll",fps,maxError,progress.x(),progress.y(),inputs);
    check(maxError<.25,"continuous input follows the same trajectory with and without presentation callbacks");
    check(std::min(progress.x(),progress.y())>inputs*.75,
          "continuous wheel input advances its smoothing even when no frames are delivered");
    wait(220);
    const auto settled=units(a);
    check(std::abs(settled.x()-inputs)<1e-7 && std::abs(settled.y()-inputs)<1e-7,
          "continuous wheel input settles at the exact accumulated destination");
    // A new input after the old trajectory's deadline must consume its full
    // distance even if that owner has received no presentation callbacks.
    feed(presented,a);feed(withheld,b);
    const auto first=units(a),second=units(b);
    const double expected=inputs+.35;
    check(std::abs(first.x()-expected)<1e-7 && std::abs(first.y()-expected)<1e-7 &&
          std::abs(second.x()-expected)<1e-7 && std::abs(second.y()-expected)<1e-7,
          "a late input preserves the full prior distance without presentation callbacks");
}
void checkPanCadence(int fps) {
    ui::FrameClock::instance().setPreference(ui::FrameMode::Fixed,fps);
    QWidget shown, withheld; shown.resize(400,300); shown.show(); wait(20);
    QPointF a(500,500), b=a, destination=a;
    const auto feed=[](QWidget& owner,QPointF& value,QPointF delta) {
        ui::ScrollMotion::pan(&owner,delta,[&value]{return value;},
            [&value](QPointF at){value=at;},[](QPointF at){return at;});
    };
    double error=0;
    for(int i=0;i<32;++i) {
        const QPointF delta=i<16 ? QPointF(4.25,1.75) : QPointF(-4.25,-1.75);
        destination+=delta;
        feed(shown,a,delta);feed(withheld,b,delta);
        error=std::max(error,(a-b).manhattanLength());
        if(i==0) check(a.x()>500 && a.x()<504.25,"grab responds immediately with a short interpolated remainder");
        wait(8);
    }
    ui::ScrollMotion::finishPan(&shown);ui::ScrollMotion::finishPan(&withheld);
    check(error<1.,"grab trajectory is independent of 30/60/240 Hz presentation callbacks");
    check((a-destination).manhattanLength()<1e-8 && (b-destination).manhattanLength()<1e-8,
          "a reversed grab returns exactly to its origin even with withheld frames");
    wait(70);
    check(a==destination,"release consumes the entire drag and leaves no inertia");
    feed(shown,a,{25,12});QThread::msleep(80);feed(shown,a,{.25,.5});
    ui::ScrollMotion::finishPan(&shown);
    check(a==destination+QPointF(25.25,12.5),"late mouse input never loses the accumulated drag distance");
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
    position={1000,1000};scroll({100,0});QThread::msleep(60);
    const auto stalledReversal=position;scroll({-60,0});
    check(position.x()<stalledReversal.x(),"reversal after a missed frame never jumps in the old direction");
    wait(220);check(std::abs(position.x()-(stalledReversal.x()-60))<1e-6,
                   "reversal after a missed frame starts at the displayed position");
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
    QPointF scale{100,16};
    const auto close=[](double a,double b){return std::abs(a-b)<1e-8;};
    const auto zoom=[&](QPointF factors,bool precise=false) {
        ui::ScrollMotion::zoom(&canvas,factors,precise,[&]{return scale;},[&](QPointF p) {
            scale={std::clamp(p.x(),40.,300.),std::clamp(p.y(),8.,40.)};
        });
    };
    for(int fps:{60,240}) {
        ui::FrameClock::instance().setPreference(ui::FrameMode::Fixed,fps);
        scale={100,16};zoom({1.5,1.25});
        check(scale.x()>100 && scale.x()<150 && scale.y()>16 && scale.y()<20,
              "both zoom axes respond immediately but leave intermediate frames");
        double previous=scale.x();bool monotonic=true;
        for(int i=0;i<12;++i) {wait(20);monotonic &= scale.x()>=previous && scale.x()<=150.+1e-8;previous=scale.x();}
        check(monotonic && close(scale.x(),150) && close(scale.y(),20),
              "60/240 FPS zoom reaches the same exact scale without overshoot");
    }
    scale={100,16};zoom({1.5,1});zoom({1.5,1});wait(220);
    check(close(scale.x(),225),"successive zoom notches accumulate multiplicatively");
    scale={100,16};zoom({1.5,1});const double zoomReversal=scale.x();zoom({.8,1});
    check(scale.x()<zoomReversal,"zoom reverses on the new input without chasing the old target");
    wait(220);check(close(scale.x(),zoomReversal*.8),"zoom reversal settles at the new target");
    scale={290,16};zoom({2,1});wait(220);zoom({.8,1});wait(220);
    check(close(scale.x(),240),"zoom can reverse immediately after hitting the scale limit");
    scale={100,16};zoom({1.5,1});const double beforePrecise=scale.x();zoom({1.1,1},true);wait(220);
    check(close(scale.x(),beforePrecise*1.1),"precise zoom cancels the old animation tail");
    zoom({1.5,1});const auto beforeScroll=scale;scroll({20,0},true);wait(220);
    check(scale==beforeScroll,"panning cancels a pending zoom");
    position={500,500};scroll({100,0});const auto beforeZoom=position;zoom({1.1,1});wait(220);
    check(position==beforeZoom,"zoom cancels a pending pan in the old coordinate system");
    zoom({1.5,1});const auto beforeKey=scale;
    QApplication::sendEvent(&canvas,&key);wait(220);
    check(scale==beforeKey,"editing input interrupts zoom at the displayed scale");
    prefs.setReducedMotion(true);scale={100,16};zoom({1.5,1.25});
    check(close(scale.x(),150) && close(scale.y(),20),"Reduce motion also makes zoom immediate");
    prefs.setReducedMotion(false);
    for(int fps:{30,60,240}) {checkInputCadence(fps,false);checkInputCadence(fps,true);}
    for(int fps:{30,60,240}) checkPanCadence(fps);
    {
        QPointF at(1,1);
        const auto pan=[&](QPointF delta) {
            ui::ScrollMotion::pan(&canvas,delta,[&]{return at;},
                [&](QPointF value){at={std::round(value.x()),std::round(value.y())};},
                [](QPointF value){return QPointF(std::clamp(value.x(),0.,200.),std::clamp(value.y(),0.,200.));});
        };
        pan({-100,-100});ui::ScrollMotion::finishPan(&canvas);
        check(at.isNull(),"grab destination clamps without an invisible overscroll remainder");
        for(int i=0;i<4;++i) pan({.25,.5});
        ui::ScrollMotion::finishPan(&canvas);
        check(at==QPointF(1,2),"fractional drag survives integer scrollbars and reverses at the boundary");
        pan({100,100});const auto beforeCancel=at;
        ui::ScrollMotion::cancel(&canvas);wait(60);
        check(at==beforeCancel,"interrupted grab cancels its pending movement");
        prefs.setReducedMotion(true);at={50,50};pan({20,10});
        check(at==QPointF(70,60),"Reduce motion keeps middle-button navigation direct");
        ui::ScrollMotion::finishPan(&canvas);prefs.setReducedMotion(false);
    }
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

    // Graphics canvases use signed scrollbar ranges: zero is a scene
    // coordinate, not the upper/left navigation boundary.
    QGraphicsScene graphScene;
    QGraphicsView graph(&graphScene);
    graph.setSceneRect(-2000,-2000,4000,4000);
    graph.resize(400,300);graph.show();wait(30);
    auto* graphX=graph.horizontalScrollBar();auto* graphY=graph.verticalScrollBar();
    graphX->setValue(10);graphY->setValue(10);
    const int graphStep=QApplication::wheelScrollLines()*graphY->singleStep();
    wheel(graph.viewport(),{},QPoint(0,120));wait(220);
    check(graphY->value()==10-graphStep && graphY->value()<0,
          "canvas wheel crosses zero into negative scene coordinates");
    const int negativeY=graphY->value();
    wheel(graph.viewport(),{},QPoint(0,120));wait(220);
    check(graphY->value()==negativeY-graphStep,
          "canvas wheel keeps scrolling above the scene origin");
    wheel(graph.viewport(),{},QPoint(120,0));wait(220);
    check(graphX->value()<0,"canvas wheel also supports negative horizontal coordinates");
    graphY->setValue(graphY->minimum()+2);
    wheel(graph.viewport(),{},QPoint(0,120));wait(220);
    check(graphY->value()==graphY->minimum(),"signed canvas respects its real minimum");
    wheel(graph.viewport(),{},QPoint(0,-120));
    check(graphY->value()>graphY->minimum(),"wheel reverses immediately at a negative boundary");
    wait(220);

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
