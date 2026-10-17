#include "MixerWidget.hpp"
#include "EngineController.hpp"
#include <QApplication>
#include <QScopeGuard>
#include <QToolButton>
#include <cstdio>

bool MixerWidget::checkMasterDockForTest() {
    auto& preferences=ui::MixerPreferences::instance();
    const bool original=preferences.masterVisible();
    const auto restore=qScopeGuard([&] { preferences.setMasterVisible(original); });
    daw::EngineController controller;
    if(!controller.initialize(48000,256,false)) return false;
    controller.addTrack(daw::TrackKind::Audio,"Track");
    MixerWidget mixer(&controller); mixer.setRackMode(false); mixer.resize(800,600); mixer.show();
    QApplication::processEvents();
    for(auto* strip:mixer.m_strips) if(strip->isMaster()) return false;
    preferences.setMasterVisible(true); mixer.m_masterToggle->click();
    if(preferences.masterVisible() || mixer.m_masterToggle->isChecked()) return false;
    mixer.m_masterToggle->click();
    if(!preferences.masterVisible() || !mixer.m_masterToggle->isChecked()) return false;
    std::fprintf(stderr,"PASS Master: mixer contains tracks only; its button controls sidebar visibility\n");
    return true;
}
