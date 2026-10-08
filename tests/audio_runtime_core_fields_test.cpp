#include "AudioRuntimeCoreFields.hpp"
#include "AudioRuntime.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

// Deliberately declared after the archive definitions, in the value's own
// namespace: endpoint domains must be able to extend the codec through ADL.
namespace test_values {
struct Snapshot {
    daw::AudioTransportSnapshot transport;
    daw::AudioDeviceSnapshot device;
    daw::AudioTimingSnapshot timing;
    std::array<float, 12> spectrum{};
    std::tuple<std::string, std::optional<std::uint64_t>> selection;
    std::bitset<11> flags;
};
struct WideValue {
    static inline std::size_t constructed = 0;
    std::array<std::uint8_t, 1024> data{};
    WideValue() { ++constructed; }
};
template<class Archive> void fields(Archive& archive, WideValue& value) { archive(value.data); }
template<class Archive> void fields(Archive& archive, Snapshot& value) {
    archive(value.transport, value.device, value.timing, value.spectrum, value.selection, value.flags);
}
}

namespace {
using namespace daw;
int failures = 0;
void check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
}
template<class Action> bool rejected(Action&& action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
void snapshots() {
    test_values::Snapshot source;
    source.transport.state = engine::TransportState::Recording;
    source.transport.position = 1234567890123; source.transport.tempo = 137;
    source.transport.playing = source.transport.recording = true;
    source.device.state = audio::AudioDeviceState::Running;
    source.device.running = source.device.hasStream = true;
    source.device.sampleRate = 96000; source.device.bufferSize = 256;
    source.device.xruns = {1, 2, 3000000000ull, 4};
    source.timing.counters = {13, 2, 876543, 1};
    source.timing.events = {{1234, 5678, 256, 3}, {4321, 5678, 256, 1}};
    source.spectrum[5] = .25f; source.selection = {"source", 7000000000ull};
    source.flags.set(0); source.flags.set(8); source.flags.set(10);
    const auto encoded = audio_value::encode(source, std::uint16_t{42});
    const auto [copy, suffix] = audio_value::decode<test_values::Snapshot, std::uint16_t>(encoded);
    check(copy.transport.position == source.transport.position && copy.transport.tempo == 137 &&
          copy.device.xruns == source.device.xruns && copy.timing.counters.maximumNs == 876543 &&
          copy.timing.events.size() == 2 && copy.timing.events[1].elapsedNs == 4321 &&
          copy.spectrum == source.spectrum && copy.selection == source.selection &&
          copy.flags == source.flags && suffix == 42,
          "owned variadic snapshots preserve fixed arrays, tuples, bitsets and 64-bit counters through ADL");
    bool rejectsTruncation = true;
    for (std::size_t size = 0; size < encoded.size(); ++size)
        rejectsTruncation &= rejected([&] {
            (void)audio_value::decode<test_values::Snapshot, std::uint16_t>(std::span(encoded).first(size));
        });
    check(rejectsTruncation, "every truncated snapshot is rejected before becoming caller-visible");
    auto trailing = encoded; trailing.push_back(0);
    check(rejected([&] { (void)audio_value::decode<test_values::Snapshot, std::uint16_t>(trailing); }),
          "variadic decoding rejects trailing data");

    const std::array<std::int16_t, 2> integers{-2, 0x1234};
    check(audio_value::encode(integers) == std::vector<std::uint8_t>({0xfe, 0xff, 0x34, 0x12}),
          "fixed arrays encode integer fields in little-endian order without ABI padding");
    auto bits = audio_value::encode(source.flags);
    check(bits == std::vector<std::uint8_t>({1, 5}), "bitset bytes have deterministic low-bit-first packing");
    bits.back() |= 0x80;
    check(rejected([&] { (void)audio_value::decode<std::bitset<11>>(bits); }),
          "nonzero unused bitset padding is rejected");
}

void resultAndLoudness() {
    AudioResultValue result(audio::Result::fail(audio::EngineError::FileWriteError, "Disk full"));
    const auto [copy] = audio_value::decode<AudioResultValue>(audio_value::encode(result));
    check(copy.result().error() == audio::EngineError::FileWriteError && copy.result().message() == "Disk full",
          "native Result crosses as an owned default-constructible code/message value");
    const auto [ok] = audio_value::decode<AudioResultValue>(audio_value::encode(AudioResultValue{}));
    check(bool(ok.result()), "successful Result remains successful after decoding");
    const auto invalidCode = audio_value::encode(std::uint32_t{500}, std::string{});
    check(rejected([&] { (void)audio_value::decode<AudioResultValue>(invalidCode); }), "unknown result codes are rejected");

    engine::LoudnessLevels levels;
    levels.shortTerm = -17.5f;
    const auto [loudness] = audio_value::decode<engine::LoudnessLevels>(audio_value::encode(levels));
    check(std::isnan(loudness.momentary) && loudness.shortTerm == -17.5f && std::isnan(loudness.integrated),
          "missing loudness readings use explicit absence while finite LUFS values are exact");
    levels.integrated = -std::numeric_limits<float>::infinity();
    const auto [silence] = audio_value::decode<engine::LoudnessLevels>(audio_value::encode(levels));
    check(std::isnan(silence.momentary) && silence.shortTerm == -17.5f &&
          silence.integrated == -std::numeric_limits<float>::infinity(),
          "measured silence survives IPC independently of unavailable and finite loudness");
    levels.integrated = std::numeric_limits<float>::infinity();
    check(rejected([&] { (void)audio_value::encode(levels); }), "positive infinite loudness remains invalid");
    check(rejected([&] { (void)audio_value::decode<engine::LoudnessLevels>(
        audio_value::encode(std::uint8_t{3}, std::uint8_t{0}, std::uint8_t{0})); }),
        "unknown loudness state is rejected");
    AudioMeterSnapshot meter; meter.left = std::numeric_limits<float>::quiet_NaN();
    check(rejected([&] { (void)audio_value::encode(meter); }), "NaN remains forbidden in ordinary DSP meter fields");
}

void deviceAndCapture() {
    audio::DeviceInfo device;
    device.uid = "ASIO: interface"; device.name = "Interface";
    device.hostApi = "ASIO"; device.inputChannels = 128; device.outputChannels = 64;
    device.isAsio = device.hasControlPanel = true;
    device.inputChannelNames = {"Input 1", "Input 2"};
    device.sampleRates = {44100, 48000, 96000}; device.bufferSizes = {64, 128, 256};
    device.preferredBufferSize = 128;
    audio::AudioDeviceConfig config;
    config.inputDeviceUid = config.outputDeviceUid = device.uid;
    config.inputChannelSelectors = {126, 127}; config.outputChannelSelectors = {0, 1};
    const auto [deviceCopy, configCopy] = audio_value::decode<audio::DeviceInfo, audio::AudioDeviceConfig>(
        audio_value::encode(device, config));
    check(deviceCopy.inputChannels == 128 && deviceCopy.sampleRates == device.sampleRates &&
          deviceCopy.inputChannelNames == device.inputChannelNames && configCopy.inputChannelSelectors == config.inputChannelSelectors,
          "device capabilities and physical channel selectors survive without engine-channel truncation");
    config.inputChannelSelectors = {-1};
    check(rejected([&] { (void)audio_value::encode(config); }), "negative physical channel selectors are rejected");
    const auto badDevice = audio_value::encode(std::string{}, std::string{}, true, 48000.,
        std::uint32_t{0}, std::vector<int>{}, std::vector<int>{});
    check(rejected([&] { (void)audio_value::decode<audio::AudioDeviceConfig>(badDevice); }),
          "malformed device requests reject a zero block size");

    AudioCaptureSpec capture; capture.directory = "recordings"; capture.channelCount = 2; capture.startSample = -512;
    AudioCapturePeaks peaks; peaks.status = {true, true, 1000, -512, 256, 48000};
    peaks.firstBucket = 2; peaks.values = {.1f, .8f};
    audio::RecordingSession closed;
    closed.state = audio::RecordingSession::State::Stopped; closed.filePath = "recordings/take.wav";
    closed.startSample = -512; closed.capturedFrames = closed.recordedSamples = 1000;
    closed.writtenFrames = 900; closed.droppedFrames = 100; closed.inputXruns = 2;
    closed.interrupted = true; closed.fileWriteSucceeded = false;
    const auto [captureCopy, peaksCopy, closedCopy] =
        audio_value::decode<AudioCaptureSpec, AudioCapturePeaks, audio::RecordingSession>(audio_value::encode(capture, peaks, closed));
    check(captureCopy.startSample == -512 && peaksCopy.status.recordedFrames == 1000 && peaksCopy.values == peaks.values &&
          closedCopy.interrupted && !closedCopy.fileWriteSucceeded && closedCopy.writtenFrames == 900 && closedCopy.droppedFrames == 100,
          "capture snapshots preserve negative pre-roll and explicit incomplete WAV durability");
    const auto badCapture = audio_value::encode(std::string{}, std::uint32_t{0}, std::uint32_t{3}, true, std::int64_t{0});
    check(rejected([&] { (void)audio_value::decode<AudioCaptureSpec>(badCapture); }),
          "capture requests reject unsupported channel counts");
    const auto badProfile = audio_value::encode(std::uint64_t{1}, std::uint64_t{2}, std::int64_t{3},
        std::uint32_t{4}, std::uint32_t{5}, std::uint32_t{2});
    check(rejected([&] { (void)audio_value::decode<rt::ProfileEvent>(badProfile); }),
          "profile events reject unknown kinds");
}

void resources() {
    struct Directory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("daw-core-fields-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Directory() { std::filesystem::create_directory(path); }
        ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    } directory;
    ProcessAudioResources store(directory.path);
    auto samples = std::make_shared<engine::SampleBuffer>(2, 64, 48000);
    samples->writableChannel(0)[17] = .75f;
    using Sample = std::shared_ptr<const engine::SampleBuffer>;
    const Sample immutable = samples;
    const auto bytes = audio_value::encodeResources(store, immutable, std::array<Sample, 2>{immutable, immutable});
    ProcessAudioResources::Cache cache;
    const auto [copy, copies] = audio_value::decodeResources<Sample, std::array<Sample, 2>>(bytes, directory.path, &cache);
    const auto [again, repeated] = audio_value::decodeResources<Sample, std::array<Sample, 2>>(bytes, directory.path, &cache);
    check(store.records().size() == 1 && copy && copy == copies[0] && copy == copies[1] && copy == again &&
          copy == repeated[0] && copy != immutable && copy->readSample(0, 17) == .75f,
          "generic resource messages share the existing immutable PCM registry and mapping cache");
    const auto cacheSize = cache.samples.size();
    check(rejected([&] {
        (void)audio_value::decodeResources<Sample, std::array<Sample, 2>>(std::span(bytes).first(bytes.size() - 1), directory.path, &cache);
    }) && cache.samples.size() == cacheSize && cache.samples.begin()->second == copy,
          "an incomplete resource message cannot replace the acknowledged mapping cache");
}
void allocationBudget() {
    // Count and remaining bytes satisfy the collection guard. Native storage
    // would exceed 2 GiB if the decoder resized before checking its budget.
    auto forged = audio_value::encode(std::uint32_t(audio_value::maxElements));
    forged.resize(forged.size() + audio_value::maxElements, 0);
    test_values::WideValue::constructed = 0;
    bool allocationFailure = false;
    try { (void)audio_value::decode<std::vector<test_values::WideValue>>(forged); }
    catch (const std::invalid_argument& error) {
        allocationFailure = std::string_view(error.what()).find("allocation limit") != std::string_view::npos;
    }
    check(allocationFailure && test_values::WideValue::constructed == 0,
          "a small forged collection is rejected before allocating or constructing its huge native elements");

    const auto allocationRejected = [](auto&& read) {
        try { read(); return false; }
        catch (const std::invalid_argument& error) {
            return std::string_view(error.what()).find("allocation limit") != std::string_view::npos;
        }
    };
    audio_value::Reader overflow(std::span<const std::uint8_t>{});
    check(allocationRejected([&] { overflow.chargeAllocation(std::numeric_limits<std::size_t>::max(), 2); }) &&
          overflow.allocatedBytes == 0,
          "native allocation multiplication is checked before size_t overflow");

    // Seed already-consumed budget instead of physically allocating hundreds
    // of megabytes just to exercise limits at a later field in one message.
    const auto blobs = audio_value::encode(std::vector<std::uint8_t>{1, 2, 3, 4},
        std::vector<std::uint8_t>{5, 6, 7, 8});
    audio_value::Reader combined(blobs);
    combined.chargeAllocation(audio_value::maxDecodedBytes - 6, 1);
    std::vector<std::uint8_t> first, second;
    combined(first);
    check(first.size() == 4 && allocationRejected([&] { combined(second); }) && second.empty(),
          "separate byte buffers consume one cumulative ownership budget");

    const auto nested = audio_value::encode(std::vector<std::string>{"payload"});
    audio_value::Reader strings(nested);
    strings.chargeAllocation(audio_value::maxDecodedBytes - sizeof(std::string) - 4, 1);
    std::vector<std::string> text;
    check(allocationRejected([&] { strings(text); }),
          "nested strings charge their storage in addition to the owning vector elements");

    const auto mapping = audio_value::encode(std::unordered_map<std::string, std::uint8_t>{{"key", 1}});
    audio_value::Reader maps(mapping);
    maps.chargeAllocation(audio_value::maxDecodedBytes, 1);
    std::unordered_map<std::string, std::uint8_t> values;
    check(allocationRejected([&] { maps(values); }) && values.empty(),
          "map node and bucket ownership is charged before inserting untrusted entries");

    const auto pointer = audio_value::encode(std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{7}));
    audio_value::Reader pointers(pointer);
    pointers.chargeAllocation(audio_value::maxDecodedBytes - sizeof(std::vector<std::uint8_t>), 1);
    std::shared_ptr<const std::vector<std::uint8_t>> value;
    check(allocationRejected([&] { pointers(value); }) && !value,
          "immutable shared values charge their object and ownership bookkeeping before construction");

    using Owned = std::tuple<std::vector<std::string>, std::unordered_map<std::string, std::vector<std::uint8_t>>,
        std::shared_ptr<const std::vector<std::uint8_t>>>;
    Owned original{{"one", "two"}, {{"blob", {1, 2, 3}}}, std::make_shared<const std::vector<std::uint8_t>>(4, 9)};
    const auto [copy] = audio_value::decode<Owned>(audio_value::encode(original));
    check(std::get<0>(copy) == std::get<0>(original) && std::get<1>(copy) == std::get<1>(original) &&
          std::get<2>(copy) != std::get<2>(original) && *std::get<2>(copy) == *std::get<2>(original),
          "normal nested payloads still decode into independent owned values within the shared budget");
}

