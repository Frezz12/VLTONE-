#include "ReverbPanel.hpp"
#include "ReverbWidgets.hpp"
#include "RackParameterBinding.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "NumericFieldInteraction.hpp"
#include <QApplication>
#include <QAccessible>
#include <QAbstractButton>
#include <QDir>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QToolButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"FAIL Reverb UI: %s\n",message);std::exit(1);}
}
template<class T> T* child(QWidget& w,const char* name) {
    auto* result=w.findChild<T*>(QString::fromUtf8(name));check(result,name);return result;
}
void layoutCheck(QWidget& w) {
    QApplication::processEvents();
    for(auto* c:w.findChildren<QWidget*>()) {
        if(!c->isVisible() || (!qobject_cast<QAbstractButton*>(c) && !qobject_cast<QDoubleSpinBox*>(c) && !qobject_cast<ReverbSlider*>(c)))continue;
        check(w.rect().contains(QRect(c->mapTo(&w,QPoint()),c->size())),"controls fit inside editor");
        check(!c->accessibleName().isEmpty(),"controls have accessible names");
        check(c->focusPolicy()!=Qt::NoFocus,"controls are keyboard reachable");
    }
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);QTemporaryDir temp;
    app.setProperty("dawHeadlessDataRoot",temp.path());app.setOrganizationName("VLTONE-Reverb-Test");app.setApplicationName("Reverb UI");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
    ui::initializeApplicationFonts();ui::installNumericFieldInteraction(app);QTranslator translator;
    const auto translation=qEnvironmentVariable("VLT_REVERB_TRANSLATION");
    if(!translation.isEmpty()){check(translator.load(translation),"translation loads");app.installTranslator(&translator);}
    daw::EngineController controller;check(bool(controller.initialize(48000,256,false)),"engine initializes");
    const auto track=controller.addTrack(daw::TrackKind::Audio,"Reverb test");
    const auto slot=controller.addInsert(track,daw::plugins::reverb::ReverbInstance::staticDescriptor());
    ReverbPanel panel(&controller,QString::fromStdString(track),QString::fromStdString(slot));panel.show();QApplication::processEvents();
    auto value=[&](const char* id){return controller.insertParameter(track,slot,id);};
    auto snapshot=[&]{auto s=controller.reverbSnapshot(track,slot);check(bool(s),"snapshot available");return s->state;};
    auto* field=child<ReverbCanvas>(panel,"ReverbField");
    const auto start=field->point().toPoint()+QPoint(6,-3),end=start+QPoint(40,-30);
    QTest::mousePress(field,Qt::LeftButton,Qt::NoModifier,start);
    check(value("size")==65 && value("brightness")==45,"grab keeps initial offset");
    QMouseEvent move(QEvent::MouseMove,end,field->mapToGlobal(end),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(field,&move);QTest::mouseRelease(field,Qt::LeftButton,Qt::NoModifier,end);
    check(value("size")>65 && value("brightness")>45 && value("decay")==4.2,"XY edits both axes without changing decay");
    const auto changed=snapshot().values;controller.undo();panel.refresh();
    check(value("size")==65 && value("brightness")==45,"one undo restores both XY axes");
    controller.redo();panel.refresh();check(snapshot().values==changed,"XY redo restores both axes");
    QTest::keyClick(field,Qt::Key_Home);check(std::abs(value("size")-65)<1.e-8 && std::abs(value("brightness")-45)<1.e-8,"Home resets XY");
    QTest::keyClick(field,Qt::Key_Right,Qt::ShiftModifier);check(std::abs(value("size")-65.1)<1.e-8,"Shift provides fine keyboard control");
    auto* number=child<QDoubleSpinBox>(panel,"ReverbValue.brightness");number->setFocus();number->selectAll();
    QTest::keyClicks(number,number->locale().toString(72.5,'f',1));QTest::keyClick(number,Qt::Key_Return);
    if(value("brightness")!=72.5)std::fprintf(stderr,"numeric brightness %.8f, displayed %s\n",value("brightness"),number->text().toUtf8().constData());
    check(value("brightness")==72.5,"numeric XY entry");controller.undo();panel.refresh();check(std::abs(value("brightness")-45)<1.e-8,"numeric edit undo");

    panel.applyFactoryPreset(0);
    auto* mix=child<ReverbSlider>(panel,"ReverbSlider.mix");
    const auto grip=mix->handlePoint().toPoint()+QPoint(3,0);
    QTest::mousePress(mix,Qt::LeftButton,Qt::NoModifier,grip);check(value("mix")==25,"custom slider grab keeps offset");
    const auto mixEnd=grip+QPoint(40,0);
    QMouseEvent sliderMove(QEvent::MouseMove,mixEnd,mix->mapToGlobal(mixEnd),Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(mix,&sliderMove);
    const double fineValue=value("mix");check(std::abs(fineValue-25-4/mix->trackRect().width()*100)<1.e-7,"Shift scales drag without jumping");
    QMouseEvent reverse(QEvent::MouseMove,grip,mix->mapToGlobal(grip),Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(mix,&reverse);check(std::abs(value("mix")-25)<1.e-7,"slider direction reverses immediately");
    QApplication::sendEvent(mix,&sliderMove);QTest::mouseRelease(mix,Qt::LeftButton,Qt::ShiftModifier,mixEnd);
    controller.undo();panel.refresh();check(value("mix")==25 && mix->plainValue()==25,"one undo restores entire slider gesture");
    QTest::keyClick(mix,Qt::Key_Right,Qt::ShiftModifier);check(std::abs(value("mix")-25.1)<1.e-7,"custom slider fine keyboard control");
    auto* accessible=QAccessible::queryAccessibleInterface(mix);check(accessible&&accessible->valueInterface(),"custom slider exposes accessible value interface");
    check(std::abs(accessible->valueInterface()->currentValue().toDouble()-25.1)<1.e-7,"accessibility reports plain value");
    QTest::keyClick(mix,Qt::Key_Return);mix->number()->selectAll();QTest::keyClicks(mix->number(),mix->number()->locale().toString(42.5,'f',1));QTest::keyClick(mix->number(),Qt::Key_Return);
    check(value("mix")==42.5,"custom slider numeric entry");controller.undo();panel.refresh();check(std::abs(value("mix")-25.1)<1.e-7,"numeric slider entry is undoable");
    panel.applyFactoryPreset(0);
    auto* response=child<ReverbCanvas>(panel,"ReverbResponse");const auto decayStart=response->fieldRect().center().toPoint();
    QTest::mousePress(response,Qt::LeftButton,Qt::NoModifier,decayStart);check(value("decay")==4.2,"response grab does not jump");
    QTest::mouseRelease(response,Qt::LeftButton,Qt::NoModifier,decayStart+QPoint(20,0));
    check(value("decay")>4.2 && value("size")==65 && value("brightness")==45,"response edits decay independently of XY");
    controller.undo();panel.refresh();check(value("decay")==4.2,"response gesture undo");
    child<QAbstractButton>(panel,"ReverbEarly")->click();QTest::keyClick(response,Qt::Key_Right,Qt::ShiftModifier);check(std::abs(value("preDelay")-30.1)<1.e-7,"early response keyboard edits predelay");
    child<QAbstractButton>(panel,"ReverbTail")->click();panel.applyFactoryPreset(0);
    child<QAbstractButton>(panel,"ReverbB")->click();check(snapshot().comparison=='B',"select B");
    RackParameterBinding rack(&controller,QString::fromStdString(track),QString::fromStdString(slot),nullptr);
    rack.write("decay",8);rack.finish("decay");panel.refresh();
    check(value("decay")==8,"rack and editor share parameters");
    child<QAbstractButton>(panel,"ReverbA")->click();check(value("decay")==4.2 && snapshot().comparison=='A',"A restores original settings");
    controller.undo();panel.refresh();check(value("decay")==8 && snapshot().comparison=='B',"A/B undo restores values and active side");
    controller.redo();panel.refresh();check(value("decay")==4.2 && snapshot().comparison=='A',"A/B redo");
    child<QAbstractButton>(panel,"ReverbCopy")->click();check(snapshot().b==snapshot().values,"Copy writes the other bank");
    controller.undo();panel.refresh();check(snapshot().b[4]==8,"Copy undo restores other bank");
    panel.applyFactoryPreset(5);check(value("mode")==5 && value("decay")!=4.2,"preset changes complete settings");
    child<QAbstractButton>(panel,"ReverbMode.1")->click();check(value("mode")==1 && value("decay")!=4.2,"space selection preserves knobs");
    child<QAbstractButton>(panel,"ReverbAdvanced")->click();

    auto* presets=child<QToolButton>(panel,"ReverbPreset");
    check(presets->height()==44,"preset caption and name retain full two-line height");
    QMetaObject::invokeMethod(presets->menu(),"aboutToShow",Qt::DirectConnection);
    QTimer::singleShot(0,[]{for(auto* w:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(w)){dialog->setTextValue("User test");dialog->accept();}});
    const auto userValues=snapshot().values;
    presets->menu()->actions().back()->trigger();check(snapshot().presetKind=="user" && snapshot().presetName=="User test","user preset saves");
    panel.applyFactoryPreset(1);QMetaObject::invokeMethod(presets->menu(),"aboutToShow",Qt::DirectConnection);
    for(auto* action:presets->menu()->actions())if(action->text()=="User test")action->trigger();
    check(snapshot().values==userValues,"user preset reloads full values");
    check(!child<ReverbSlider>(panel,"ReverbSlider.shimmerAmount")->isEnabled(),"Shimmer Amount disabled outside Shimmer");
    child<QAbstractButton>(panel,"ReverbMode.5")->click();check(child<ReverbSlider>(panel,"ReverbSlider.shimmerAmount")->isEnabled(),"Shimmer Amount enabled");
    child<QAbstractButton>(panel,"ReverbFreeze")->click();check(value("freeze")==1,"Freeze automatable control");
    child<QAbstractButton>(panel,"ReverbFreeze")->click();
    child<QAbstractButton>(panel,"ReverbAdvanced")->click();

    check(controller.setInsertChannelMode(track,slot,daw::PluginChannelMode::DualMono),"Dual Mono supported");
    controller.setInsertEditorChannel(track,slot,daw::PluginEditorChannel::Right);
    controller.setInsertParameter(track,slot,"mix",73);
    controller.setInsertEditorChannel(track,slot,daw::PluginEditorChannel::Left);
    check(value("mix")!=73,"Dual Mono channels independent");
    controller.setInsertEditorChannel(track,slot,daw::PluginEditorChannel::Right);check(value("mix")==73,"right channel retained");
    check(controller.setInsertChannelMode(track,slot,daw::PluginChannelMode::Stereo),"return to stereo");
    controller.setInsertEditorChannel(track,slot,daw::PluginEditorChannel::Left);
    const auto detector=controller.addTrack(daw::TrackKind::Audio,"Detector");
    check(controller.insertSupportsSidechain(track,slot) && controller.setInsertSidechainSource(track,slot,detector),"external sidechain routing");
    const auto saved=snapshot();const auto project=(temp.path()+"/Reverb.vlt").toStdString();
    check(bool(controller.saveProject(project)),"project saves");
    { daw::EngineController restored;check(bool(restored.initialize(48000,256,false)),"restore engine");
      check(bool(restored.openProject(project)),"project opens");const auto state=restored.reverbSnapshot(track,slot);
      check(state && state->state.values==saved.values && state->state.a==saved.a && state->state.b==saved.b && state->state.comparison==saved.comparison && state->state.presetName==saved.presetName,"project preserves parameters, A/B and preset"); }

    const auto output=qEnvironmentVariable("VLT_REVERB_SCREENSHOTS");if(!output.isEmpty())QDir().mkpath(output);
    panel.applyFactoryPreset(0);
    for(const auto* theme:{"studio-gray","light"}) {
        ThemeManager::instance().setThemeId(theme,false);
        for(bool compact:{false,true}) {
            panel.resize(compact?QSize(840,660):QSize(1120,716));panel.refresh();layoutCheck(panel);
            for(const auto* id:{"preDelay","decay","width","mix","bloom","motion","duck"})
                check(!child<ReverbSlider>(panel,(QString("ReverbSlider.")+id).toUtf8().constData())->displayText().isEmpty(),"main sliders show editable values permanently");
            if(!output.isEmpty())check(panel.grab().save(output+QString("/%1-%2.png").arg(theme,compact?"minimum":"full")),"screenshot saves");
        }
        child<QAbstractButton>(panel,"ReverbAdvanced")->click();layoutCheck(panel);
        if(!output.isEmpty())panel.grab().save(output+QString("/%1-advanced.png").arg(theme));
        child<QAbstractButton>(panel,"ReverbAdvanced")->click();
    }
    auto visual=*controller.reverbSnapshot(track,slot);visual.telemetry.tail=.25f;field->setSnapshot(visual,false);
    check(field->animationActive(),"audible tail starts cloud animation");visual.state.values[10]=1;field->setSnapshot(visual,false);check(!field->animationActive(),"Freeze holds cloud contours");visual.state.values[10]=0;
    field->setSnapshot(visual,true);check(!field->animationActive(),"reduced motion stops cloud animation");
    field->setSnapshot(visual,false);panel.hide();check(!field->animationActive() && !panel.findChild<QTimer*>("ReverbTelemetryTimer")->isActive(),"hidden editor stops visual work");
    panel.show();controller.removeInsert(track,slot);panel.refresh();check(!panel.isEnabled(),"removed instance disables stale editor");
    controller.addInsert(track,daw::plugins::reverb::ReverbInstance::staticDescriptor());panel.refresh();check(!panel.isEnabled(),"replacement never reconnects stale editor");
    std::puts("PASS Reverb UI: XY/Undo, custom sliders, response gestures, numeric and keyboard input, A/B/Copy, presets, rack sync, channel routing, project state, themes, accessibility, reduced motion and deletion");
}
