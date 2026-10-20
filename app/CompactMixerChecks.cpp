#include "MixerWidget.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "MainWindow.hpp"
#include "MiniModuleRack.hpp"
#include "RackWidget.hpp"
#include "Theme.hpp"
#include "TrackListWidget.hpp"
#include "model/MiniModules.hpp"
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdio>

bool MixerWidget::checkCompactForTest() {
    bool ok = true;
    const auto check = [&](bool result, const char* message) {
        std::printf("%s compact mixer: %s\n", result ? "PASS" : "FAIL", message);
        std::fflush(stdout);
        ok &= result;
    };
    auto& prefs = ui::MixerPreferences::instance();
    const auto oldView = prefs.view();
    const int oldClassicWidth = prefs.channelWidth(), oldCompactWidth = prefs.compactWidth();
    const int oldDetailsWidth = prefs.detailsWidth();
    const bool oldColor = prefs.colorVisible();
    const QString oldTheme = ThemeManager::instance().themeId();
    QMap<QString, QVariant> saved;
    for (const char* key : {ui::MixerPreferences::kViewSetting, ui::MixerPreferences::kWidthSetting,
                           ui::MixerPreferences::kCompactWidthSetting, ui::MixerPreferences::kDetailsWidthSetting,
                           ui::MixerPreferences::kColorVisibleSetting, "ui/rackMode"})
        saved.insert(QString::fromLatin1(key), QSettings().value(key));
    const auto restore = qScopeGuard([&] {
        prefs.setView(oldView); prefs.setChannelWidth(oldClassicWidth); prefs.setCompactWidth(oldCompactWidth);
        prefs.setDetailsWidth(oldDetailsWidth); prefs.setColorVisible(oldColor);
        ThemeManager::instance().setThemeId(oldTheme, false);
        for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
            if (it.value().isValid()) QSettings().setValue(it.key(), it.value());
            else QSettings().remove(it.key());
        }
    });
    prefs.setView(ui::MixerPreferences::View::Compact);
    prefs.setChannelWidth(111);
    prefs.setCompactWidth(32);
    prefs.setDetailsWidth(120);
    prefs.setColorVisible(false);
    daw::EngineController controller;
    if (!controller.initialize(48000, 256, false)) return false;
    const auto first = controller.addTrack(daw::TrackKind::Audio, "Kick and percussion");
    const auto second = controller.addTrack(daw::TrackKind::Audio, "Snare");
    const auto bus = controller.addTrack(daw::TrackKind::Aux, "Room");
    const auto folder = controller.addFolder(false, "Folder");
    const auto instrument = controller.addTrack(daw::TrackKind::Instrument, "Synth");
    const auto pattern = controller.addPattern("Drum pattern");
    const auto module = controller.addMiniModule(first, daw::plugins::mini::builtin("color"));
    const auto fx = controller.addInsert(first, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
    const auto send = controller.addSend(first, bus);
    if (module.empty() || fx.empty() || send.empty()) return false;
    // Keep horizontal overflow even with the narrower details panel.
    for (int i = 0; i < 30; ++i) controller.addTrack(daw::TrackKind::Audio, "Channel " + std::to_string(i + 7));
    controller.setTrackInputRouting(first, 0, 2, true);
    controller.setTrackMonitor(first, true);
    audio::AudioBuffer input(2, 256), output(2, 256);
    std::fill_n(input.getChannel(0), 256, .25f);
    std::fill_n(input.getChannel(1), 256, 2.f);
    for (int block = 0; block < 8; ++block) controller.processDeviceBlockForTest(input, output, 256);
    MixerWidget mixer(&controller);
    mixer.setRackMode(false);
    mixer.resize(1000, 650);
    mixer.show();
    const auto settle = [&] {
        for (int pass = 0; pass < 80; ++pass) {
            mixer.syncVisibleStrips();
            QApplication::processEvents();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    };
    settle();
    mixer.setSelectedTrack(QString::fromStdString(first));
    settle();
    check(mixer.m_details && mixer.m_details->trackId().toStdString() == first &&
          mixer.m_detailsScroll->width() == 120, "one selected-channel panel opens at 120 logical px");
    if (!mixer.m_details || mixer.m_strips.empty()) return false;
    auto* details = mixer.m_details;
    const auto stripFor = [&](const std::string& id) -> ChannelStrip* {
        for (auto* strip : mixer.m_strips) if (strip->trackId().toStdString() == id) return strip;
        return nullptr;
    };
    check(details->findChildren<ui::FaderWidget*>().empty() && details->findChildren<ui::PanKnob*>().empty() &&
          details->findChildren<ui::LevelMeter*>().empty() &&
          details->findChild<ui::MiniModuleRack*>() && details->findChild<QWidget*>("ChannelColorRow")->isVisible(),
          "details has no duplicate level controls and shows mini modules with classic modules hidden");
    const auto undoLayout = controller.undoDepth();
    const auto graph = controller.routingGraph();
    const QString shots = qEnvironmentVariable("DAW_COMPACT_MIXER_SCREENSHOTS");
    if (!shots.isEmpty()) QDir().mkpath(shots);
    for (const bool dark : {true, false}) {
        for (const auto& theme : ThemeManager::instance().presets())
            if (theme.dark == dark) { ThemeManager::instance().setThemeId(theme.id, false); break; }
        for (const int width : {24, 32, 40, 48}) {
            prefs.setCompactWidth(width);
            settle();
            mixer.refreshMeters();
            auto* strip = stripFor(first);
            if (!strip) return false;
            auto* fader = strip->findChild<ui::FaderWidget*>();
            auto* pan = strip->findChild<ui::PanKnob*>();
            auto* meter = strip->findChild<ui::LevelMeter*>();
            const auto buttons = strip->findChildren<ui::MsrButton*>();
            bool fits = strip->width() == width && fader && pan && meter && buttons.size() == 2 &&
                strip->findChildren<ui::MiniModuleRack*>().empty() && !strip->findChild<QWidget*>("ChannelRack");
            for (auto* control : {static_cast<QWidget*>(fader), static_cast<QWidget*>(pan), static_cast<QWidget*>(meter)}) {
                const QRect bounds(control->mapTo(strip, QPoint()), control->size());
                fits &= strip->rect().contains(bounds);
            }
            if (buttons.size() == 2) {
                const auto muteAt = buttons[0]->mapTo(strip, QPoint());
                const auto soloAt = buttons[1]->mapTo(strip, QPoint());
                fits &= width == 48 ? muteAt.y() == soloAt.y() && muteAt.x() != soloAt.x()
                                    : muteAt.x() == soloAt.x() && muteAt.y() != soloAt.y();
                fits &= meter->mapTo(strip, QPoint(0, meter->height())).y() <= muteAt.y() &&
                    pan->mapTo(strip, QPoint()).y() > soloAt.y() &&
                    fader->mapTo(strip, QPoint()).y() >= pan->mapTo(strip, QPoint(0, pan->height())).y();
            }
            check(fits, "24/32/40/48 px, both themes: ordered controls fit and narrow strips contain no rack");
            if (!shots.isEmpty())
                check(mixer.grab().save(shots + QString("/%1-%2.png").arg(dark ? "dark" : "light").arg(width)),
                      "layout screenshot saved");
        }
    }
    check(controller.undoDepth() == undoLayout && controller.routingGraph() == graph &&
          prefs.channelWidth() == 111 && QSettings().value(ui::MixerPreferences::kCompactWidthSetting).toInt() == 48,
          "display preferences persist independently without touching graph, history or classic width");
    prefs.setDetailsWidth(310);
    settle();
    check(mixer.m_detailsScroll->width() == 310, "details divider applies the saved width");
    prefs.setDetailsWidth(999);
    check(prefs.detailsWidth() == 400, "details width is bounded at 400");
    prefs.setDetailsWidth(50);
    check(prefs.detailsWidth() == 100, "details width is bounded at 100");
    prefs.setDetailsWidth(120);
    prefs.setCompactWidth(32);
    settle();
    auto* strip = stripFor(first);
    auto* fader = strip->findChild<ui::FaderWidget*>();
    auto* pan = strip->findChild<ui::PanKnob*>();
    auto* meter = strip->findChild<ui::LevelMeter*>();
    mixer.refreshMeters();
    check(meter->hasVisibleLevel() && controller.meterSnapshot(first).right > controller.meterSnapshot(first).left,
          "compact meter displays real independent stereo peaks");
    mixer.m_scroll->horizontalScrollBar()->setValue(300);
    settle();
    meter->clearLevels();
    mixer.refreshMeters();
    check(!mixer.stripIsVisible(strip) && !meter->hasVisibleLevel(),
          "offscreen resident channels receive no meter updates");
    mixer.m_scroll->horizontalScrollBar()->setValue(0);
    settle();
    mixer.refreshMeters();
    check(meter->hasVisibleLevel(), "scrolling back resumes meter updates");
    const int initialFaderHeight = fader->height();
    const int initialMeterHeight = strip->findChild<ui::LevelMeter*>()->height();
    mixer.resize(1000, 850);
    settle();
    check(fader->height() > initialFaderHeight && strip->findChild<ui::LevelMeter*>()->height() > initialMeterHeight,
          "tall mixer grows both the meter and fader");
    mixer.resize(1000, 240);
    settle();
    check(mixer.m_scroll->verticalScrollBar()->maximum() > 0 &&
          mixer.m_detailsScroll->verticalScrollBar()->maximum() > 0, "short mixer scrolls both areas");
    mixer.resize(1000, 650);
    settle();
    const auto mouse = [](QWidget* target, QEvent::Type type, QPointF point,
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(type, point, target->mapToGlobal(point),
                          type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(target, &event);
    };
    const QPointF at = fader->rect().center();
    const auto drag = [&](Qt::KeyboardModifiers modifiers) {
        mouse(fader, QEvent::MouseButtonPress, at);
        mouse(fader, QEvent::MouseMove, at + QPointF(0, 28), modifiers);
        mouse(fader, QEvent::MouseButtonRelease, at + QPointF(0, 28), modifiers);
    };
    auto depth = controller.undoDepth();
    drag(Qt::NoModifier);
    const float coarse = controller.project().findTrack(first)->volume;
    check(coarse < 1 && controller.undoDepth() == depth + 1, "fader drag commits one history entry");
    controller.undo(); mixer.syncFromModel();
    check(std::abs(fader->gain() - 1) < 1e-6, "Undo restores the compact fader");
    drag(Qt::ShiftModifier);
    check(controller.project().findTrack(first)->volume > coarse &&
          controller.project().findTrack(first)->volume < 1, "Shift retains precise fader adjustment");
    mouse(fader, QEvent::MouseButtonDblClick, at);
    check(std::abs(controller.project().findTrack(first)->volume - 1) < 1e-6, "double click resets volume");
    depth = controller.undoDepth();
    mouse(pan, QEvent::MouseButtonPress, pan->rect().center());
    mouse(pan, QEvent::MouseMove, QPointF(pan->rect().center()) + QPointF(24, 0));
    mixer.setSelectedTrack(QString::fromStdString(second));
    check(!pan->isEditing() && controller.project().findTrack(first)->pan > 0 &&
          controller.project().findTrack(second)->pan == 0 && controller.undoDepth() == depth + 1,
          "selection commits an unfinished pan to the old channel");
    mouse(pan, QEvent::MouseMove, QPointF(90, 0));
    controller.undo(); mixer.syncFromModel();
    check(controller.project().findTrack(first)->pan == 0 && pan->pan() == 0, "old pan gesture cannot resume after selection");
    mixer.setSelectedTrack(QString::fromStdString(first));
    for (auto* button : strip->findChildren<ui::MsrButton*>()) {
        depth = controller.undoDepth();
        button->click();
        const auto* track = controller.project().findTrack(first);
        check((button->objectName() == "ChannelMute" ? track->muted : track->soloed) &&
              controller.undoDepth() == depth, "M/S preserves the existing channel and history semantics");
        button->click(); mixer.syncFromModel();
    }
    QString automationChannel;
    connect(&mixer, &MixerWidget::automateControlRequested, &mixer,
            [&](const QString& id, bool) { automationChannel = id; });
    emit fader->automateRequested();
    check(automationChannel.toStdString() == first, "compact fader requests automation on its channel");
    daw::AutomationTarget target;
    target.kind = daw::AutomationTargetKind::TrackVolume; target.channelId = first;
    const auto lane = controller.addAutomationLane(first, target);
    const auto clip = controller.addAutomationClip(lane, target, 0, 4);
    daw::AutomationPoint start, end;
    start.beats = 0; start.value = .25; end.beats = 8; end.value = .85;
    controller.setAutomationPoints(lane, clip, {start, end});
    controller.play();
    controller.seekSeconds(.3);
    mixer.refreshMeters();
    const auto automated = controller.automationValueAtPlayhead(target);
    check(automated && std::abs(fader->gain() - *automated) < 1e-5, "existing meter frame follows automation");
    controller.stop(); mixer.refreshAutomationValues();
    QString editedChannel, editedSlot;
    connect(&mixer, &MixerWidget::pluginEditorRequested, &mixer, [&](const QString& id, const QString& slot) {
        editedChannel = id; editedSlot = slot;
    });
    for (auto* button : details->findChildren<QToolButton*>("SlotButton"))
        if (button->property("pluginSlotId").toString().toStdString() == fx) button->click();
    check(editedChannel.toStdString() == first && editedSlot.toStdString() == fx, "details opens the existing plugin editor");
    auto* monitor = details->findChild<ui::MsrButton*>("ChannelMonitor");
    if (monitor) monitor->click();
    check(monitor && !controller.project().findTrack(first)->monitor, "details monitoring uses the existing channel action");
    for (auto* button : details->findChildren<ui::IconButton*>())
        if (button->property("insertBypassSlot").toString().toStdString() == fx) { button->click(); break; }
    settle();
    check(controller.project().findTrack(first)->inserts.front().bypassed, "details plugin bypass reaches the selected insert");
    controller.undo(); mixer.syncFromModel(); settle();
    auto* sendRow = details->findChild<QWidget*>("ChannelSendsRow");
    auto* sendKnob = sendRow ? sendRow->findChild<ui::Knob*>() : nullptr;
    check(sendKnob != nullptr, "details exposes send level controls");
    if (sendKnob) {
        const float before = controller.project().findTrack(first)->sends.front().level;
        depth = controller.undoDepth();
        mouse(sendKnob, QEvent::MouseButtonPress, sendKnob->rect().center());
        mouse(sendKnob, QEvent::MouseMove, QPointF(sendKnob->rect().center()) + QPointF(0, -20));
        mixer.setSelectedTrack(QString::fromStdString(second));
        check(mixer.m_details == details && controller.project().findTrack(first)->sends.front().level != before &&
              controller.project().findTrack(second)->sends.empty() && controller.undoDepth() == depth + 1,
              "one details instance finishes a send gesture before rebinding");
        controller.undo();
        check(std::abs(controller.project().findTrack(first)->sends.front().level - before) < 1e-6,
              "send gesture Undo restores the original channel");
    }
    QMimeData chain;
    chain.setData("application/x-daw-chain", QByteArray::fromStdString(first));
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &chain, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(details, &enter);
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &chain, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(details, &drop);
    settle();
    check(enter.isAccepted() && drop.isAccepted() && !controller.project().findTrack(second)->inserts.empty(),
          "chain drag and copy work on the selected-channel details surface");
    controller.renameTrack(second, "New snare name");
    controller.setTrackColor(second, 0xE05D87);
    mixer.syncFromModel();
    check(details->findChild<QLabel*>("StripName")->text() == "New snare name" &&
          stripFor(second)->toolTip() == "New snare name", "rename and color refresh the strip and panel");
    mixer.setSelectedTrack(QString::fromStdString(instrument));
    check(details == mixer.m_details && details->findChild<QWidget*>("ChannelInstrumentRow"),
          "changing track kind exposes the instrument section on the same details surface");
    mixer.setSelectedTrack(QString::fromStdString(pattern));
    check(details->findChild<ui::MsrButton*>("ChannelPattern"), "Pattern actions remain available");
    mixer.setSelectedTrack(QString::fromStdString(folder));
    check(details->findChild<QLabel*>("MixerDetailsEmpty") && !details->acceptDrops(),
          "unsupported folder shows an empty, inactive details state");
    mixer.setSelectedTrack(QString::fromStdString(second));
    controller.removeTrack(second);
    mixer.rebuild(); settle();
    check(details->findChild<QLabel*>("MixerDetailsEmpty"), "deleted channel clears details");

    // Large model fixture deliberately avoids compiling 1000 audio graphs.
    // Display preferences must not compile one either.
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    for (int i = int(project.tracks.size()); i < 1000; ++i) {
        daw::TrackModel track;
        track.id = "compact-test-" + std::to_string(i); track.name = "Track " + std::to_string(i + 1);
        track.kind = daw::TrackKind::Audio; track.color = 0x71A7BA;
        project.tracks.push_back(std::move(track));
    }
    project.invalidateTrackIndex();
    mixer.rebuild(); settle();
    mixer.m_scroll->horizontalScrollBar()->setValue(12345);
    settle();
    const int compactCount = int(mixer.m_strips.size());
    const double anchor = double(mixer.m_scroll->horizontalScrollBar()->value()) / (mixer.channelWidth() + 2);
    prefs.setCompactWidth(48); settle();
    check(std::abs(double(mixer.m_scroll->horizontalScrollBar()->value()) / 50 - anchor) < .02,
          "changing compact width preserves the fractional scroll anchor");
    prefs.setCompactWidth(32); settle();
    const auto layoutDepth = controller.undoDepth();
    const auto layoutGraph = controller.routingGraph();
    mixer.setSelectedTrack(QString::fromStdString(first));
    prefs.setView(ui::MixerPreferences::View::Classic);
    settle();
    check(mixer.channelWidth() == 111 && !mixer.m_details && mixer.m_selectedTrackId.toStdString() == first &&
          std::abs(double(mixer.m_scroll->horizontalScrollBar()->value()) / 113 - anchor) < .02,
          "Classic restores its own width and preserves selection and scroll anchor");
    prefs.setView(ui::MixerPreferences::View::Compact);
    settle();
    check(mixer.channelWidth() == 32 && std::abs(double(mixer.m_scroll->horizontalScrollBar()->value()) / 34 - anchor) < .02,
          "returning to Compact preserves the channel scroll anchor");
    check(controller.undoDepth() == layoutDepth && controller.routingGraph() == layoutGraph,
          "switching views leaves project history and audio graph unchanged");
    mixer.setRackMode(true);
    mixer.setRackMode(false);
    settle();
    check(mixer.m_view == ui::MixerPreferences::View::Compact && mixer.m_details->trackId().toStdString() == first,
          "Rack returns to the chosen mixer view and selection");
    mixer.m_scroll->horizontalScrollBar()->setValue(mixer.m_scroll->horizontalScrollBar()->maximum());
    settle();
    check(compactCount < 180 && mixer.m_strips.size() < 180 &&
          mixer.m_details && mixer.findChildren<ui::MiniModuleRack*>().size() < 4,
          "1000 channels retain only a viewport-sized set of slim strips and one details rack");
    std::printf("Compact mixer residency: %zu strips, %zu cached shells, %lld project tracks\n",
                mixer.m_strips.size(), mixer.m_recycledStrips.size(), qlonglong(project.tracks.size()));
    check(std::all_of(mixer.m_strips.begin(), mixer.m_strips.end(), [&](auto* item) {
        return item->presentation() == ChannelStrip::Presentation::Compact;
    }), "recycled strips retain compact presentation");
    QWidget dock;
    dock.resize(1000, 650);
    auto* dockLayout = new QVBoxLayout(&dock);
    dockLayout->setContentsMargins(0, 0, 0, 0);
    mixer.setParent(&dock);
    dockLayout->addWidget(&mixer);
    dock.show(); mixer.show(); settle();
    check(mixer.m_detailsScroll->width() == 120, "docked mixer retains details width");
    dockLayout->removeWidget(&mixer);
    mixer.setParent(nullptr);
    mixer.resize(1000, 650); mixer.show(); settle();
    check(mixer.m_detailsScroll->width() == 120 && mixer.m_view == ui::MixerPreferences::View::Compact,
          "detaching keeps compact view and panel size");
    mixer.hide();
    // Exercise the actual shell wiring in both directions.
    MainWindow window(false);
    auto* appController = window.collaborationEngineController();
    const auto appFirst = appController->addTrack(daw::TrackKind::Audio, "First");
    const auto appSecond = appController->addTrack(daw::TrackKind::Audio, "Second");
    auto* tracks = window.findChild<TrackListWidget*>();
    auto* appMixer = window.findChild<MixerWidget*>();
    if (!tracks || !appMixer) return false;
    tracks->rebuild(); appMixer->rebuild();
    tracks->setSelectedTrack(QString::fromStdString(appFirst));
    emit tracks->selectionChanged(QString::fromStdString(appFirst));
    QApplication::processEvents();
    check(appMixer->m_selectedTrackId.toStdString() == appFirst &&
          appMixer->m_details->trackId().toStdString() == appFirst, "arrangement selection reaches compact details");
    emit appMixer->trackSelected(QString::fromStdString(appSecond));
    QApplication::processEvents();
    check(tracks->selectedTrackId().toStdString() == appSecond &&
          appMixer->m_details->trackId().toStdString() == appSecond, "mixer selection reaches arrangement and details");
    return ok;
}