void nativeValidation() {
    AudioRuntime runtime;
    AudioSessionSpec session;
    AudioGraphSpec::Channel channel; channel.id = "source"; channel.name = "Source"; channel.acceptsMidi = true;
    session.graph.channels.push_back(channel);
    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>(1);
    auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>(1);
    auto samples = std::make_shared<engine::SampleBuffer>(2, 4096, 48000);
    clips->front().audio = samples; clips->front().lengthSamples = samples->frames();
    AudioContentSpec content;
    content.clips = AudioContentSpec::Clips{clips, {}};
    content.midi = AudioContentSpec::Midi{notes, {}, false};
    session.channels.push_back({"source", content});
    const bool prepared = bool(runtime.prepare(48000, 64)) && bool(runtime.applySession(session));
    check(prepared, "native command validation fixture publishes a minimal playback session");
    if (!prepared) return;
    std::shared_ptr<engine::ClipPlayerNode> clipNode;
    std::shared_ptr<engine::MidiClipPlayerNode> midiNode;
    for (const auto& entry : runtime.routingGraph()->nodes) {
        if (entry.node->name() == "Source Clips") clipNode = std::dynamic_pointer_cast<engine::ClipPlayerNode>(entry.owner);
        if (entry.node->name() == "Source Notes") midiNode = std::dynamic_pointer_cast<engine::MidiClipPlayerNode>(entry.owner);
    }
    check(clipNode && midiNode, "validation fixture retains both playback schedules");
    if (!clipNode || !midiNode) return;
    const auto oldClips = clipNode->controlState();
    const auto oldMidi = midiNode->controlState();
    auto badNotes = std::make_shared<engine::MidiClipPlayerNode::NoteList>(*notes);
    badNotes->front().key = 255;
    auto invalid = content;
    invalid.clips->shared = std::make_shared<const engine::ClipPlayerNode::ClipList>();
    invalid.midi->notes = badNotes;
    invalid.midi->timelineSuppressed = true;
    check(rejected([&] { runtime.applyContent("source", invalid); }) &&
          clipNode->controlState() == oldClips && midiNode->controlState().notes == oldMidi.notes &&
          midiNode->controlState().controllers == oldMidi.controllers &&
          midiNode->controlState().timelineSuppressed == oldMidi.timelineSuppressed,
          "invalid partial content is rejected before replacing any clip or MIDI schedule");
    badNotes->front().key = 60;
    engine::curve::Point point; point.shape = static_cast<engine::curve::Shape>(255);
    badNotes->front().pitch.push_back(point);
    check(rejected([&] { validateAudioContent(invalid); }), "partial content rejects unknown note curve shapes");
    auto badClips = std::make_shared<engine::ClipPlayerNode::ClipList>(*clips);
    auto warp = std::make_shared<ClipWarpModel>(); warp->enabled = true;
    badClips->front().warp = warp;
    invalid = content; invalid.clips->shared = badClips;
    check(rejected([&] { validateAudioContent(invalid); }), "partial content rejects an enabled warp without anchors");

    check(rejected([&] { runtime.requestCountIn(-1); }), "negative count-in is rejected before publication");
    AudioTransportCommand tempo; tempo.action = AudioTransportCommand::Action::Tempo; tempo.value = 1e-300;
    runtime.transportCommand(tempo);
    check(rejected([&] { runtime.requestCountIn(1); }), "count-in rejects an overflowing absolute deadline");
    tempo.value = 120; runtime.transportCommand(tempo);
    runtime.requestCountIn(0);

    check(!runtime.startPreview(samples, false, std::numeric_limits<double>::quiet_NaN()),
          "preview rejects nonfinite pitch before starting playback");
    check(rejected([&] {
        runtime.previewCommand({AudioPreviewCommand::Action::SeekSeconds, std::numeric_limits<double>::quiet_NaN()});
    }), "preview rejects nonfinite commands");
    const bool started = runtime.startPreview(samples, true, 0);
    audio::AudioBuffer output(2, 64);
    audio::AudioCallbackContext callback;
    callback.outputBuffer = &output; callback.numFrames = 64; callback.sampleRate = 48000;
    runtime.processDeviceBlock(callback);
    check(started && runtime.previewSnapshot().playing && rejected([&] {
        runtime.previewCommand({AudioPreviewCommand::Action::SeekSeconds, std::numeric_limits<double>::max()});
    }), "preview rejects overflowing frame positions after its source is active");
    runtime.previewCommand({AudioPreviewCommand::Action::Stop});

    AudioCaptureSpec capture; capture.inputChannel = engine::kMaxChannels;
    AudioCaptureStarted out; out.id = 99;
    const auto result = runtime.startCapture(capture, out);
    check(result.error() == audio::EngineError::InvalidArgument && out.id == 0 && out.path.empty() && !runtime.hasActiveCaptures(),
          "invalid capture routing is rejected before opening a recording file or allocating a capture ID");
}
}

int main() {
    try { snapshots(); resultAndLoudness(); deviceAndCapture(); resources(); allocationBudget(); nativeValidation(); }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL exception: %s\n", error.what()); return 1; }
    return failures ? 1 : 0;
}
