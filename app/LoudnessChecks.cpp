#include "LoudnessDisplay.hpp"
#include "ChannelStrip.hpp"
#include "MixerWidget.hpp"
#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <numbers>

bool LoudnessDisplay::checkForTest(const QString& screenshotPrefix) {
    bool ok = true;
    const auto check = [&](bool value, const char* message) {
        std::fprintf(stderr, "%s loudness display: %s\n", value ? "PASS" : "FAIL", message);
        ok &= value;
    };
    daw::EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return false;
    QTemporaryDir temporary;
    const auto path = temporary.filePath("Loudness reference.wav").toStdString();
    audio::AudioBuffer signal(2, 192000);
    for (unsigned i = 0; i < signal.numFrames(); ++i) {
        signal.getChannel(0)[i] = .1f * std::sin(2 * std::numbers::pi * 1000 * i / 48000);
        signal.getChannel(1)[i] = -signal.getChannel(0)[i];
    }
    audio::AudioRecorder writer; writer.initialize(48000, 2);
    if (!writer.writeWAVFile(path, signal, 48000)) return false;
    const auto track = controller.importAudioToNewTrack(path, 0, "Audio");
    controller.addTrack(daw::TrackKind::Midi, "MIDI");
    controller.addTrack(daw::TrackKind::Bus, "Bus");
    controller.seekSeconds(0); controller.play();
    audio::AudioBuffer input(2, 256), output(2, 256); input.clear();
    for (int i = 0; i < 620; ++i) {
        controller.processDeviceBlockForTest(input, output, 256);
        // Imported audio streams asynchronously. Let its read-ahead worker
        // keep up, as it would with real 256-frame device callbacks.
        QThread::usleep(5500);
    }
    const auto measured = controller.masterLoudness();
    check(std::isfinite(measured.shortTerm) && std::isfinite(measured.integrated), "controller publishes measured playback loudness");
    controller.pause();
    MixerWidget mixer(&controller);
    mixer.resize(620, 650); mixer.show();
    for (int i = 0; i < 24; ++i) QApplication::processEvents();
    mixer.refreshMeters();
    auto* display = mixer.findChild<LoudnessDisplay*>();
    if (!display) return false;
    const auto value = QString::number(measured.shortTerm, 'f', 1);
    check(display->toolTip().contains(value) && display->accessibleName().contains(value), "visible and accessible readings use engine data");
    if (!screenshotPrefix.isEmpty()) check(mixer.grab().save(screenshotPrefix + "-live.png"), "saves the live master and aligned channel rows");
    const auto before = controller.undoDepth();
    display->click();
    check(std::isnan(controller.masterLoudness().integrated) && !display->toolTip().contains(value), "click resets engine and display together");
    check(controller.undoDepth() == before, "meter reset does not edit the project");
    for (int width : {75, 100, 180}) {
        ChannelStrip inspector(&controller, QString::fromStdString(track), false);
        inspector.setInspectorCompact(true); inspector.resize(width, inspector.naturalHeight());
        inspector.show(); QApplication::processEvents();
        auto* fx = inspector.findChild<QWidget*>("ChannelInsertsRow");
        auto* sends = inspector.findChild<QWidget*>("ChannelSendsRow");
        check(fx && sends && fx->mapTo(&inspector, QPoint()).y() + fx->height() <= sends->mapTo(&inspector, QPoint()).y(),
              "inspector keeps FX above sends at each supported width");
    }
    check(bool(controller.newProject()), "new project commits");
    check(std::isnan(controller.masterLoudness().integrated), "new project starts a new measurement");
    return ok;
}
