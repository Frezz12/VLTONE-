#include "EngineController.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <vector>

namespace {
using daw::plugins::pitch::PitchCorrectorInstance;
constexpr double rate = 48000.0;
constexpr unsigned block = 128;
int failures = 0;

bool check(bool okay, const char* message) {
    std::printf("%s %s\n", okay ? "PASS" : "FAIL", message);
    failures += !okay;
    return okay;
}

PitchCorrectorInstance* instance(daw::EngineController& controller,
                                 const std::string& track, const std::string& slot) {
    return dynamic_cast<PitchCorrectorInstance*>(controller.insertInstance(track, slot));
}

void silence(daw::EngineController& controller, unsigned blocks = 80) {
    audio::AudioBuffer input(2, block), output(2, block);
    input.clear(block);
    for (unsigned n = 0; n < blocks; ++n)
        controller.processDeviceBlockForTest(input, output, block);
}

float sourceSample(std::size_t frame) {
    return float(.2 * std::sin(2 * std::numbers::pi * 431.0 * double(frame) / rate));
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto directory = std::filesystem::temp_directory_path() /
                           ("vlt-pitch-integration-" + daw::newUuid());
    std::filesystem::create_directories(directory);
    const auto& descriptor = PitchCorrectorInstance::staticDescriptor();

    {
        daw::EngineController controller{};
        controller.initialize(rate, block, false);
        const auto lead = controller.addTrack(daw::TrackKind::Audio, "Lead");
        const auto doubles = controller.addTrack(daw::TrackKind::Audio, "Doubles");
        const auto source = controller.addInsert(lead, descriptor);
        const auto target = controller.addInsert(doubles, descriptor);
        const auto master = controller.addInsert(daw::EngineController::kMasterChannelId, descriptor);
        controller.setInsertChannelMode(doubles, target, daw::PluginChannelMode::DualMono);
        controller.setInsertEditorChannel(doubles, target, daw::PluginEditorChannel::Right);
        controller.setInsertBypassed(doubles, target, true);
        controller.setInsertParameter(lead, source, "tune", 95);
        controller.setInsertParameter(lead, source, "humanize", 37);
        controller.setInsertParameter(lead, source, "a4_hz", 432);
        const auto oldDepth = controller.undoDepth();
        std::size_t updated = 0;
        check(controller.pitchCorrectorCount() == 3, "pitch targets include tracks and master");
        check(bool(controller.applyKeyToPitchCorrectors(9, "natural_minor", &updated)) && updated == 3 &&
                  controller.undoDepth() == oldDepth + 1,
              "detected key applies to all pitch correctors in one undo action");
        check(controller.insertParameter(lead, source, "key") == 9 &&
                  controller.insertParameter(lead, source, "scale") == 2 &&
                  controller.insertParameter(lead, source, "humanize") == 37 &&
                  controller.insertParameter(lead, source, "a4_hz") == 432,
              "key import preserves unrelated vocal settings");
        check(controller.insertParameter(doubles, target, "key") == 9 &&
                  controller.insertModel(doubles, target)->editorChannel == daw::PluginEditorChannel::Right &&
                  controller.insertModel(doubles, target)->bypassed,
              "key import reaches the right channel and preserves bypass and editor side");
        controller.setInsertEditorChannel(doubles, target, daw::PluginEditorChannel::Left);
        check(controller.insertParameter(doubles, target, "key") == 9, "key import also reaches the left channel");
        controller.undo();
        check(controller.insertParameter(lead, source, "key") == 0 &&
                  controller.insertParameter(doubles, target, "scale") == 0,
              "one undo restores every pitch key and scale");
        controller.redo();
        check(controller.insertParameter(daw::EngineController::kMasterChannelId, master, "key") == 9,
              "redo restores the imported key on master");
        const auto depth = controller.undoDepth();
        check(!controller.applyKeyToPitchCorrectors(-1, "major") &&
                  !controller.applyKeyToPitchCorrectors(1, "unknown") && controller.undoDepth() == depth,
              "unavailable or unsupported key creates no partial edits");
        check(bool(controller.copyPitchCorrectorSettings(lead, source, &updated)) && updated == 2 &&
                  controller.undoDepth() == depth + 1 &&
                  controller.insertParameter(doubles, target, "humanize") == 37 &&
                  controller.insertParameter(daw::EngineController::kMasterChannelId, master, "a4_hz") == 432,
              "send to all copies complete settings in one undo step");
        controller.setInsertEditorChannel(doubles, target, daw::PluginEditorChannel::Right);
        check(controller.insertParameter(doubles, target, "humanize") == 37,
              "send to all updates both destination processors");
        controller.undo();
        check(controller.insertParameter(doubles, target, "humanize") == 0 &&
                  controller.insertParameter(lead, source, "humanize") == 37,
              "send undo restores destinations while retaining the source");
        controller.redo();
        const auto noOpDepth = controller.undoDepth();
        check(bool(controller.copyPitchCorrectorSettings(lead, source, &updated)) && updated == 0 &&
                  controller.undoDepth() == noOpDepth, "identical settings create no empty undo action");
        controller.setInsertParameter(lead, source, "quality", 1);
        controller.setInsertParameter(lead, source, "humanize", 71);
        controller.play();
        check(!controller.copyPitchCorrectorSettings(lead, source) &&
                  controller.insertParameter(doubles, target, "humanize") == 37,
              "live quality mismatch refuses the entire send before changing any target");
        controller.stop();
        const auto package = directory / "pitch-sync.vlt";
        check(bool(controller.saveProject(package.string())), "synchronized pitch settings save");
        daw::EngineController reopened{};
        reopened.initialize(rate, block, false);
        check(bool(reopened.openProject(package.string())) &&
                  reopened.insertParameter(doubles, target, "key") == 9 &&
                  reopened.insertParameter(doubles, target, "a4_hz") == 432,
              "synchronized settings survive reopening the project");
    }

    {
        daw::EngineController controller{};
        if (!check(bool(controller.initialize(rate, block, false)), "pitch controller initializes")) return 1;
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Vocal");
        const auto slot = controller.addInsert(track, descriptor);
        auto* corrector = instance(controller, track, slot);
        if (!check(corrector != nullptr, "pitch corrector instantiates through the live graph")) return 1;
        const auto liveLatency = corrector->latencySamples();
        check(liveLatency > 0 && liveLatency <= 480 && controller.latencySamples() == liveLatency,
              "Real-Time advertises its bounded processing delay to graph PDC");

        controller.setTrackMonitor(track, true);
        const auto rebuilds = controller.graphRebuildCountForTest();
        const auto gated = controller.gatedAudioBlocks();
        controller.setInsertParameter(track, slot, "quality", 1);
        controller.commitInsertParameterEdit(track, slot, "quality", 0, "HD quality");
        controller.pumpPluginEvents();
        check(corrector->activeQuality() == 0 && corrector->qualityChangePending() &&
                  corrector->latencySamples() == liveLatency &&
                  controller.graphRebuildCountForTest() == rebuilds && controller.gatedAudioBlocks() == gated,
              "monitoring defers quality and PDC changes without parking the live renderer");

        // Saved state records the selected mode even while this running instance
        // retains Real-Time. A newly opened instance starts in the selected HD mode.
        const auto package = (directory / "pending-quality.vlt").string();
        controller.setInsertParameter(track, slot, "a4_hz", 442);
        check(bool(controller.saveProject(package)), "pending quality and reference tuning save");
        {
            daw::EngineController reopened{};
            reopened.initialize(rate, block, false);
            const bool opened = bool(reopened.openProject(package));
            auto* restored = instance(reopened, track, slot);
            check(opened && restored && restored->activeQuality() == 1 &&
                      !restored->qualityChangePending() && restored->latencySamples() > liveLatency &&
                      reopened.insertParameter(track, slot, "a4_hz") == 442,
                  "a newly opened project adopts the saved selected HD quality");
        }
        check(corrector->activeQuality() == 0 && corrector->qualityChangePending(),
              "saving and reopening a copy do not change the monitored instance");

        controller.undo();
        controller.pumpPluginEvents();
        check(!corrector->qualityChangePending() && corrector->activeQuality() == 0,
              "undo cancels a pending quality request during monitoring");
        controller.redo();
        controller.pumpPluginEvents();
        check(corrector->qualityChangePending() && corrector->activeQuality() == 0,
              "redo restores a pending quality request without a live restart");

        check(controller.setInsertChannelMode(track, slot, daw::PluginChannelMode::Mono) &&
                  corrector->activeQuality() == 0 && corrector->qualityChangePending(),
              "an unrelated bus reprepare preserves the active quality while monitoring");
        controller.setTrackMonitor(track, false);
        silence(controller);
        const auto beforeApply = controller.graphRebuildCountForTest();
        controller.pumpPluginEvents();
        check(corrector->activeQuality() == 1 && !corrector->qualityChangePending() &&
                  corrector->latencySamples() > liveLatency &&
                  controller.latencySamples() == corrector->latencySamples() &&
                  controller.graphRebuildCountForTest() == beforeApply + 1,
              "the first idle pump adopts HD and rebuilds compensation once");
        controller.pumpPluginEvents();
        check(controller.graphRebuildCountForTest() == beforeApply + 1,
              "settled quality does not repeatedly rebuild the graph");

        controller.play();
        controller.setInsertParameter(track, slot, "quality", 0);
        controller.pumpPluginEvents();
        check(corrector->activeQuality() == 1 && corrector->qualityChangePending(),
              "playback also defers the return to Real-Time");
        controller.stop();
        silence(controller);
        controller.pumpPluginEvents();
        check(corrector->activeQuality() == 0 && controller.latencySamples() == liveLatency,
              "stopping playback permits the requested Real-Time latency");

        check(controller.setInsertChannelMode(track, slot, daw::PluginChannelMode::DualMono),
              "pitch correction supports the host's dual-mono insert layout");
        auto* left = instance(controller, track, slot);
        controller.setInsertEditorChannel(track, slot, daw::PluginEditorChannel::Right);
        auto* right = instance(controller, track, slot);
        controller.setTrackMonitor(track, true);
        controller.setInsertParameter(track, slot, "quality", 1);
        controller.pumpPluginEvents();
        check(left && right && left != right && right->qualityChangePending() &&
                  left->activeQuality() == 0 && right->activeQuality() == 0,
              "the right dual-mono processor independently defers its quality change");
        controller.setTrackMonitor(track, false);
        silence(controller);
        controller.pumpPluginEvents();
        check(left && right && left->activeQuality() == 0 && right->activeQuality() == 1 &&
                  controller.latencySamples() == right->latencySamples(),
              "idle applies right-channel HD and compensates the dual-mono graph");
        controller.setInsertEditorChannel(track, slot, daw::PluginEditorChannel::Left);

        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::PluginParameter;
        target.channelId = track; target.slotId = slot; target.parameterId = "quality";
        check(controller.ensureAutomation(target).first.empty(),
              "the controller refuses automation for structural quality");
        target.parameterId = "tune";
        check(!controller.ensureAutomation(target).first.empty(),
              "musical pitch correction controls retain ordinary automation");
    }

    {
        daw::EngineController controller{};
        controller.initialize(rate, block, false);
        controller.setRecordDirectory(directory.string());
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Live capture");
        controller.setTrackInputRouting(track, 0, 1, true);
        const auto slot = controller.addInsert(track, descriptor);
        controller.setInsertParameter(track, slot, "output_db", -12);
        controller.setInsertParameter(track, slot, "tune", 100);
        controller.setInsertParameter(track, slot, "humanize", 0);
        controller.setInsertParameter(track, slot, "vibrato", 0);
        controller.seekSeconds(1);
        if (!check(controller.startRecording(track), "live pitch-corrected monitoring records")) return 1;
        const auto graphDelay = controller.latencySamples();
        audio::AudioBuffer input(2, block), output(2, block);
        constexpr unsigned blocks = 400;
        double wetEnergy = 0;
        bool rendered = true;
        for (unsigned n = 0; n < blocks; ++n) {
            for (unsigned i = 0; i < block; ++i) {
                input.getChannel(0)[i] = sourceSample(std::size_t(n) * block + i);
                input.getChannel(1)[i] = .9f;
            }
            const auto time = std::int64_t(1'000'000'000) +
                              std::int64_t(std::llround(double(n * block) * 1e9 / rate));
            rendered &= controller.processDeviceBlockForTest(input, output, block, time, time + 10'000'000);
            if (n >= blocks / 2) for (unsigned i = 0; i < block; ++i)
                wetEnergy += double(output.getChannel(0)[i]) * output.getChannel(0)[i];
        }
        const auto run = controller.finalizeRecordingCapture();
        check(rendered && wetEnergy > .001 &&
                  std::sqrt(wetEnergy / double(blocks / 2 * block)) < .08,
              "monitored microphone audio passes through the wet insert and its output gain");
        if (check(run.tracks.size() == 1, "one dry microphone capture is finalized")) {
            const auto& capture = run.tracks.front();
            const double expectedStart = 1.0 - double(graphDelay + 480) / rate;
            check(capture.fileWriteSucceeded && std::abs(capture.startSeconds - expectedStart) < .5 / rate,
                  "dry capture placement subtracts graph PDC and ADC/DAC timestamps exactly once");
            audio::platform::DecodedAudio decoded;
            bool exact = bool(audio::platform::decodeAudioFile(capture.closedWavPath, decoded)) &&
                         decoded.channels == 1 && decoded.frames == blocks * block;
            if (exact) for (std::size_t i = 0; i < decoded.frames; ++i)
                exact &= std::abs(decoded.interleaved[i] - sourceSample(i)) < 1e-6f;
            check(exact, "the recorded WAV contains the original selected input, without pitch or gain processing");
        }
    }

    {
        constexpr unsigned frames = 12000;
        audio::AudioBuffer source(2, frames);
        for (unsigned i = 0; i < frames; ++i)
            source.getChannel(0)[i] = source.getChannel(1)[i] = sourceSample(i);
        const auto sourcePath = (directory / "source.wav").string();
        audio::AudioRecorder writer; writer.initialize(rate, 2);
        writer.writeWAVFile(sourcePath, source, rate);
        daw::EngineController controller{};
        controller.initialize(rate, block, false);
        controller.setRecordDirectory(directory.string());
        const auto track = controller.importAudioToNewTrack(sourcePath, 0);
        const auto slot = controller.addInsert(track, descriptor);
        controller.setInsertParameter(track, slot, "amount", 0);
        daw::rendering::Spec spec;
        spec.outputDir = directory.string(); spec.range = daw::rendering::Range::Custom;
        spec.customEndSeconds = double(frames) / rate;
        spec.file.container = audio::platform::Container::Wav;
        spec.file.encoding = audio::platform::Encoding::Float32;
        spec.stemChannelIds = {track};
        for (int quality : {0, 1}) {
            controller.setInsertParameter(track, slot, "quality", quality);
            controller.pumpPluginEvents();
            spec.baseName = quality ? "hd" : "live";
            daw::rendering::Report report;
            bool exact = bool(controller.renderProject(spec, {}, report)) && report.files.size() == 2;
            for (const auto& path : report.files) {
                audio::platform::DecodedAudio decoded;
                bool aligned = bool(audio::platform::decodeAudioFile(path, decoded)) &&
                               decoded.channels == 2 && decoded.frames == frames;
                if (aligned) for (unsigned i = 0; i < frames; ++i)
                    aligned &= std::abs(decoded.interleaved[2 * i] - sourceSample(i)) < 2e-5f;
                exact &= aligned;
            }
            check(exact, quality ? "HD master and stem exports compensate latency without shifting or truncating audio"
                                 : "Real-Time master and stem exports compensate latency without shifting or truncating audio");
        }
        daw::rendering::Report frozen;
        check(bool(controller.freezeTrack(track, {}, frozen)) && controller.isTrackFrozen(track),
              "HD pitch correction participates in the existing freeze pipeline");
        controller.setInsertParameter(track, slot, "tune", 80);
        check(!controller.isTrackFrozen(track), "a pitch correction edit invalidates the frozen track");
        const auto clip = controller.project().findTrack(track)->clips.front().id;
        const auto clipPitch = controller.addClipFxInsert(track, clip, descriptor);
        const auto samplerTrack = controller.addTrack(daw::TrackKind::Instrument, "Sampler pitch");
        check(controller.loadInstrumentSampler(samplerTrack, sourcePath), "sampler pitch fixture loads");
        const auto samplerSlot = controller.project().findTrack(samplerTrack)->instrument.id;
        const auto samplerPitch = controller.addSamplerFxInsert(samplerTrack, samplerSlot, descriptor);
        std::size_t changed = 0;
        check(!clipPitch.empty() && !samplerPitch.empty() && controller.pitchCorrectorCount() == 3 &&
                  bool(controller.applyKeyToPitchCorrectors(4, "major", &changed)) && changed == 3 &&
                  controller.insertParameter(track, clipPitch, "key") == 4 &&
                  controller.insertParameter(samplerTrack, samplerPitch, "scale") == 1,
              "project-wide key import includes clip and sampler FX");
        controller.setInsertParameter(track, clipPitch, "humanize", 42);
        check(bool(controller.copyPitchCorrectorSettings(track, clipPitch, &changed)) && changed == 2 &&
                  controller.insertParameter(samplerTrack, samplerPitch, "humanize") == 42 &&
                  controller.insertParameter(track, slot, "humanize") == 42,
              "send to all accepts a clip-FX source and reaches sampler FX");
    }

    // A corrected final syllable exercises both the shifter's buffered audio
    // and the renderer's latency flush. Amount=0 alone cannot catch a wet tail
    // being cut off. Keep the source end deliberately off a block boundary.
    {
        constexpr unsigned frames = 48073;
        audio::AudioBuffer source(2, frames);
        source.clear(frames);
        for (unsigned i = 2400; i < frames; ++i) {
            if (i >= 19200 && i < 30000) continue;
            const unsigned onset = i < 19200 ? 2400 : 30000;
            const double phase = 2 * std::numbers::pi * (i < 19200 ? 142.3 : 171.0) * i / rate;
            const float sample = float(.14 * std::min(1.0, double(i - onset) / 240) *
                (std::sin(phase) + .3 * std::sin(2 * phase) + .17 * std::sin(3 * phase)));
            source.getChannel(0)[i] = source.getChannel(1)[i] = sample;
        }
        const auto path = (directory / "last-syllable.wav").string();
        audio::AudioRecorder writer; writer.initialize(rate, 2);
        writer.writeWAVFile(path, source, rate);
        audio::platform::DecodedAudio inputFile;
        if (!check(bool(audio::platform::decodeAudioFile(path, inputFile)) && inputFile.frames == frames &&
                       inputFile.channels == 2, "the last-syllable fixture decodes")) return 1;

        for (int quality : {0, 1}) {
            daw::EngineController controller{};
            controller.initialize(rate, block, false);
            controller.setRecordDirectory(directory.string());
            const auto track = controller.importAudioToNewTrack(path, 0);
            const auto slot = controller.addInsert(track, descriptor);
            controller.setInsertParameter(track, slot, "tune", 100);
            controller.setInsertParameter(track, slot, "humanize", 0);
            controller.setInsertParameter(track, slot, "vibrato", 0);
            controller.setInsertParameter(track, slot, "quality", quality);
            controller.pumpPluginEvents();

            // This instance sees the same decoded source followed by a full
            // second of zeros. It does not use the graph, PDC or file renderer.
            std::vector<std::uint8_t> state;
            PitchCorrectorInstance reference;
            auto* live = instance(controller, track, slot);
            if (!check(live && live->saveState(state) && reference.loadState(state) &&
                           reference.activate({rate, block, true}), "wet tail reference prepares")) return 1;
            reference.startProcessing();
            const unsigned latency = reference.latencySamples();
            const unsigned tail = reference.tailSamples();
            const unsigned referenceFrames = frames + tail + unsigned(rate);
            audio::AudioBuffer input(2, referenceFrames), output(2, referenceFrames);
            input.clear(referenceFrames);
            for (unsigned i = 0; i < frames; ++i) for (unsigned channel = 0; channel < 2; ++channel)
                input.getChannel(channel)[i] = inputFile.interleaved[2 * i + channel];
            bool correctedLastSyllable = false;
            for (unsigned offset = 0; offset < referenceFrames; offset += block) {
                const unsigned count = std::min(block, referenceFrames - offset);
                const float* in[]{input.getChannel(0) + offset, input.getChannel(1) + offset};
                float* out[]{output.getChannel(0) + offset, output.getChannel(1) + offset};
                daw::plugins::PluginProcessContext context;
                context.inputs = in; context.outputs = out;
                context.inputChannels = context.outputChannels = 2;
                context.frames = count; context.sampleTime = offset;
                reference.process(context);
                if (offset + count <= frames && offset + count + block > frames) {
                    const auto telemetry = reference.telemetrySnapshot();
                    correctedLastSyllable = telemetry.voiced && std::abs(telemetry.correctionCents) > 10;
                }
            }
            double lastEnergy = 0, changedEnergy = 0;
            for (unsigned i = frames - 2400; i < frames; ++i) {
                const double wet = output.getChannel(0)[i + latency];
                lastEnergy += wet * wet;
                const double difference = wet - input.getChannel(0)[i];
                changedEnergy += difference * difference;
            }
            check(correctedLastSyllable && std::sqrt(lastEnergy / 2400) > .05 &&
                      std::sqrt(changedEnergy / 2400) > .01,
                  quality ? "HD reference applies nonzero correction to the final voiced syllable"
                          : "Real-Time reference applies nonzero correction to the final voiced syllable");
            float latePeak = 0;
            unsigned lastAudibleFrame = 0;
            bool finite = true;
            for (unsigned i = 0; i < referenceFrames; ++i) for (unsigned channel = 0; channel < 2; ++channel) {
                const float magnitude = std::abs(output.getChannel(channel)[i]);
                finite &= std::isfinite(magnitude);
                if (i >= frames + tail) latePeak = std::max(latePeak, magnitude);
                if (i >= latency && magnitude > 1e-5f) lastAudibleFrame = i - latency;
            }
            check(finite && tail >= latency && latePeak < 1e-5f,
                  quality ? "HD declared tail covers the complete wet decay after the input ends"
                          : "Real-Time declared tail covers the complete wet decay after the input ends");

            auto matchesReference = [&](const std::string& file, unsigned expectedFrames, bool includeTail) {
                audio::platform::DecodedAudio decoded;
                if (!audio::platform::decodeAudioFile(file, decoded) || decoded.channels != 2 ||
                    decoded.frames < frames || (expectedFrames && decoded.frames != expectedFrames) ||
                    (includeTail && decoded.frames <= lastAudibleFrame)) return false;
                float error = 0;
                for (std::uint64_t i = 0; i < decoded.frames; ++i) for (unsigned channel = 0; channel < 2; ++channel) {
                    const float expected = i + latency < referenceFrames
                        ? output.getChannel(channel)[i + latency] : 0.f;
                    if (!std::isfinite(decoded.interleaved[2 * i + channel])) return false;
                    error = std::max(error, std::abs(decoded.interleaved[2 * i + channel] - expected));
                }
                if (error >= 2e-5f) std::printf("wet tail comparison: max error %.9g in %s\n", error, file.c_str());
                return error < 2e-5f;
            };
            daw::rendering::Spec spec;
            spec.outputDir = directory.string(); spec.range = daw::rendering::Range::Custom;
            spec.customEndSeconds = double(frames) / rate; spec.blockSize = block;
            spec.file.container = audio::platform::Container::Wav;
            spec.file.encoding = audio::platform::Encoding::Float32;
            spec.stemChannelIds = {track};
            for (bool includeTail : {false, true}) {
                spec.baseName = std::string(quality ? "hd-voiced" : "live-voiced") + (includeTail ? "-tail" : "-range");
                spec.tail = includeTail ? daw::rendering::Tail::Fixed : daw::rendering::Tail::None;
                spec.tailSeconds = double(tail) / rate;
                daw::rendering::Report report;
                bool complete = bool(controller.renderProject(spec, {}, report)) && report.files.size() == 2;
                for (const auto& file : report.files)
                    complete &= matchesReference(file, frames + (includeTail ? tail : 0), includeTail);
                check(complete, includeTail ? "wet master and stem exports retain the complete declared tail"
                                            : "fixed-range wet master and stems retain the final syllable after PDC");
            }
            daw::rendering::Report frozen;
            const bool froze = bool(controller.freezeTrack(track, {}, frozen)) && controller.isTrackFrozen(track) &&
                               frozen.files.size() == 1;
            check(froze && matchesReference(frozen.files.front(), 0, true),
                  quality ? "HD freeze preserves the complete corrected final syllable and tail"
                          : "Real-Time freeze preserves the complete corrected final syllable and tail");
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return failures ? 1 : 0;
}
