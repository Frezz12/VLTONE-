#include "MasterPanel.hpp"
#include "MainWindow.hpp"
#include "MixerWidget.hpp"
#include "MixerPreferences.hpp"
#include "TransportBar.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QScopeGuard>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>

bool MasterPanel::checkForTest(const QString& prefix) {
    daw::EngineController controller;
    if(!controller.initialize(48000,256,false)) return false;
    const auto track=controller.addTrack(daw::TrackKind::Audio,"Reference");
    controller.setTrackInputRouting(track,0,2,true); controller.setTrackMonitor(track,true);
    const auto& descriptor=daw::plugins::equalizer::EqualizerInstance::staticDescriptor();
    const auto fx=controller.addInsert(daw::EngineController::kMasterChannelId,descriptor);
    const auto analyzer=controller.addInsert(daw::EngineController::kMasterChannelId,descriptor,size_t(-1),true);
    if(fx.empty() || analyzer.empty()) return false;
    MasterPanel panel(&controller); panel.resize(196,850); panel.show(); QApplication::processEvents(); panel.sync();
    const auto check=[&](bool ok,const char* text) { std::fprintf(stderr,"%s Master panel: %s\n",ok?"PASS":"FAIL",text); return ok; };
    if(!check(panel.m_inserts->count()==2 && panel.m_analysisInserts->count()==2,"processing and analysis wells each have a plugin and an add area")) return false;
    const auto depth=controller.undoDepth();
    QKeyEvent key(QEvent::KeyPress,Qt::Key_Down,Qt::NoModifier); QApplication::sendEvent(panel.m_fader,&key);
    if(!check(controller.masterVolume()<1 && controller.undoDepth()==depth+1,"keyboard gain edit is one undo step")) return false;
    controller.undo(); panel.sync();
    if(!check(std::abs(panel.m_fader->gain()-1)<1e-6,"undo updates master gain")) return false;
    panel.m_analysisToggle->setChecked(true); QApplication::processEvents();
    if(!check(panel.m_analysisBody->isVisible(),"analysis section expands")) return false;
    auto* footer=panel.findChild<QWidget*>("MasterAnalysisFooter");
    const auto footerAtBottom=[&] { return footer && footer->geometry().bottom()==panel.height()-1; };
    if(!check(footerAtBottom(),"expanded analyzers are anchored to the panel bottom")) return false;
    panel.m_analysisToggle->setChecked(false); QApplication::processEvents();
    if(!check(footerAtBottom(),"collapsed analyzers remain anchored to the panel bottom")) return false;
    panel.resize(196,1100); QApplication::processEvents();
    auto* outputWell=panel.findChild<QWidget*>("MasterOutput");
    const auto outputAtFooter=[&] {
        const int gap=footer->y()-outputWell->mapTo(&panel,QPoint(0,outputWell->height())).y();
        return gap>=0 && gap<=10;
    };
    const auto tallFaderHeight=panel.m_fader->height();
    if(!check(outputAtFooter() && tallFaderHeight>400 && panel.m_outputMeter->height()==tallFaderHeight,
              "fader and stereo meter fill the height down to the collapsed analyzers")) return false;
    panel.m_analysisToggle->setChecked(true); QApplication::processEvents();
    if(!check(outputAtFooter() && panel.m_fader->height()<tallFaderHeight && panel.m_fader->height()>200,
              "expanding analyzers resizes the console without leaving a gap")) return false;
    panel.resize(196,850); QApplication::processEvents();
    controller.play();
    audio::AudioBuffer input(2,256),output(2,256);
    for(int block=0;block<650;++block) {
        for(unsigned i=0;i<256;++i) {
            input.getChannel(0)[i]=.16f*std::sin((block*256+i)*2*3.141592653589793*440/48000);
            input.getChannel(1)[i]=.12f*std::sin((block*256+i)*2*3.141592653589793*440/48000+.3);
        } controller.processDeviceBlockForTest(input,output,256);
    }
    panel.refreshMeters(); QApplication::processEvents();
    if(!check(controller.masterLoudness().vuLeft>.1f,"VU receives measured audio")) return false;
    if(!prefix.isEmpty()) panel.grab().save(prefix+"-panel.png");
    panel.resize(196,470); QApplication::processEvents();
    const auto* scroll=panel.findChild<QScrollArea*>();
    if(!check(scroll && scroll->widget()->width()<=scroll->viewport()->width(),"narrow panel fits beside the vertical scrollbar")) return false;
    if(!check(footerAtBottom(),"resizing keeps analyzers pinned beneath the scrollable controls")) return false;
    if(!prefix.isEmpty()) panel.grab().save(prefix+"-narrow.png");
    auto* reset=panel.findChild<QToolButton*>("MasterResetMeters"); reset->click();
    if(!check(std::isnan(controller.masterLoudness().integrated),"reset clears integrated measurement")) return false;
    controller.stop(); return true;
}

