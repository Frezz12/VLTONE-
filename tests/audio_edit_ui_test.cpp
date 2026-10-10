#include "numeric_field_checks.hpp"
#include "AudioEditPanel.hpp"
#include "Controls.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include <QApplication>
#include <QTranslator>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QPixmap>
#include <algorithm>
#include <cstdio>
#include <cmath>

static bool gestureChecks() {
    AudioEditCanvas canvas;
    canvas.resize(800,300);
    const auto doc=daw::audioedit::fromSource({"gestures","test.wav",48000,48000,2});
    canvas.setAudio(doc,{},{});
    canvas.showRange(6000,30000);
    canvas.setSelection(12000,24000);
    auto mouse=[&](QEvent::Type type,double x,double y=140) {
        QMouseEvent event(type,QPointF(x,y),QPointF(x,y),
            type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
            type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&event);
    };
    auto escape=[&] {
        QKeyEvent event(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&event);
    };
    mouse(QEvent::MouseButtonPress,200);mouse(QEvent::MouseMove,300);
    if(canvas.first()!=15000||canvas.last()!=24000)return false;
    escape();
    if(canvas.first()!=12000||canvas.last()!=24000)return false;
    int slips=0,moves=0,fades=0;double slipped=0;
    QObject::connect(&canvas,&AudioEditCanvas::slipRequested,[&](double delta){++slips;slipped=delta;});
    QObject::connect(&canvas,&AudioEditCanvas::moveRequested,[&](const QString&,qint64){++moves;});
    QObject::connect(&canvas,&AudioEditCanvas::fadeRequested,[&](bool,qint64,double){++fades;});
    canvas.setTool(AudioEditCanvas::Tool::Slip);
    mouse(QEvent::MouseButtonPress,400);mouse(QEvent::MouseMove,300);
    if(canvas.scroll()!=9000)return false;
    mouse(QEvent::MouseMove,450);
    if(canvas.scroll()!=4500)return false;
    escape();mouse(QEvent::MouseButtonRelease,450);
    if(canvas.scroll()!=6000||slips!=0)return false;
    mouse(QEvent::MouseButtonPress,400);mouse(QEvent::MouseMove,300);mouse(QEvent::MouseButtonRelease,300);
    if(slips!=1||std::abs(slipped-3000./48000)>1e-12)return false;
    canvas.showRange(6000,30000);
    canvas.setTool(AudioEditCanvas::Tool::Move);
    mouse(QEvent::MouseButtonPress,400);mouse(QEvent::MouseMove,450);escape();mouse(QEvent::MouseButtonRelease,450);
    if(moves!=0)return false;
    canvas.setTool(AudioEditCanvas::Tool::Select);
    mouse(QEvent::MouseButtonPress,206,34);mouse(QEvent::MouseMove,400,34);escape();mouse(QEvent::MouseButtonRelease,400,34);
    if(fades!=0)return false;
    mouse(QEvent::MouseButtonPress,206,34);mouse(QEvent::MouseMove,400,34);mouse(QEvent::MouseButtonRelease,400,34);
    if(fades!=1)return false;
    canvas.showRange(24000,72000);
    mouse(QEvent::MouseButtonPress,600);mouse(QEvent::MouseButtonRelease,600);
    if(canvas.cursor()!=60000||canvas.first()!=48000||canvas.last()!=48000)return false;
    QKeyEvent space(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&space);
    if(space.isAccepted())return false;
    std::fprintf(stderr,"PASS Audio editor: selection handles, immediate slip reversal, Escape, single commit, project Space\n");
    return true;
}

