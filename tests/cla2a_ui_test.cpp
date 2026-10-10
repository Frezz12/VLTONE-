#include "Cla2aPanel.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include "Recording/RecordingEngine.hpp"
#include "cloud/PublishPreflight.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QAccessible>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace {
namespace comp = daw::plugins::cla2a;
int failures = 0;
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }
void events(int ms = 65) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
void mouse(QWidget* w, QEvent::Type type, QPoint local, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type, QPointF(local), QPointF(w->mapToGlobal(local)), button, buttons, modifiers);
    QApplication::sendEvent(w, &event);
}
double difference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    double error = 0; for (unsigned i = 0; i < a.size(); ++i) error = std::max(error, std::abs(double(a[i]) - b[i]));
    return error;
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ui::registerFontUrlScheme(); QApplication app(argc, argv); ui::initializeApplicationFonts();
    QTemporaryDir temporary; app.setProperty("dawHeadlessDataRoot", temporary.path());
    app.setOrganizationName("VLTONE-CLA2A-Test"); app.setApplicationName("Cla2aNative");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QTranslator translation;
    const auto translationPath = qEnvironmentVariable("DAW_UI_CHECK_TRANSLATION");
    if (!translationPath.isEmpty()) { check(translation.load(translationPath), "UI check translation loads"); app.installTranslator(&translation); }
    const bool pointerCheck = app.arguments().contains("--pointer-check");
    if (pointerCheck && !qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE")) {
        check(false, "pointer check requires VLT_GPU_WORKSPACE=1"); return 1;
    }
    daw::EngineController controller{}; check(bool(controller.initialize(48000, 257, false)), "headless engine initializes");
    const auto descriptor = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.cla2a");
    check(descriptor.has_value() && descriptor->name == "VLT 2A", "plugin browser discovers built-in VLT 2A"); if (!descriptor) return 1;
    // The comparison exports the first second. Keep the clip's live-only
    // boundary de-click beyond that range so this measures the effect/PDC.
    audio::AudioBuffer source(2, 96000);
    for (unsigned i = 0; i < source.numFrames(); ++i) source.getChannel(0)[i] = source.getChannel(1)[i] = float(.3 * std::sin(2 * std::numbers::pi * 431 * i / 48000));
    const auto sourcePath = temporary.path().toStdString() + "/source.wav";
    audio::AudioRecorder writer; writer.initialize(48000, 2); writer.writeWAVFile(sourcePath, source, 48000);
    const auto track = controller.importAudioToNewTrack(sourcePath, 0); const auto insert = controller.addInsert(track, *descriptor);
    check(!insert.empty(), "CLA-2A inserts through the standard host"); if (insert.empty()) return 1;
    controller.undo(); check(!controller.insertInstance(track, insert), "undo removes the inserted effect");
    controller.redo(); check(controller.insertInstance(track, insert), "redo restores the inserted effect");
    check(!controller.insertSupportsSidechain(track, insert) && daw::cloud::isSupportedBuiltinV1(*controller.insertModel(track, insert)),
          "classic buses and cloud compatibility are registered");
    if (qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE")) {
        for (const bool compressorFirst : {false, true}) {
            QWidget workspace; workspace.resize(920, 440);
            QPushButton focusTarget("Other control", &workspace); focusTarget.setGeometry(20, 10, 140, 28);
            ui::Knob shared({}, &workspace); shared.setBare(36); shared.setRange(0, 100); shared.setValue(50);
            shared.setFocusPolicy(Qt::StrongFocus); shared.move(200, 10);
            ui::Knob sampler({}, &workspace); sampler.setBare(36); sampler.setRange(0, 100); sampler.setValue(50); sampler.move(260, 10);
            auto* compressor = new Cla2aPanel(&controller, QString::fromStdString(track), QString::fromStdString(insert), &workspace);
            compressor->move(0, 65);
            ui::graphics::WorkspaceSurface surface(&workspace);
            workspace.show(); workspace.activateWindow(); events(); surface.quickWindow()->requestActivate(); events();
            const auto pointer = [&](QWidget* target, QEvent::Type type, QPoint offset, Qt::MouseButton button, Qt::MouseButtons buttons) {
                const auto global = target->mapToGlobal(target->rect().center() + offset);
                const auto scene = surface.quickWindow()->mapFromGlobal(global);
                QMouseEvent event(type, QPointF(scene), QPointF(global), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(surface.quickWindow(), &event); events(35);
            };
            auto* routedGain = compressor->findChild<ui::Knob*>("Cla2aGain");
            auto* routedPeak = compressor->findChild<ui::Knob*>("Cla2aPeakReduction");
            check(routedGain && routedPeak, "both physical knobs are available for routed input");
            if (!routedGain || !routedPeak) return 1;
            std::array<ui::Knob*, 4> knobs{&shared, routedGain, routedPeak, &sampler};
            if (compressorFirst) std::swap(knobs[0], knobs[1]);
            for (auto* knob : knobs) {
                pointer(&focusTarget, QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton);
                pointer(&focusTarget, QEvent::MouseButtonRelease, {}, Qt::LeftButton, Qt::NoButton);
                const double before = knob->value(); int finished = 0;
                const auto connection = QObject::connect(knob, &ui::Knob::editFinished, [&] { ++finished; });
                pointer(knob, QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton);
                check(knob->value() == before, "first press never jumps the knob value");
                pointer(knob, QEvent::MouseMove, QPoint(0, -55), Qt::NoButton, Qt::LeftButton);
                check(knob->value() > before, "first drag updates an unfocused knob outside its bounds through Quick");
                pointer(knob, QEvent::MouseButtonRelease, QPoint(0, -55), Qt::LeftButton, Qt::NoButton);
                check(finished == 1, "first routed drag finishes exactly once");
                QObject::disconnect(connection);
                if (knob == routedGain || knob == routedPeak) { controller.undo(); events(); }
            }
            int interrupted = 0;
            QObject::connect(&shared, &ui::Knob::editFinished, &workspace, [&] { ++interrupted; });
            pointer(&shared, QEvent::MouseButtonPress, {}, Qt::LeftButton, Qt::LeftButton);
            pointer(&shared, QEvent::MouseMove, QPoint(0, -10), Qt::NoButton, Qt::LeftButton);
            QApplication::setActiveWindow(nullptr);
            QEvent deactivate(QEvent::WindowDeactivate); QApplication::sendEvent(surface.quickWindow(), &deactivate);
            check(!shared.isEditing() && interrupted == 1, "leaving the workspace still completes an active gesture once");
            pointer(&shared, QEvent::MouseButtonRelease, QPoint(0, -10), Qt::LeftButton, Qt::NoButton);
            check(interrupted == 1, "release after workspace deactivation cannot commit twice");
        }
        if (pointerCheck) return failures ? 1 : 0;
    }
    Cla2aPanel panel(&controller, QString::fromStdString(track), QString::fromStdString(insert)); panel.show(); panel.activateWindow(); events();
    auto* gain = panel.findChild<ui::Knob*>("Cla2aGain"); auto* peak = panel.findChild<ui::Knob*>("Cla2aPeakReduction");
    auto* mode = panel.findChild<QAbstractButton*>("Cla2aMode"); auto* timer = panel.findChild<QTimer*>("Cla2aTelemetryTimer");
    check(gain && peak && mode && timer && timer->isActive() && panel.minimumSize() == QSize(688, 286), "native panel controls and visible-only 33 ms metering");
    if (!gain || !peak || !mode || !timer) return 1;
    const auto value = [&](const char* id) { return controller.insertParameter(track, insert, id); };
    auto* accessible = QAccessible::queryAccessibleInterface(gain);
    check(accessible && accessible->role() == QAccessible::Slider && accessible->valueInterface() && accessible->valueInterface()->currentValue().toDouble() == 40,
          "knob exposes an accessible role, name, range and value");
    auto depth = controller.undoDepth();
    if (accessible && accessible->valueInterface()) accessible->valueInterface()->setCurrentValue(47.25);
    check(value("gain") == 47.25 && controller.undoDepth() == depth + 1, "assistive value edit follows the normal undo path"); controller.undo(); events();
    controller.redo(); check(value("gain") == 47.25, "parameter redo restores the edited value"); controller.undo(); events();

    const QPoint start = gain->rect().center(); depth = controller.undoDepth();
    mouse(gain, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(gain, QEvent::MouseMove, start + QPoint(0, -50), Qt::NoButton, Qt::LeftButton);
    const double coarse = value("gain");
    mouse(gain, QEvent::MouseMove, start + QPoint(0, -60), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    check(std::abs(value("gain") - coarse - 100. / 60) < 1e-8, "adding Shift mid-drag changes precision without jumping");
    mouse(gain, QEvent::MouseButtonRelease, start + QPoint(0, -60), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    check(controller.undoDepth() == depth + 1, "one drag is one undo"); controller.undo(); events();
    mouse(gain, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(gain, QEvent::MouseMove, start + QPoint(0, -400), Qt::NoButton, Qt::LeftButton);
    mouse(gain, QEvent::MouseMove, start + QPoint(0, -390), Qt::NoButton, Qt::LeftButton);
    check(value("gain") < 100 && value("gain") > 90, "direction reverses immediately after a clamped overshoot");
    mouse(gain, QEvent::MouseButtonRelease, start + QPoint(0, -390), Qt::LeftButton, Qt::NoButton); controller.undo(); events();

    depth = controller.undoDepth();
    for (unsigned i = 0; i < 3; ++i) {
        QWheelEvent event(QPointF(start), QPointF(gain->mapToGlobal(start)), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(gain, &event);
    }
    events(220); check(value("gain") == 46 && controller.undoDepth() == depth + 1, "wheel burst is grouped into one undo"); controller.undo(); events();
    QTest::keyClick(gain, Qt::Key_Right); check(value("gain") == 41, "keyboard adjusts immediately"); controller.undo(); events();
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (!dialog) { check(false, "numeric dialog opens"); return; }
        auto* spin = dialog->findChild<QDoubleSpinBox*>();
        if (!spin) { dialog->reject(); check(false, "numeric input is native"); return; }
        spin->setFocus(); QTest::keyClick(spin, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(spin, spin->locale().toString(47.35, 'f', 2)); QTest::keyClick(dialog, Qt::Key_Return);
    });
    panel.findChild<QPushButton*>("Cla2aGainValue")->click();
    check(std::abs(value("gain") - 47.35) < 1e-9, "typed numerical input commits precise values"); controller.undo(); events();
    QString automated; QObject::connect(&panel, &Cla2aPanel::automationRequested, [&](const QString& id) { automated = id; });
    QTimer::singleShot(0, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) { check(false, "automation context menu opens"); return; }
        for (auto* action : menu->actions()) if (action->objectName() == "Cla2aAutomate") { QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(action).center()); return; }
        menu->close();
    });
    QContextMenuEvent context(QContextMenuEvent::Mouse, peak->rect().center(), peak->mapToGlobal(peak->rect().center())); QApplication::sendEvent(peak, &context);
    check(automated == "peakReduction", "automation uses the stable parameter ID");
    QTest::keyClick(mode, Qt::Key_Space); check(value("mode") == 1 && mode->isChecked(), "mode is keyboard accessible and model backed"); controller.undo(); events();

    audio::AudioBuffer input(2, 257), output(2, 257); input.clear(); QTimer pump; unsigned blocks = 0; bool rendered = true; double energy = 0;
    QObject::connect(&pump, &QTimer::timeout, [&] {
        rendered &= controller.processDeviceBlockForTest(input, output, 257); ++blocks;
        for (unsigned i = 0; i < 257; ++i) energy += output.getChannel(0)[i] * output.getChannel(0)[i];
    });
    controller.seekSeconds(0); controller.play(); pump.start(5); depth = controller.undoDepth();
    const QPoint at = peak->rect().center(); mouse(peak, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    for (unsigned i = 1; i <= 10; ++i) { mouse(peak, QEvent::MouseMove, at + QPoint(0, -int(i) * 7), Qt::NoButton, Qt::LeftButton); events(34); }
    mouse(peak, QEvent::MouseButtonRelease, at + QPoint(0, -70), Qt::LeftButton, Qt::NoButton);
    pump.stop(); controller.stop();
    check(rendered && blocks > 10 && energy > 1 && value("peakReduction") > 40 && controller.undoDepth() == depth + 1,
          "device audio and telemetry keep running during a single undoable gesture"); controller.undo(); events();
    depth = controller.undoDepth(); mouse(gain, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(gain, QEvent::MouseMove, start + QPoint(0, -10), Qt::NoButton, Qt::LeftButton);
    QEvent ungrab(QEvent::UngrabMouse); QApplication::sendEvent(gain, &ungrab);
    check(controller.undoDepth() == depth + 1, "losing pointer capture commits the gesture once"); controller.undo(); events();
    depth = controller.undoDepth(); mouse(peak, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    mouse(peak, QEvent::MouseMove, at + QPoint(0, -15), Qt::NoButton, Qt::LeftButton); panel.hide();
    check(!timer->isActive() && controller.undoDepth() == depth + 1, "hiding stops metering and completes an active edit"); controller.undo(); panel.show(); events();
    {
        auto closing = std::make_unique<Cla2aPanel>(&controller, QString::fromStdString(track), QString::fromStdString(insert));
        closing->show(); events(); auto* closingGain = closing->findChild<ui::Knob*>("Cla2aGain"); const auto where = closingGain->rect().center();
        depth = controller.undoDepth(); mouse(closingGain, QEvent::MouseButtonPress, where, Qt::LeftButton, Qt::LeftButton);
        mouse(closingGain, QEvent::MouseMove, where + QPoint(0, -15), Qt::NoButton, Qt::LeftButton); closing.reset();
        check(controller.undoDepth() == depth + 1, "destroying a panel during a drag completes the undoable edit"); controller.undo(); events();
    }

    controller.setInsertParameter(track, insert, "gain", 48.25); controller.setInsertParameter(track, insert, "peakReduction", 57); controller.setInsertParameter(track, insert, "mode", 1);
    const auto copy = controller.copyChannelStrip(track, false); const auto other = controller.addTrack(daw::TrackKind::Audio, "Copy");
    check(controller.pasteChannelInserts(other, copy), "CLA-2A copies through opaque state");
    const auto copied = controller.project().findTrack(other)->inserts.front().id;
    check(controller.insertParameter(other, copied, "gain") == 48.25 && controller.insertParameter(other, copied, "peakReduction") == 57 && controller.insertParameter(other, copied, "mode") == 1,
          "copied effect preserves all three values");
    controller.setInsertBypassed(track, insert, true);
    const auto projectPath = temporary.path().toStdString() + "/cla2a.vlt"; check(bool(controller.saveProject(projectPath)), "CLA-2A project saves");
    {
        daw::EngineController reopened{}; reopened.initialize(48000, 257, false);
        check(bool(reopened.openProject(projectPath)) && reopened.insertParameter(track, insert, "gain") == 48.25 &&
              reopened.insertParameter(track, insert, "peakReduction") == 57 && reopened.insertParameter(track, insert, "mode") == 1 && reopened.insertModel(track, insert)->bypassed,
              "project restores state and host bypass");
    }
    controller.setInsertBypassed(track, insert, false); controller.setInsertParameter(track, insert, "gain", 40);
    controller.setInsertParameter(track, insert, "peakReduction", 50); controller.setInsertParameter(track, insert, "mode", 0);
    controller.pumpPluginEvents();
    QString screenshots;
    for (int i = 1; i + 1 < argc; ++i) if (QString::fromLocal8Bit(argv[i]) == "--screenshots") screenshots = QString::fromLocal8Bit(argv[i + 1]);
    if (!screenshots.isEmpty()) QDir().mkpath(screenshots);
    for (const auto size : {QSize(688, 286), QSize(820, 310), QSize(1120, 400)}) {
        panel.resize(size); events();
        check(panel.rect().contains(gain->geometry()) && panel.rect().contains(peak->geometry()) && panel.rect().contains(mode->geometry()), "controls stay in bounds while resizing");
        const auto shot = panel.grab(); check(!shot.isNull() && shot.devicePixelRatio() >= 1, "native panel renders at the current display DPR");
        if (!screenshots.isEmpty()) check(shot.save(screenshots + QString("/cla2a-%1x%2.png").arg(size.width()).arg(size.height())), "panel screenshot saves");
    }

    {
        daw::EngineController render{}; render.initialize(48000, 257, false);
        const auto audio = render.importAudioToNewTrack(sourcePath, 0); const auto fx = render.addInsert(audio, *descriptor);
        render.setInsertParameter(audio, fx, "peakReduction", 50); render.pumpPluginEvents();
        audio::AudioBuffer in(2, 257), out(2, 257); in.clear();
        const auto captureDevice = [&] {
            render.stop(); render.seekSeconds(0); render.pumpPluginEvents();
            // Both paths start from the same saved state. Reusing the previous
            // live engine carries transport de-click and automation history
            // that a fresh export deliberately does not inherit.
            const auto comparisonProject = temporary.path().toStdString() + "/comparison.vlt";
            daw::EngineController playback{};
            bool deviceRendered = bool(render.saveProject(comparisonProject)) &&
                bool(playback.initialize(48000, 257, false)) && bool(playback.openProject(comparisonProject));
            playback.pumpPluginEvents();
            const auto graph = playback.routingGraph();
            check(graph && graph->totalLatency == render.routingGraph()->totalLatency,
                  "fresh playback preserves the graph latency");
            if (graph) {
                for (const auto& entry : graph->nodes) entry.node->reset();
                for (const auto& delay : graph->delays) delay->reset();
                for (const auto& delay : graph->midiDelays) delay->reset();
            }
            std::vector<float> captured(48544 * 2); playback.play();
            for (unsigned startFrame = 0; startFrame < 48544; startFrame += 257) {
                const unsigned frames = std::min(257u, 48544 - startFrame);
                deviceRendered &= playback.processDeviceBlockForTest(in, out, frames);
                for (unsigned i = 0; i < frames; ++i) for (unsigned ch = 0; ch < 2; ++ch) captured[(startFrame + i) * 2 + ch] = out.getChannel(ch)[i];
            }
            check(deviceRendered, "device comparison renders every block"); playback.stop(); return captured;
        };
        const auto latency = render.routingGraph()->totalLatency;
        const auto live = captureDevice();
        daw::rendering::Spec spec; spec.outputDir = temporary.path().toStdString(); spec.baseName = "cla2a-render";
        spec.range = daw::rendering::Range::Custom; spec.customEndSeconds = 1;
        spec.file.container = audio::platform::Container::Wav; spec.file.encoding = audio::platform::Encoding::Float32; spec.stemChannelIds = {audio};
        daw::rendering::Report report; check(bool(render.renderProject(spec, {}, report)) && report.files.size() == 2, "CLA-2A master and stem export");
        audio::platform::DecodedAudio wet;
        if (!report.files.empty()) audio::platform::decodeAudioFile(report.files.front(), wet);
        double liveError = 1e9;
        unsigned liveErrorFrame = 0;
        if (wet.frames == 48000) {
            liveError = 0;
            for (unsigned i = 0; i < 48000; ++i) for (unsigned ch = 0; ch < 2; ++ch) {
                const auto error = std::abs(double(live[(i + latency) * 2 + ch]) - wet.interleaved[i * 2 + ch]);
                if (error > liveError) { liveError = error; liveErrorFrame = i; }
            }
        }
        std::printf("MEASURE device/export maximum error %.9g at frame %u\n", liveError, liveErrorFrame);
        check(liveError < 2e-5, "device playback and export match after the declared graph latency");
        audio::platform::DecodedAudio stem; if (report.files.size() > 1) audio::platform::decodeAudioFile(report.files[1], stem);
        check(difference(wet.interleaved, stem.interleaved) < 2e-5, "single-track master and stem are aligned");

        const std::array<std::vector<daw::AutomationPoint>, 3> curves{{
            {{0, .4}, {.9, .46}, {2, .4}},
            {{0, .5}, {.8, .64}, {2, .5}},
            {{0, 0, daw::AutomationSegment::Hold}, {1, 1, daw::AutomationSegment::Hold}, {1.5, 0, daw::AutomationSegment::Hold}, {2, 0}}
        }};
        bool createdAutomation = true; std::array<std::string, 3> automationLanes;
        for (unsigned i = 0; i < 3; ++i) {
            daw::AutomationTarget target; target.kind = daw::AutomationTargetKind::PluginParameter;
            target.channelId = audio; target.slotId = fx; target.parameterId = comp::parameterTable()[i].id;
            const auto lane = render.addAutomationLane(audio, target);
            const auto clip = render.addAutomationClip(lane, target, 0, 1);
            automationLanes[i] = lane;
            render.setAutomationPoints(lane, clip, curves[i]); createdAutomation &= !lane.empty() && !clip.empty();
        }
        check(createdAutomation, "host automation lanes bind all three CLA-2A parameters");
        const auto automatedLive = captureDevice();
        spec.baseName = "cla2a-automated"; spec.blockSize = 257;
        daw::rendering::Report automatedReport; audio::platform::DecodedAudio automatedAudio;
        bool automatedMatch = bool(render.renderProject(spec, {}, automatedReport)) && !automatedReport.files.empty();
        if (automatedMatch) automatedMatch = bool(audio::platform::decodeAudioFile(automatedReport.files.front(), automatedAudio));
        double automationError = 1e9;
        unsigned automationErrorFrame = 0;
        if (automatedMatch && automatedAudio.frames == 48000) {
            automationError = 0;
            for (unsigned i = 0; i < 48000; ++i) for (unsigned ch = 0; ch < 2; ++ch) {
                const auto error = std::abs(double(automatedLive[(i + latency) * 2 + ch]) - automatedAudio.interleaved[i * 2 + ch]);
                if (error > automationError) { automationError = error; automationErrorFrame = i; }
            }
        }
        std::printf("MEASURE automated device/export maximum error %.9g at frame %u\n", automationError, automationErrorFrame);
        check(automationError < 2e-5 && difference(wet.interleaved, automatedAudio.interleaved) > .001,
              "Gain, Peak Reduction and mode automation affect audio identically in playback and export");
        daw::rendering::Report guardedFreeze;
        check(!render.freezeTrack(audio, {}, guardedFreeze) && !render.isTrackFrozen(audio), "active external automation uses the DAW's freeze guard");
        for (const auto& lane : automationLanes) render.removeAutomationLane(lane);
        render.setInsertParameter(audio, fx, "gain", 40); render.setInsertParameter(audio, fx, "peakReduction", 50); render.setInsertParameter(audio, fx, "mode", 0);
        spec.stemChannelIds.clear(); spec.baseName = "cla2a-frozen"; daw::rendering::Report frozen;
        const auto froze = render.freezeTrack(audio, {}, frozen);
        if (!froze) std::printf("freeze error: %s\n", froze.message().c_str());
        check(bool(froze) && render.isTrackFrozen(audio), "CLA-2A track freezes");
        daw::rendering::Report after; audio::platform::DecodedAudio frozenAudio;
        bool freezeMatch = bool(froze) && bool(render.renderProject(spec, {}, after)) && !after.files.empty();
        if (freezeMatch) freezeMatch = bool(audio::platform::decodeAudioFile(after.files.front(), frozenAudio));
        check(freezeMatch && difference(wet.interleaved, frozenAudio.interleaved) < 2e-5, "frozen playback preserves optical processing and alignment");
        render.unfreezeTrack(audio); render.setInsertBypassed(audio, fx, true); spec.baseName = "cla2a-bypass";
        daw::rendering::Report bypass; audio::platform::DecodedAudio dry;
        bool dryMatch = bool(render.renderProject(spec, {}, bypass)) && !bypass.files.empty();
        if (dryMatch) dryMatch = bool(audio::platform::decodeAudioFile(bypass.files.front(), dry));
        double dryError = 0;
        if (dry.frames != 48000) dryMatch = false;
        else for (unsigned i = 0; i < 48000; ++i) dryError = std::max(dryError, std::abs(double(dry.interleaved[2 * i]) - source.getChannel(0)[i]));
        check(dryMatch && dryError < 2e-5, "host bypass preserves sample alignment and original audio");
    }
    return failures ? 1 : 0;
}