bool MainWindow::checkMasterPanelForTest() {
    const auto prefix=qEnvironmentVariable("DAW_MASTER_SCREENSHOT");
    if(!MasterPanel::checkForTest(prefix)) return false;
    auto& preferences=ui::MixerPreferences::instance(); const bool before=preferences.masterVisible();
    const auto restore=qScopeGuard([&] { preferences.setMasterVisible(before); });
    const auto settle=[&] { for(int i=0;i<4;++i) QApplication::processEvents(); };
    const auto fail=[&](const char* reason) {
        std::fprintf(stderr,"FAIL Master workspace: %s; shell=%d master=%d visible=%d\n",reason,
            centralWidget()->width(),m_masterPanel?m_masterPanel->width():-1,m_masterPanel?m_masterPanel->isVisible():false);
        std::fprintf(stderr,"workspace=%d min=%d transportMin=%d panelX=%d\n",m_workspace->width(),m_workspace->minimumWidth(),m_transport->minimumResponsiveWidth(),m_masterPanel->x());
        const auto prefix=qEnvironmentVariable("DAW_MASTER_SCREENSHOT");
        if(!prefix.isEmpty()) grab().save(prefix+"-failure.png");
        return false;
    };
    preferences.setMasterVisible(true); setMasterVisible(true); resize(1600,960); show(); settle();
    if(!m_masterPanel || m_masterPanel->isHidden() || !m_transport) return fail("panel not open");
    auto* toggle=m_transport->findChild<QAbstractButton*>("HeaderMasterButton");
    if(!toggle || !toggle->isChecked()) return fail("header toggle out of sync");
    m_masterPanel->findChild<QAbstractButton*>("HideMasterPanel")->click(); settle();
    if(!m_masterPanel->isHidden() || toggle->isChecked()) return fail("close did not hide panel");
    toggle->click(); settle();
    if(m_masterPanel->isHidden() || !preferences.masterVisible()) return fail("header did not reopen panel");
    const auto right=m_masterPanel->mapTo(centralWidget(),QPoint(m_masterPanel->width(),0)).x();
    if(right>centralWidget()->width() || m_masterPanel->width()<196) return fail("panel outside workspace or too narrow");
    if(m_masterPanel->width()!=196) return fail("default master width is not reduced by another 10 percent");
    auto* outputWell=m_masterPanel->findChild<QWidget*>("MasterOutput");
    auto* footer=m_masterPanel->findChild<QWidget*>("MasterAnalysisFooter");
    const int gap=footer->y()-outputWell->mapTo(m_masterPanel,QPoint(0,outputWell->height())).y();
    if(gap<0 || gap>10) return fail("output console does not reach the analysis footer");
    if(!prefix.isEmpty()) grab().save(prefix+"-workspace.png");
    if(!MixerWidget::checkMasterDockForTest()) return fail("mixer still owns master");
    return true;
}