static bool editorChecks() {
    QTemporaryDir directory;
    const auto path=directory.filePath("source.wav").toStdString();
    audio::AudioBuffer source(2,4800);
    for(unsigned ch=0;ch<2;++ch)for(unsigned f=0;f<4800;++f)source.getChannel(ch)[f]=.2f;
    if(!audio::AudioRecorder::writeWAVFile(path,source,48000))return false;
    daw::EngineController controller;
    if(!controller.initialize(48000,512,false))return false;
    const auto track=controller.addTrack(daw::TrackKind::Audio,"Editors");
    const auto original=controller.importAudio(path,track,0);
    const auto copies=controller.duplicateLinkedClips({{track,original}});
    if(copies.size()!=1)return false;
    const daw::EngineController::AudioEditTarget target{track,original,false};
    AudioEditPanel first(&controller,target),second(&controller,{track,copies.front().clipId,false});
    first.resize(720,440);second.resize(720,440);first.show();second.show();
    auto* left=first.findChild<AudioEditCanvas*>();auto* right=second.findChild<AudioEditCanvas*>();
    auto* gain=first.findChild<ui::Knob*>("sampleEditor.gain");
    const auto wait=[](auto ready) {
        QElapsedTimer timer;timer.start();
        while(timer.elapsed()<5000){QApplication::processEvents();if(ready())return true;QThread::msleep(1);}
        return false;
    };
    if(!left||!right||!gain||!wait([&]{first.triggerAction("all");second.triggerAction("all");return left->last()==4800&&right->last()==4800;}))return false;
    auto* start = first.findChild<QDoubleSpinBox*>("sampleEditor.start");
    if (!start) return false;
    left->setSelection(480, 4320);
    auto numberMouse = [&](QEvent::Type type, double y) {
        QMouseEvent event(type, QPointF(10, 10), QPointF(300, y),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::ShiftModifier);
        QApplication::sendEvent(start, &event);
    };
    numberMouse(QEvent::MouseButtonPress, 300);
    numberMouse(QEvent::MouseMove, 296);
    if (left->first() != 2400) return false;
    numberMouse(QEvent::MouseMove, 298);
    if (left->first() != 1440) return false;
    QKeyEvent cancelNumber(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(start, &cancelNumber);
    if (left->first() != 480) return false;
    first.triggerAction("all");
    std::fprintf(stderr, "PASS Audio editor: Start field scrubs selection live and Escape restores it\n");
    const auto baseline=controller.audioEditDocument(target);
    const auto undoBefore=controller.undoLabel();
    QMetaObject::invokeMethod(gain,"valueChanged",Qt::DirectConnection,Q_ARG(double,-6.0));
    if(!wait([&]{return controller.audioEditDocument(target).revision!=baseline.revision;}))return false;
    QMetaObject::invokeMethod(gain,"valueChanged",Qt::DirectConnection,Q_ARG(double,0.0));
    QMetaObject::invokeMethod(gain,"editFinished",Qt::DirectConnection);
    if(!wait([&]{return controller.audioEditDocument(target)==baseline&&!first.property("gainTransaction").toBool();})||
       controller.undoLabel()!=undoBefore)return false;
    second.hide(); // Stop polling to force an obsolete second view.
    first.triggerAction("silence");
    if(!wait([&]{return controller.audioEditDocument(target).regions.empty();}))return false;
    // An operation made before the next refresh must never restore old audio.
    second.triggerAction("reverse");
    QElapsedTimer settle;settle.start();
    while(settle.elapsed()<250){QApplication::processEvents();QThread::msleep(1);}
    if(!controller.audioEditDocument(target).regions.empty())return false;
    controller.undo();
    if(controller.audioEditDocument(target)!=baseline)return false;
    std::fprintf(stderr,"PASS Audio editor: async gain preview, return to zero, shared open editors, one-step undo\n");
    return true;
}

static bool longWaveformChecks() {
    constexpr unsigned frames=48000*600;
    auto audio=std::make_shared<daw::engine::SampleBuffer>(2,frames,48000);
    for(unsigned i=0;i<frames;++i){
        audio->writableChannel(0)[i]=float(int(i%1024)-512)/1024;
        audio->writableChannel(1)[i]=float(int(i%511)-255)/511;
    }
    QElapsedTimer timer;timer.start();
    auto peaks=AudioEditPeaks::build(*audio);
    const auto preparationMs=timer.elapsed();
    if(!peaks||peaks->channels.size()!=2)return false;
    AudioEditCanvas canvas;canvas.resize(1000,400);
    canvas.setAudio(daw::audioedit::fromSource({"long","long.wav",48000,frames,2}),audio,peaks);
    QPixmap image(canvas.size());std::vector<double> paintMs;
    for(int step=0;step<120;++step){
        if(step%20==0)canvas.fit();else canvas.zoom(step%2?2:.5,500);
        canvas.scrollTo(double(step%12)*frames/24.);
        timer.restart();canvas.render(&image);paintMs.push_back(timer.nsecsElapsed()/1e6);
    }
    std::sort(paintMs.begin(),paintMs.end());
    std::fprintf(stderr,"PASS 10-minute stereo waveform: peaks %lld ms, navigation paint p50 %.2f ms / p95 %.2f ms\n",
        static_cast<long long>(preparationMs),paintMs[paintMs.size()/2],paintMs[paintMs.size()*95/100]);
    return true;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    std::fprintf(stderr,"Audio editor UI check: QApplication ready\n");
    QApplication::setApplicationName("VLTONE Audio Editor Check");
    QTranslator russian;
    if (argc > 1 && russian.load(QString::fromLocal8Bit(argv[1])))
        app.installTranslator(&russian);
    return numericFieldChecks(app) && gestureChecks() && editorChecks() && longWaveformChecks() && AudioEditPanel::checkForTest() ? 0 : 1;
}
