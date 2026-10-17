#include "Vlt1073Panel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Recording/RecordingEngine.hpp"
#include "cloud/PublishPreflight.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Typography.hpp"
#include <QAccessible>
#include <QApplication>
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace {
int failures=0;
void check(bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what);failures+=!ok;}
void events(int ms=40){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
void mouse(QWidget* w,QEvent::Type type,QPoint local,Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers mods=Qt::NoModifier){QMouseEvent e(type,QPointF(local),QPointF(w->mapToGlobal(local)),button,buttons,mods);QApplication::sendEvent(w,&e);}
QWidget* readout(){for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->windowType()==Qt::ToolTip&&!w->accessibleName().isEmpty())return w;return nullptr;}
class PaintWatch final:public QObject {
public:
    QRegion region;
    bool eventFilter(QObject*,QEvent* e)override{if(e->type()==QEvent::Paint)region+=static_cast<QPaintEvent*>(e)->region();return false;}
};
double difference(const std::vector<float>& a,const std::vector<float>& b){if(a.size()!=b.size()||a.empty())return 1e9;double e=0;for(unsigned i=0;i<a.size();++i)e=std::max(e,std::abs(double(a[i])-b[i]));return e;}
}
int main(int argc,char** argv){
    std::setvbuf(stdout,nullptr,_IONBF,0);ui::registerFontUrlScheme();QApplication app(argc,argv);ui::initializeApplicationFonts();QTemporaryDir temp;app.setProperty("dawHeadlessDataRoot",temp.path());
    app.setOrganizationName("VLTONE-1073-Test");app.setApplicationName("Vlt1073Native");QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
    daw::EngineController controller;check(bool(controller.initialize(48000,257,false)),"headless engine initializes");
    const auto descriptor=controller.pluginManager().find(daw::plugins::Format::Internal,"daw.1073");check(descriptor&&descriptor->name=="VLT 1073","browser discovers VLT 1073");if(!descriptor)return 1;
    // Compare complete clips: cutting a longer source at an export boundary
    // intentionally changes the FIR's final look-ahead samples after PDC.
    audio::AudioBuffer source(2,48000);for(unsigned i=0;i<source.numFrames();++i)source.getChannel(0)[i]=source.getChannel(1)[i]=float(.12589254*std::sin(2*std::numbers::pi*431*i/48000));
    const auto sourcePath=temp.path().toStdString()+"/source.wav";audio::AudioRecorder writer;writer.initialize(48000,2);writer.writeWAVFile(sourcePath,source,48000);
    const auto track=controller.importAudioToNewTrack(sourcePath,0);const auto insert=controller.addInsert(track,*descriptor);check(!insert.empty(),"insert uses standard plugin host");if(insert.empty())return 1;
    check(daw::cloud::isSupportedBuiltinV1(*controller.insertModel(track,insert)),"cloud built-in allowlist includes VLT 1073");
    controller.undo();check(!controller.insertInstance(track,insert),"insertion undo");controller.redo();check(controller.insertInstance(track,insert),"insertion redo");
    Vlt1073Panel panel(&controller,QString::fromStdString(track),QString::fromStdString(insert));panel.resize(315,743);panel.show();events();
    const auto value=[&](const char* id){return controller.insertParameter(track,insert,id);};
    auto* high=panel.findChild<ui::Knob*>("highGain");auto* mid=panel.findChild<ui::Knob*>("midGain");auto* frequency=panel.findChild<ui::Knob*>("midFrequency");auto* power=panel.findChild<QAbstractButton*>("Vlt1073Power");
    check(high&&mid&&frequency&&power,"native controls exist");if(!high||!mid||!frequency||!power)return 1;
    if(auto* focused=QApplication::focusWidget())focused->clearFocus();events();const auto unfocusedFace=panel.grab().toImage();bool cleanFocus=true;
    for(auto* w:panel.findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(w->isVisible()&&w->isEnabled()){w->setFocus(Qt::MouseFocusReason);cleanFocus&=panel.grab().toImage()==unfocusedFace;}
    check(cleanFocus,"all controls retain the physical face without focus outlines");
    auto* accessible=QAccessible::queryAccessibleInterface(high);auto depth=controller.undoDepth();
    check(accessible&&accessible->valueInterface()&&accessible->role()==QAccessible::Slider,"accessible value interface");
    if(accessible&&accessible->valueInterface())accessible->valueInterface()->setCurrentValue(3.5);
    check(value("highGain")==3.5&&controller.undoDepth()==depth+1,"assistive edit is undoable");controller.undo();events();controller.redo();check(value("highGain")==3.5,"parameter redo");controller.undo();events();
    const QPoint start=high->rect().center();depth=controller.undoDepth();
    mouse(high,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);mouse(high,QEvent::MouseMove,start+QPoint(0,-40),Qt::NoButton,Qt::LeftButton);
    const double coarse=value("highGain");mouse(high,QEvent::MouseMove,start+QPoint(0,-50),Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier);
    check(std::abs(value("highGain")-coarse-.5)<1e-8,"Shift changes drag precision immediately");mouse(high,QEvent::MouseButtonRelease,start+QPoint(0,-50),Qt::LeftButton,Qt::NoButton);
    check(controller.undoDepth()==depth+1,"one drag creates one undo");controller.undo();events();
    auto* level=panel.findChild<ui::Knob*>("level");const QPoint levelStart=level->rect().center();depth=controller.undoDepth();
    mouse(level,QEvent::MouseButtonPress,levelStart,Qt::LeftButton,Qt::LeftButton);events();
    check(readout()&&readout()->accessibleName()=="+0.0 dB","fader immediately displays its exact starting value");
    const QPoint bubbleStart=readout()?readout()->pos():QPoint();
    mouse(level,QEvent::MouseMove,levelStart+QPoint(0,40),Qt::NoButton,Qt::LeftButton);events();
    const auto levelText=QString::fromStdString(controller.insertInstance(track,insert)->parameterText(14,value("level")));
    check(value("level")<0&&readout()&&readout()->accessibleName()==levelText&&readout()->pos().y()>bubbleStart.y(),"fader readout follows the cap and current dB value during drag");
    mouse(level,QEvent::MouseButtonRelease,levelStart+QPoint(0,40),Qt::LeftButton,Qt::NoButton);events();
    check(!readout()&&controller.undoDepth()==depth+1,"releasing fader hides the readout and commits one undo");controller.undo();events();
    controller.setInsertParameter(track,insert,"level",-90);events();mouse(level,QEvent::MouseButtonPress,levelStart,Qt::LeftButton,Qt::LeftButton);events();
    check(readout()&&readout()->accessibleName().contains(QString::fromUtf8("−∞")),"muted fader displays minus infinity");QTest::keyClick(level,Qt::Key_Escape);check(!readout(),"Escape dismisses the live readout");controller.setInsertParameter(track,insert,"level",0);events();
    depth=controller.undoDepth();mouse(high,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);mouse(high,QEvent::MouseMove,start+QPoint(0,-30),Qt::NoButton,Qt::LeftButton);QTest::keyClick(high,Qt::Key_Escape);
    std::printf("MEASURE Escape value %.8g, undo %zu -> %zu, widget %.8g\n",value("highGain"),size_t(depth),size_t(controller.undoDepth()),high->value());
    check(value("highGain")==0&&controller.undoDepth()==depth,"Escape restores pre-gesture value without undo");
    QTest::keyClick(high,Qt::Key_Right);check(std::abs(value("highGain")-.1)<1e-10,"keyboard responds directly");controller.undo();events();
    QTimer::singleShot(0,[&]{auto* d=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());check(d,"numeric entry opens");if(d){d->setDoubleValue(4.75);d->accept();}});QTest::keyClick(high,Qt::Key_Return);
    check(value("highGain")==4.75,"Enter accepts precise numeric value");controller.undo();events();
    QString automated;QObject::connect(&panel,&Vlt1073Panel::automationRequested,[&](QString id){automated=id;});
    QTimer::singleShot(0,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu)return;for(auto* a:menu->actions())if(a->objectName()=="Vlt1073Automate"){QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,menu->actionGeometry(a).center());return;}menu->close();});
    QContextMenuEvent context(QContextMenuEvent::Mouse,high->rect().center(),high->mapToGlobal(high->rect().center()));QApplication::sendEvent(high,&context);check(automated=="highGain","context automation preserves parameter ID");
    check(panel.childAt(mid->geometry().center())==mid&&!frequency->mask().contains(frequency->rect().center()),"concentric centre and annulus are separate hit regions");
    const auto midBefore=value("midGain");QTest::keyClick(frequency,Qt::Key_Right);check(value("midFrequency")==2&&value("midGain")==midBefore,"outer ring changes frequency only");controller.undo();events();
    auto* hz=panel.findChild<QAbstractButton*>("Vlt1073_midFrequency_5");check(hz,"clickable frequency label exists");if(hz)hz->click();check(value("midFrequency")==5,"frequency label selects 4800 Hz");controller.undo();events();
    auto* mic=panel.findChild<QAbstractButton*>("Vlt1073_inputMode_1");auto* line=panel.findChild<QAbstractButton*>("Vlt1073_inputMode_0");mic->click();controller.setInsertParameter(track,insert,"micGain",65);line->click();mic->click();
    check(value("micGain")==65&&value("lineGain")==0,"MIC and LINE retain separate gains");line->click();controller.setInsertParameter(track,insert,"micGain",20);
    PaintWatch paint;panel.installEventFilter(&paint);
    for(auto* mode:{line,mic}){
        mode->click();events();
        for(const char* id:{"micZ","pad"}){
            auto* toggle=panel.findChild<ui::Knob*>(id);const double before=value(id);const auto pixels=panel.grab(toggle->geometry()).toImage();events();paint.region={};
            QTest::mouseClick(toggle,Qt::LeftButton);events();
            check(value(id)==1-before&&toggle->value()==1-before&&paint.region.intersects(toggle->geometry()),"MIC Z/PAD toggle and repaint in both LINE and MIC");
            check(panel.grab(toggle->geometry()).toImage()!=pixels,"switch cap visibly moves between LO/HI or 0/-20");controller.undo();
            check(QTest::qWaitFor([&]{return toggle->value()==before;},250)&&panel.grab(toggle->geometry()).toImage()==pixels,"switch position follows undo");
        }
    }
    line->click();events();const auto poweredFace=panel.grab(power->geometry()).toImage();events();paint.region={};
    power->click();events();check(controller.insertModel(track,insert)->bypassed,"POWER controls host bypass");
    check(paint.region.intersects(power->geometry())&&panel.grab(power->geometry()).toImage()!=poweredFace,"POWER repaints its cap immediately when bypass changes");
    controller.undo();check(QTest::qWaitFor([&]{return power->isChecked();},250),"POWER follows host undo");
    check(panel.grab(power->geometry()).toImage()==poweredFace,"POWER restores its visible ON position after undo");panel.removeEventFilter(&paint);
    controller.setInsertParameter(track,insert,"highGain",6.25);controller.setInsertParameter(track,insert,"lowFrequency",4);controller.setInsertParameter(track,insert,"level",-7);
    const auto strip=controller.copyChannelStrip(track,false);const auto target=controller.addTrack(daw::TrackKind::Audio,"Copy");check(controller.pasteChannelInserts(target,strip),"effect copies using opaque state");
    const auto copied=controller.project().findTrack(target)->inserts.front().id;check(controller.insertParameter(target,copied,"highGain")==6.25&&controller.insertParameter(target,copied,"lowFrequency")==4&&controller.insertParameter(target,copied,"level")==-7,"copied parameters preserved");
    const auto projectPath=temp.path().toStdString()+"/vlt1073.vlt";check(bool(controller.saveProject(projectPath)),"project saves");{daw::EngineController restored;restored.initialize(48000,257,false);check(bool(restored.openProject(projectPath))&&restored.insertParameter(track,insert,"highGain")==6.25&&restored.insertParameter(track,insert,"level")==-7,"project restores effect state");}
    for(const auto& p:daw::plugins::vlt1073::parameterTable())controller.setInsertParameter(track,insert,p.id,p.defaultValue);controller.pumpPluginEvents();events();
    QString screenshots;for(int i=1;i+1<argc;++i)if(QString::fromLocal8Bit(argv[i])=="--screenshots")screenshots=QString::fromLocal8Bit(argv[++i]);
    if(!screenshots.isEmpty()){QDir().mkpath(screenshots);for(auto size:{QSize(252,594),QSize(315,743),QSize(504,1188)}){panel.resize(size);events();check(panel.rect().contains(mid->geometry())&&panel.rect().contains(power->geometry()),"proportional layout stays inside the face");check(panel.grab().save(screenshots+QString("/vlt1073-%1x%2.png").arg(size.width()).arg(size.height())),"actual Qt screenshot saved");}
        panel.resize(315,743);controller.setInsertParameter(track,insert,"inputMode",1);controller.setInsertParameter(track,insert,"micGain",60);controller.setInsertParameter(track,insert,"highPass",4);controller.setInsertParameter(track,insert,"midFrequency",6);controller.setInsertParameter(track,insert,"lowFrequency",4);controller.setInsertParameter(track,insert,"level",-10);events();panel.grab().save(screenshots+"/vlt1073-switched.png");
        controller.setInsertParameter(track,insert,"highPass",2);controller.setInsertParameter(track,insert,"midFrequency",1);controller.setInsertParameter(track,insert,"lowFrequency",1);controller.setInsertParameter(track,insert,"level",-12);events();panel.grab().save(screenshots+"/vlt1073-reference.png");
        mouse(level,QEvent::MouseButtonPress,level->rect().center(),Qt::LeftButton,Qt::LeftButton);events();if(auto* bubble=readout())bubble->grab().save(screenshots+"/vlt1073-level-readout.png");QTest::keyClick(level,Qt::Key_Escape);
        controller.setInsertParameter(track,insert,"micZ",0);controller.setInsertParameter(track,insert,"pad",1);power->click();events();panel.grab().save(screenshots+"/vlt1073-switches-off.png");}
    {
    // Start render acceptance from a fresh document, independent of the UI
    // test's undo history, copied track and queued preview gestures.
    daw::EngineController controller;controller.initialize(48000,257,false);
    const auto track=controller.importAudioToNewTrack(sourcePath,0);
    const auto insert=controller.addInsert(track,*descriptor);
    controller.setInsertParameter(track,insert,"highGain",6);controller.pumpPluginEvents();
    daw::rendering::Spec spec;spec.outputDir=temp.path().toStdString();spec.baseName="vlt1073-export";spec.range=daw::rendering::Range::Custom;spec.customEndSeconds=1;spec.blockSize=257;spec.file.container=audio::platform::Container::Wav;spec.file.encoding=audio::platform::Encoding::Float32;spec.stemChannelIds={track};
    const auto capture=[&]{controller.stop();controller.seekSeconds(0);controller.pumpPluginEvents();controller.saveProject(projectPath);daw::EngineController playback;playback.initialize(48000,257,false);playback.openProject(projectPath);playback.pumpPluginEvents();auto graph=playback.routingGraph();for(const auto& e:graph->nodes)e.node->reset();for(const auto& d:graph->delays)d->reset();for(const auto& d:graph->midiDelays)d->reset();
        audio::AudioBuffer in(2,257),out(2,257);in.clear();std::vector<float> data(48544*2);playback.play();bool ok=true;for(unsigned at=0;at<48544;at+=257){unsigned n=std::min(257u,48544-at);ok&=playback.processDeviceBlockForTest(in,out,n);for(unsigned i=0;i<n;++i)for(unsigned ch=0;ch<2;++ch)data[(at+i)*2+ch]=out.getChannel(ch)[i];}playback.stop();check(ok,"device playback renders every block");return data;};
    const auto live=capture();const auto latency=controller.routingGraph()->totalLatency;daw::rendering::Report report;check(bool(controller.renderProject(spec,{},report))&&report.files.size()==2,"master and stem export succeed");audio::platform::DecodedAudio wet,stem;if(!report.files.empty())audio::platform::decodeAudioFile(report.files[0],wet);if(report.files.size()>1)audio::platform::decodeAudioFile(report.files[1],stem);
    auto alignedError=[&](const std::vector<float>& device,const audio::platform::DecodedAudio& exported){double error=0;if(exported.frames!=48000)return 1e9;for(unsigned i=0;i<48000;++i)for(unsigned ch=0;ch<2;++ch)error=std::max(error,std::abs(double(device[(i+latency)*2+ch])-exported.interleaved[i*2+ch]));return error;};
    std::printf("MEASURE playback/export maximum error %.9g (latency %u)\n",alignedError(live,wet),latency);check(alignedError(live,wet)<2e-5,"playback and export match after latency compensation");check(difference(wet.interleaved,stem.interleaved)<2e-5,"master and stem alignment");
    if(!screenshots.isEmpty()&&wet.frames==48000){std::ofstream csv((screenshots+"/render-comparison.csv").toStdString());csv<<"frame,playback,export,difference\n";for(unsigned i=0;i<48000;++i)csv<<i<<','<<live[(i+latency)*2]<<','<<wet.interleaved[i*2]<<','<<double(live[(i+latency)*2])-wet.interleaved[i*2]<<'\n';}
    std::vector<std::string> lanes;for(auto id:{"highGain","level","eqEnabled"}){daw::AutomationTarget t;t.kind=daw::AutomationTargetKind::PluginParameter;t.channelId=track;t.slotId=insert;t.parameterId=id;auto lane=controller.addAutomationLane(track,t);auto clip=controller.addAutomationClip(lane,t,0,1);controller.setAutomationPoints(lane,clip,{{0,.6},{.6,.8},{1,.5}});lanes.push_back(lane);}
    const auto automatedLive=capture();spec.baseName="vlt1073-automation";daw::rendering::Report automatedReport;audio::platform::DecodedAudio automatedAudio;bool exported=bool(controller.renderProject(spec,{},automatedReport))&&!automatedReport.files.empty();if(exported)audio::platform::decodeAudioFile(automatedReport.files[0],automatedAudio);
    std::printf("MEASURE automated playback/export maximum error %.9g\n",alignedError(automatedLive,automatedAudio));check(exported&&alignedError(automatedLive,automatedAudio)<2e-5,"automation produces matching playback and export");daw::rendering::Report guard;check(!controller.freezeTrack(track,{},guard),"external automation retains standard freeze guard");for(const auto& lane:lanes)controller.removeAutomationLane(lane);
    for(const auto& p:daw::plugins::vlt1073::parameterTable())controller.setInsertParameter(track,insert,p.id,p.defaultValue);controller.setInsertParameter(track,insert,"highGain",6);spec.stemChannelIds.clear();spec.baseName="vlt1073-freeze";daw::rendering::Report frozen;check(bool(controller.freezeTrack(track,{},frozen))&&controller.isTrackFrozen(track),"track freezes");daw::rendering::Report frozenExport;audio::platform::DecodedAudio frozenAudio;if(controller.renderProject(spec,{},frozenExport)&&!frozenExport.files.empty())audio::platform::decodeAudioFile(frozenExport.files[0],frozenAudio);check(difference(wet.interleaved,frozenAudio.interleaved)<2e-5,"freeze preserves processing and latency");
    std::printf("MEASURE freeze/export maximum error %.9g\n",difference(wet.interleaved,frozenAudio.interleaved));
    if(!screenshots.isEmpty()&&frozenAudio.frames==48000){std::ofstream csv((screenshots+"/freeze-comparison.csv").toStdString());csv<<"frame,export,freeze,difference\n";for(unsigned i=0;i<48000;++i)csv<<i<<','<<wet.interleaved[i*2]<<','<<frozenAudio.interleaved[i*2]<<','<<double(wet.interleaved[i*2])-frozenAudio.interleaved[i*2]<<'\n';}
    controller.unfreezeTrack(track);controller.setInsertBypassed(track,insert,true);spec.baseName="vlt1073-bypass";daw::rendering::Report bypass;audio::platform::DecodedAudio dry;if(controller.renderProject(spec,{},bypass)&&!bypass.files.empty())audio::platform::decodeAudioFile(bypass.files[0],dry);double dryError=0;if(dry.frames==48000){for(unsigned i=0;i<48000;++i)dryError=std::max(dryError,std::abs(double(dry.interleaved[2*i])-source.getChannel(0)[i]));}else dryError=1e9;check(dryError<2e-5,"bypass exports original aligned audio");
    }
    return failures?1:0;
}
