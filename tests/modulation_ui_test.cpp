#include "EngineController.hpp"
#include "ModulationPanel.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "graphics/BrowserSurface.hpp"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEngineView>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <cmath>
#include <cstdio>
#include <memory>
namespace mod = daw::plugins::modulation;
namespace {
int failures = 0;
void check(bool ok, const char* text) { std::printf("%s %s\n",ok?"PASS":"FAIL",text); if(!ok)++failures; }
void events(int ms=70) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
QVariant js(ui::graphics::BrowserPage* page, const QString& code) {
    struct Result {bool done=false; QVariant value;}; auto result=std::make_shared<Result>();
    page->runJavaScript(code,[result](const QVariant& v){result->value=v;result->done=true;});
    for(int i=0;i<100&&!result->done;++i)events(10);
    check(result->done,"JavaScript completes"); events(); return result->value;
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ui::registerFontUrlScheme(); QtWebEngineQuick::initialize();
    QApplication app(argc,argv); ui::initializeApplicationFonts(); QTemporaryDir temporary;
    app.setProperty("dawHeadlessDataRoot",temporary.path());
    app.setOrganizationName("VLTONE-Modulation-Test");app.setApplicationName("ModulationWeb");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temporary.path());
    QString screenshots;
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--screenshots"){screenshots=QString::fromLocal8Bit(argv[2]);QDir().mkpath(screenshots);}
    daw::EngineController controller;check(bool(controller.initialize(48000,257,false)),"controller initializes");
    for(int k=0;k<=mod::kindCount;++k) {
        const bool rack=k==mod::kindCount;
        const auto& desc=rack?mod::ModulationRackInstance::staticDescriptor():mod::descriptorFor(mod::Kind(k));
        const auto track=controller.addTrack(daw::TrackKind::Audio,desc.name),insert=controller.addInsert(track,desc);
        check(!insert.empty(),"plugin inserts through host"); if(insert.empty())continue;
        ModulationPanel panel(&controller,QString::fromStdString(track),QString::fromStdString(insert));
        panel.resize(rack?1040:440,rack?530:k==4?660:530);panel.show();
        auto* view=panel.findChild<ui::graphics::BrowserSurface*>("ModulationWebView");
        check(view!=nullptr,"HTML editor uses shared Chromium surface");if(!view)continue;
        auto* page=view->page();bool ready=false;
        for(int i=0;i<40&&!ready;++i)ready=js(page,"document.documentElement.dataset.connected === 'true' && document.querySelectorAll('.card').length > 0").toBool();
        check(ready,"local resources and WebChannel initialize without a server");if(!ready)continue;
        events(250);
        const auto initial=panel.grab().toImage();
        check(!initial.isNull() && initial.pixelColor(initial.width()/2,initial.height()/3).lightness()>120 &&
              initial.pixelColor(initial.width()/2,8).lightness()<100,"Chromium paints the light cards and dark header");
        if(!screenshots.isEmpty())initial.save(screenshots+"/"+QString::fromStdString(desc.name).remove(' ')+"-initial.png");
        check(js(page,QString("document.querySelectorAll('.card').length === %1").arg(rack?4:1)).toBool(),"correct number of module cards");
        check(js(page,"document.documentElement.scrollWidth <= innerWidth && document.documentElement.scrollHeight <= innerHeight").toBool(),"default panel has no scrollbars or clipped controls");
        auto* plugin=controller.insertInstance(track,insert);
        const unsigned mainIndex=rack?1:0;
        const auto id=plugin->parameters()[mainIndex].id;
        const auto value=[&]{return controller.insertParameter(track,insert,id);};
        const double before=value();
        js(page,QString("document.getElementById('value-%1').value='37';document.getElementById('value-%1').dispatchEvent(new Event('change'))").arg(mainIndex));
        check(std::abs(value()-.37)<1.e-6,"HTML numeric input edits actual audio parameter");
        controller.undo();events();check(std::abs(value()-before)<1.e-6,"numeric input is one undo step");
        js(page,QString("document.getElementById('param-%1').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true}))").arg(mainIndex));
        check(value()>before,"keyboard adjusts round slider");controller.undo();events();
        QString routed;QObject::connect(&panel,&ModulationPanel::automationRequested,[&](QString parameter){routed=parameter;});
        js(page,QString("document.getElementById('param-%1').dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,clientX:100,clientY:150}));document.getElementById('automate').click()").arg(mainIndex));
        check(routed==QString::fromStdString(id),"automation uses stable identity");
        if(auto* native=view->findChild<QWebEngineView*>()) {
            const auto position=js(page,QString("(()=>{let r=document.getElementById('param-%1').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").arg(mainIndex)).toList();
            const QPoint center(qRound(position[0].toDouble()),qRound(position[1].toDouble()));
            auto* receiver=native->focusProxy();if(!receiver)receiver=native;
            controller.play();events();
            QTest::mousePress(receiver,Qt::LeftButton,Qt::NoModifier,center);events();
            bool monotonic=true;double last=value();
            for(int step=1;step<=12;++step){QTest::mouseMove(receiver,center-QPoint(0,step*3),15);events(35);monotonic&=value()>=last;last=value();}
            QTest::mouseRelease(receiver,Qt::LeftButton,Qt::NoModifier,center-QPoint(0,36));events();
            check(monotonic&&value()>before+.12,"pointer drag during playback remains anchored across telemetry updates");
            controller.stop();controller.undo();events();check(std::abs(value()-before)<1.e-6,"entire drag undoes in one step");
        }
        auto* bridge=panel.findChild<ModulationWebBridge*>();
        check(bridge->savePreset("My modulation",false).isEmpty(),"user preset saves");
        bridge->edit(mainIndex,.11,true);bridge->loadPreset("My modulation");events();
        check(std::abs(value()-before)<1.e-6,"user preset restores values");
        check(!bridge->savePreset("My modulation",false).isEmpty(),"accidental preset overwrite rejected");
        check(bridge->renamePreset("My modulation","Renamed").isEmpty(),"user preset renames");
        if(rack){
            if(auto* native=view->findChild<QWebEngineView*>()) {
                auto* receiver=native->focusProxy();if(!receiver)receiver=native;
                const auto rect=js(page,"(()=>{const r=document.querySelector('.grab').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").toList();
                const QPoint start(qRound(rect[0].toDouble()),qRound(rect[1].toDouble()));
                QTest::mousePress(receiver,Qt::LeftButton,Qt::NoModifier,start);
                for(int i=1;i<=10;++i)QTest::mouseMove(receiver,start+QPoint(i*52,0),10);
                QTest::mouseRelease(receiver,Qt::LeftButton,Qt::NoModifier,start+QPoint(520,0));events();
                check(plugin->parameterValue(mod::ModulationRackInstance::orderParameter)!=0,"real pointer drag reorders modules");
                controller.undo();events();check(plugin->parameterValue(mod::ModulationRackInstance::orderParameter)==0,"pointer reorder is one undo step");
            }
            check(js(page,"document.getElementById('eq-content').hidden").toBool(),"EQ initially collapsed");
            js(page,"document.getElementById('eq-toggle').click()");
            check(js(page,"!document.getElementById('eq-content').hidden && document.getElementById('eq-toggle').getAttribute('aria-expanded')==='true'").toBool(),"EQ disclosure opens accessibly");
            panel.resize(1040,800);events(300);js(page,"scrollTo(0,0)");
            if(auto* native=view->findChild<QWebEngineView*>()) {
                auto* receiver=native->focusProxy();if(!receiver)receiver=native;
                const auto rect=js(page,"(()=>{const r=document.querySelector('[data-band=\"1\"]').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").toList();
                const QPoint start(qRound(rect[0].toDouble()),qRound(rect[1].toDouble()));
                const double oldFrequency=plugin->parameterValue(26),oldGain=plugin->parameterValue(27);
                QTest::mousePress(receiver,Qt::LeftButton,Qt::NoModifier,start);
                QTest::mouseMove(receiver,start+QPoint(20,-30),30);QTest::mouseRelease(receiver,Qt::LeftButton,Qt::NoModifier,start+QPoint(20,-30));events();
                check(plugin->parameterValue(26)>oldFrequency&&plugin->parameterValue(27)>oldGain,"real EQ drag edits frequency and gain together");
                controller.undo();events();check(plugin->parameterValue(26)==oldFrequency&&plugin->parameterValue(27)==oldGain,"EQ drag restores both coordinates in one undo");
            }
            const auto orderBefore=plugin->parameterValue(mod::ModulationRackInstance::orderParameter);
            js(page,"document.querySelector('.grab').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight',altKey:true,bubbles:true}))");
            check(plugin->parameterValue(mod::ModulationRackInstance::orderParameter)!=orderBefore,"keyboard reordering changes DSP chain");
            controller.undo();events();check(plugin->parameterValue(mod::ModulationRackInstance::orderParameter)==orderBefore,"reordering undoes once");
            bridge->reorder(0,3);events();check(js(page,"document.querySelector('.card:last-child').dataset.module === '0'").toBool(),"visual order follows host order");
            js(page,"document.querySelector('[data-module=\"2\"] .power').click()");
            check(plugin->parameterValue(mod::ModulationRackInstance::offsets[2])==0,"module power controls correct identity after reorder");
            js(page,"document.querySelector('[data-band=\"1\"]').dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true}))");
            check(plugin->parameterValue(27)==1,"EQ handle keyboard movement changes gain");controller.undo();events();
            js(page,"document.getElementById('eq-band').value='0';document.getElementById('eq-band').dispatchEvent(new Event('change'));document.getElementById('eq-frequency').value='120';document.getElementById('eq-frequency').dispatchEvent(new Event('change'))");
            check(plugin->parameterValue(21)==1&&plugin->parameterValue(22)==120,"low-cut inspector enables actual filter");
            js(page,"document.getElementById('eq-band').value='5';document.getElementById('eq-band').dispatchEvent(new Event('change'));document.getElementById('eq-frequency').value='6000';document.getElementById('eq-frequency').dispatchEvent(new Event('change'))");
            check(plugin->parameterValue(41)==1&&plugin->parameterValue(42)==6000,"high-cut inspector enables actual filter");
            if(!screenshots.isEmpty())panel.grab().save(screenshots+"/Modulation-expanded.png");
            js(page,"document.getElementById('eq-toggle').click()");panel.resize(1040,530);events(300);js(page,"scrollTo(0,0)");
        } else {
            panel.applyFactoryPreset(9);events();bool match=true;
            for(const auto& info:plugin->parameters())match&=std::abs(plugin->parameterValue(info.index)-mod::factoryPresets(mod::Kind(k))[9].values[info.index])<1.e-6;
            check(match,"all standalone factory parameters preserved");controller.undo();events();
        }
        if(!screenshots.isEmpty())panel.grab().save(screenshots+"/"+QString::fromStdString(desc.name).remove(' ')+".png");
        panel.resize(rack?800:360,rack?490:k==4?650:490);events();
        check(js(page,"document.documentElement.scrollWidth <= innerWidth").toBool(),"minimum supported width has no horizontal overflow");
        panel.hide();events();check(!panel.visualUpdatesActive(),"hidden panel stops timer");panel.show();events();check(panel.visualUpdatesActive(),"shown panel resumes updates");
        const auto path=(temporary.path()+"/state.vlt").toStdString();
        check(bool(controller.saveProject(path)),"project saves HTML-controlled plugin state");
        std::vector<std::uint8_t> saved;plugin->saveState(saved);
        check(bool(controller.openProject(path)),"project reopens");
        auto* restored=controller.insertInstance(track,insert);std::vector<std::uint8_t> loaded;
        if(restored)restored->saveState(loaded);
        check(restored && loaded==saved,"reopened project preserves all module, order and EQ state");
        panel.hide();bridge->deletePreset("Renamed");
    }
    std::printf("%d failures\n",failures);return failures?1:0;
}
