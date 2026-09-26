#include "ClipLibraryChecks.hpp"
#include "ClipLibraryDrag.hpp"
#include "ClipLibraryView.hpp"
#include "FileBrowserPanel.hpp"
#include "TimelineWidget.hpp"
#include "EngineController.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Core/AudioBuffer.hpp"
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <cstdio>

bool checkClipLibraryForTest(const QString& screenshot) {
    bool ok=true;
    const auto check=[&](bool value,const char* what) { std::fprintf(stderr,"%s clip library: %s\n",value?"PASS":"FAIL",what); ok &= value; return value; };
    daw::EngineController controller;
    if (!controller.initialize(48000,256,false)) return false;
    const auto midiTrack=controller.addTrack(daw::TrackKind::Midi,"Keys");
    const auto midi=controller.addMidiClip(midiTrack,1,8);
    controller.setClipName(midiTrack,midi,"Evening chords");
    std::vector<daw::NoteModel> notes;
    for (int beat=0;beat<16;++beat) for (int step:{0,4,7}) {
        daw::NoteModel n; n.id=daw::newUuid(); n.startBeats=beat; n.lengthBeats=.8;
        n.pitch=48+(beat/4%3)*2+step; n.velocity=40+beat*5; notes.push_back(n);
    }
    controller.setClipNotes(midiTrack,midi,notes,"Fixture notes");
    auto& recorded=const_cast<daw::TrackModel*>(controller.project().findTrack(midiTrack))->clips.front();
    daw::TakeModel take; take.id=daw::newUuid(); take.lengthSeconds=8; take.notes=std::move(notes);
    recorded.notes.clear(); recorded.takes={take};
    daw::CompSegment comp; comp.id=daw::newUuid(); comp.takeId=take.id; comp.endSeconds=8;
    recorded.comp={comp};
    QTemporaryDir dir;
    const auto wav=dir.filePath("Percussion.wav").toStdString();
    audio::AudioBuffer tone(1,48000);
    for (unsigned i=0;i<48000;++i) tone.getChannel(0)[i]=.7f*std::sin(i*.081f)*std::exp(-float(i%6000)/900.f);
    audio::AudioRecorder writer; writer.initialize(48000,1); writer.writeWAVFile(wav,tone,48000);
    const auto audioTrack=controller.importAudioToNewTrack(wav,2,"Percussion");
    const auto audio=controller.project().findTrack(audioTrack)->clips.front().id;
    const auto pattern=controller.addPattern("Drum idea");
    const auto part=controller.addPatternSample(pattern,wav,0);
    std::vector<daw::NoteModel> rhythm;
    for (int step=0;step<8;++step) {
        daw::NoteModel note; note.id=daw::newUuid(); note.pitch=60;
        note.startBeats=step*.5; note.lengthBeats=.2; note.velocity=step%2?65:110;
        rhythm.push_back(note);
    }
    controller.setClipNotes(part,controller.project().findTrack(part)->clips.front().id,rhythm,"Fixture rhythm");
    const auto patternClip=controller.project().findTrack(pattern)->clips.front().id;
    daw::AutomationTarget target; target.channelId=audioTrack;
    const auto lane=controller.addAutomationLane(audioTrack,target);
    const auto curve=controller.addAutomationClip(lane,target,0,4);
    auto& automation=const_cast<daw::TrackModel*>(controller.project().findTrack(lane))->clips.front();
    automation.name="Build-up"; automation.automation.points={{0,.1},{2,.2},{4,.75},{6,.6},{8,1}};

    QWidget root; auto* row=new QHBoxLayout(&root); row->setContentsMargins(0,0,0,0); row->setSpacing(2);
    auto* browser=new FileBrowserPanel(&controller,&root); browser->setFixedWidth(280);
    auto* timeline=new TimelineWidget(&controller,&root); row->addWidget(browser); row->addWidget(timeline,1);
    root.resize(1100,730); root.show(); QApplication::processEvents();
    auto* list=browser->findChild<ClipLibraryView*>();
    if (!check(list!=nullptr,"dedicated browser card view")) return false;
    for (auto* button:browser->findChildren<QToolButton*>())
        if (button->toolTip()==FileBrowserPanel::tr("All folders")) { button->click(); break; }
    check(!list->isVisible(),"file browser stays open before saving");
    std::unique_ptr<QMimeData> mime(ui::cliplibrary::timelineMime(QString::fromStdString(midiTrack),QString::fromStdString(midi)));
    QDragEnterEvent enter(QPoint(30,150),Qt::CopyAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(browser,&enter);
    check(enter.isAccepted(),"closed library tab accepts timeline clip through browser");
    QDropEvent drop(QPointF(30,150),Qt::CopyAction,mime.get(),Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(browser,&drop); QApplication::processEvents();
    check(drop.isAccepted() && list->isVisible() && list->count()==1 &&
        controller.project().findTrack(midiTrack)->clips.size()==1,"drop saves copy, opens library, preserves source");
    const auto entry=controller.project().clipLibrary.front().id;
    browser->saveClipToLibrary(QString::fromStdString(audioTrack),QString::fromStdString(audio));
    browser->saveClipToLibrary(QString::fromStdString(pattern),QString::fromStdString(patternClip));
    browser->saveClipToLibrary(QString::fromStdString(lane),QString::fromStdString(curve));
    check(list->count()==4,"MIDI, audio, Pattern and automation cards");
    auto* search=browser->findChild<QLineEdit*>(QStringLiteral("BrowserSearch"));
    search->setText("Evening"); check(list->count()==1,"search filters card names");
    search->clear(); check(list->count()==4,"clearing search restores cards");
    list->selectEntry(QString::fromStdString(entry));
    QKeyEvent overrideDelete(QEvent::ShortcutOverride,Qt::Key_Delete,Qt::NoModifier);
    overrideDelete.ignore(); QApplication::sendEvent(list,&overrideDelete);
    QKeyEvent deleteKey(QEvent::KeyPress,Qt::Key_Delete,Qt::NoModifier);
    QApplication::sendEvent(list,&deleteKey);
    check(overrideDelete.isAccepted() && !controller.libraryClip(entry) &&
        controller.project().findTrack(midiTrack)->clips.size()==1,
        "Delete belongs to library and leaves original timeline clip intact");
    controller.undo(); browser->refreshClipLibrary(); list->selectEntry(QString::fromStdString(entry));
    QMenu menu; check(list->populateActions(menu) && menu.actions().size()>=4,"context actions for restoration, rename and removal");
    const auto before=controller.project().findTrack(midiTrack)->clips.size();
    menu.actions().front()->trigger();
    check(controller.project().findTrack(midiTrack)->clips.size()==before+1,"original-position menu restores through browser action");
    controller.undo(); browser->refreshClipLibrary();

    QMimeData fromLibrary; fromLibrary.setData(ui::cliplibrary::kLibraryMime,QByteArray::fromStdString(entry));
    const QPoint targetPoint(int(10*timeline->pixelsPerSecondForTest()),timeline->laneCentreForTest(0));
    QDragEnterEvent timelineEnter(targetPoint,Qt::CopyAction,&fromLibrary,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(timeline,&timelineEnter);
    QDropEvent timelineDrop(targetPoint,Qt::CopyAction,&fromLibrary,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(timeline,&timelineDrop);
    check(timelineDrop.isAccepted() && controller.project().findTrack(midiTrack)->clips.size()==before+1,
        "card drops onto timeline as a new clip");
    controller.undo();

    const auto mouse=[&](QEvent::Type type,const QPoint& local,Qt::MouseButton button,Qt::MouseButtons buttons) {
        QMouseEvent event(type,QPointF(local),QPointF(timeline->mapToGlobal(local)),button,buttons,Qt::NoModifier);
        QApplication::sendEvent(timeline,&event);
    };
    const QPoint press(int(2*timeline->pixelsPerSecondForTest()),timeline->laneCentreForTest(0));
    const auto undoBefore=controller.undoDepth();
    mouse(QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton);
    mouse(QEvent::MouseMove,press+QPoint(45,0),Qt::NoButton,Qt::LeftButton);
    QTimer::singleShot(80,&root,[] { QDrag::cancel(); });
    mouse(QEvent::MouseMove,timeline->mapFromGlobal(browser->mapToGlobal(QPoint(50,150))),Qt::NoButton,Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease,press,Qt::LeftButton,Qt::NoButton);
    check(std::abs(controller.project().findTrack(midiTrack)->clips.front().startSeconds-1)<1e-9 &&
        controller.undoDepth()==undoBefore,"drag handoff and cancellation preserve original placement and history");
    browser->showClipLibrary(); list->selectEntry(QString::fromStdString(entry));
    QEventLoop settle; QTimer::singleShot(150,&settle,&QEventLoop::quit); settle.exec();
    if (!screenshot.isEmpty()) check(root.grab().save(screenshot),"visual fixture saved");
    for (int width:{160,220,400}) {
        browser->setFixedWidth(width); QApplication::processEvents();
        check(list->horizontalScrollBar()->maximum()==0,"cards fit narrow and wide browser widths");
        if (width==160 && !screenshot.isEmpty()) {
            const QFileInfo path(screenshot);
            check(root.grab().save(path.absolutePath()+"/"+path.completeBaseName()+"-narrow.png"),"narrow visual fixture saved");
        }
    }
    controller.newProject(); browser->refreshClipLibrary();
    check(list->count()==0,"switching projects clears previous project's cards");
    return ok;
}
