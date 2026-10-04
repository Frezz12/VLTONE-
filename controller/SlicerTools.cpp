#include "SlicerTools.hpp"

#include "model/Document.hpp"
#include "recovery/CloudRecordingRecovery.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>

namespace daw::slicer {
namespace {
namespace p = plugins::slicer;
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t kMaxPresetBytes = 256u * 1024u * 1024u;
bool selected(std::span<const std::uint32_t> ids, std::uint32_t id) {
    return ids.empty() || std::find(ids.begin(), ids.end(), id) != ids.end();
}
bool running(const std::function<bool()>& keepGoing) { return !keepGoing || keepGoing(); }
double unit(std::mt19937_64& rng) { return double(rng() >> 11) / 9007199254740992.0; }
template<class T> void shuffle(std::vector<T>& values, std::mt19937_64& rng) {
    // Explicit Fisher-Yates, including rejection, keeps seeds portable across STL implementations.
    for (std::size_t n = values.size(); n > 1; --n) {
        const auto bound = std::uint64_t(n);
        const auto threshold = (std::uint64_t(0) - bound) % bound;
        std::uint64_t draw;
        do { draw = rng(); } while (draw < threshold);
        std::swap(values[n - 1], values[std::size_t(draw % bound)]);
    }
}
bool readBytes(const std::string& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    const auto native = platform::pathFromUtf8(path);
    const auto size = fs::file_size(native, ec);
    if (ec || size == 0 || size > kMaxPresetBytes) { error = "File is missing, empty or larger than 256 MB."; return false; }
    std::ifstream file(native, std::ios::binary);
    bytes.resize(std::size_t(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
        error = "Could not read the complete file."; return false;
    }
    return true;
}
bool durable(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    return recovery::writeDurableRecoveryFile(path,
        {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}
}

void randomize(p::SliceTable& table, std::span<const std::uint32_t> ids, const RandomSettings& s) {
    std::mt19937_64 rng(s.seed);
    std::vector<int> indices;
    for (std::uint32_t i = 0; i < table.count; ++i) {
        auto& slice = table.slices[i];
        if (slice.locked || !selected(ids, slice.id)) continue;
        indices.push_back(int(i));
        if (s.pitch) slice.transpose = std::int16_t(std::round((unit(rng) * 2.0 - 1.0) * std::clamp(s.pitchRange, 0.0, 48.0)));
        if (s.gain) slice.gain = float(std::pow(10.0, (unit(rng) * 2.0 - 1.0) * std::clamp(s.gainRangeDb, 0.0, 24.0) / 20.0));
        if (s.pan) slice.pan = float((unit(rng) * 2.0 - 1.0) * std::clamp(s.panRange, 0.0, 1.0));
        if (s.reverse) slice.flags = std::uint8_t((slice.flags & ~p::kSliceReverse) | (unit(rng) >= 0.5 ? p::kSliceReverse : 0));
        if (s.filter) {
            slice.filter = 1;
            slice.effect = 0;
            const auto lo = std::clamp(s.cutoffMin, 0.0, 1.0), hi = std::clamp(s.cutoffMax, lo, 1.0);
            slice.cutoff = float(lo + unit(rng) * (hi - lo));
        }
    }
    if (s.keys) {
        std::vector<std::int16_t> keys;
        for (int i : indices) keys.push_back(table.slices[i].key);
        shuffle(keys, rng);
        for (std::size_t i = 0; i < indices.size(); ++i) table.slices[indices[i]].key = keys[i];
    }
    table.rebuild();
}

bool normalize(const engine::SampleBuffer& audio, p::SliceTable& table,
               std::span<const std::uint32_t> ids, const std::function<bool()>& keepGoing) {
    auto staged = table;
    for (std::uint32_t i = 0; i < staged.count; ++i) {
        auto& slice = staged.slices[i];
        if (!selected(ids, slice.id)) continue;
        float peak = 0.0f;
        for (engine::ChannelCount ch = 0; ch < audio.channels(); ++ch) {
            const float* data = audio.channel(ch);
            for (auto f = slice.start; f < std::min(slice.end, audio.frames()); ++f) {
                if ((f & 4095u) == 0 && !running(keepGoing)) return false;
                if (std::isfinite(data[f])) peak = std::max(peak, std::abs(data[f]));
            }
        }
        if (peak > 1e-12f) slice.normalization = float(std::pow(10.0, -1.0 / 20.0) / peak);
    }
    if (!running(keepGoing)) return false;
    table = std::move(staged);
    return true;
}

midifile::File midiPhrase(const p::ControlState& state, PhraseOrder order, std::uint64_t seed) {
    midifile::File result;
    result.format = 0; result.trackCount = 1; result.ticksPerQuarter = 960;
    result.firstTempoBpm = std::clamp(state.analysis.sourceBpm, 20.0, 999.0);
    result.trackNames = {"Slicer"};
    if (!state.table || !state.audio || state.audio->sampleRate() <= 0.0) return result;
    std::vector<std::uint32_t> indices(state.table->count);
    std::iota(indices.begin(), indices.end(), 0u);
    if (order == PhraseOrder::Reverse) std::reverse(indices.begin(), indices.end());
    if (order == PhraseOrder::Shuffle) { std::mt19937_64 rng(seed); shuffle(indices, rng); }
    for (auto i : indices) {
        const auto& s = state.table->slices[i];
        const double beats = double(s.end - s.start) / state.audio->sampleRate() * result.firstTempoBpm / 60.0;
        if (!(s.flags & p::kSliceMuted)) result.notes.push_back({s.key, result.lengthBeats, beats, 127});
        result.lengthBeats += beats;
    }
    return result;
}

bool savePreset(const std::string& path, const p::ControlState& state, std::string& error,
                const std::function<bool()>& keepGoing) {
    try {
        if (!state.audio || state.path.empty()) { error = "Load a sample before saving a preset."; return false; }
        std::vector<std::uint8_t> sampleBytes, stateBytes;
        if (!readBytes(state.path, sampleBytes, error) || !running(keepGoing)) return false;
        // Never replace the source with a preset, even if a caller bypasses the file dialog.
        std::error_code ec;
        if (fs::equivalent(platform::pathFromUtf8(path), platform::pathFromUtf8(state.path), ec)) {
            error = "The preset destination is the source sample."; return false;
        }
        const auto name = platform::pathToUtf8(platform::pathFromUtf8(state.path).filename());
        p::SlicerInstance staged; staged.restoreState(state);
        if (!staged.saveProjectState(stateBytes, name)) { error = "Could not encode the instrument state."; return false; }
        auto bytes = Json::to_cbor(Json{{"format", "vlt-slicer"}, {"version", 1},
            {"name", name}, {"state", Json::binary(stateBytes)}, {"sample", Json::binary(sampleBytes)}});
        if (bytes.size() > kMaxPresetBytes || !running(keepGoing)) { error = "Preset is too large or cancelled."; return false; }
        if (!durable(path, bytes)) { error = "Could not save the preset."; return false; }
        return true;
    } catch (...) { error = "Could not encode the preset."; return false; }
}

bool loadPreset(const std::string& path, const std::string& mediaCache, p::ControlState& out,
                std::string& error, const std::function<bool()>& keepGoing) {
    fs::path stagedPath;
    const auto cleanup = [&] { if (!stagedPath.empty()) { std::error_code ec; fs::remove(stagedPath, ec); } };
    try {
        std::vector<std::uint8_t> bytes;
        if (!readBytes(path, bytes, error) || !running(keepGoing)) return false;
        const auto json = Json::from_cbor(bytes);
        if (!json.is_object() || json.value("format", "") != "vlt-slicer" || json.value("version", 0) != 1
            || !json.contains("sample") || !json["sample"].is_binary() || !json.contains("state") || !json["state"].is_binary()) {
            error = "Invalid Slicer preset."; return false;
        }
        auto extension = platform::pathToUtf8(platform::pathFromUtf8(json.value("name", "sample.wav")).extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (extension.empty() || !audio::platform::isDecodableExtension(extension.substr(1))) {
            error = "Unsupported embedded sample format."; return false;
        }
        const auto dir = platform::pathFromUtf8(mediaCache);
        fs::create_directories(dir);
        stagedPath = dir / platform::pathFromUtf8(newUuid() + extension);
        const auto samplePath = platform::pathToUtf8(stagedPath);
        if (!durable(samplePath, json["sample"].get_binary())) { error = "Could not create cached sample."; cleanup(); return false; }
        audio::platform::DecodedAudio decoded;
        audio::platform::DecodeOptions opts; opts.keepGoing = keepGoing;
        auto decodedResult = audio::platform::decodeAudioFile(samplePath, decoded, opts);
        if (!decodedResult || !running(keepGoing) || decoded.frames == 0 || decoded.frames > UINT32_MAX || decoded.channels == 0) {
            error = "Could not decode embedded sample."; cleanup(); return false;
        }
        auto audio = engine::SampleBuffer::fromInterleaved(decoded.interleaved, decoded.channels,
            engine::FrameCount(decoded.frames), decoded.sampleRate);
        p::SlicerInstance staged; staged.adoptSample(samplePath, audio);
        // Use the validated cache path, ignoring all paths embedded in untrusted state.
        auto stateJson = Json::parse(json["state"].get_binary());
        stateJson["sample"] = samplePath;
        const auto stateText = stateJson.dump();
        if (!staged.loadState({reinterpret_cast<const std::uint8_t*>(stateText.data()), stateText.size()}) || !running(keepGoing)) {
            error = "Invalid instrument state."; cleanup(); return false;
        }
        out = staged.captureState();
        return true;
    } catch (...) { error = "Damaged Slicer preset."; cleanup(); return false; }
}

bool renderWav(const std::string& path, const p::ControlState& state, std::uint32_t sliceId,
               bool processed, std::string& error, const std::function<bool()>& keepGoing) {
    if (!state.audio || !state.table) { error = "No sliced sample."; return false; }
    const int index = state.table->indexForId(sliceId);
    if (index < 0) { error = "Slice no longer exists."; return false; }
    const auto& slice = state.table->slices[index];
    if (slice.end <= slice.start || slice.end > state.audio->frames()) { error = "Invalid slice range."; return false; }
    std::error_code ec;
    if (fs::exists(platform::pathFromUtf8(path), ec)) { error = "WAV cache destination already exists."; return false; }
    if (!state.path.empty() && fs::equivalent(platform::pathFromUtf8(path), platform::pathFromUtf8(state.path), ec)) {
        error = "The export destination is the source sample."; return false;
    }
    const double rate = state.audio->sampleRate();
    const auto channels = processed ? 2u : unsigned(state.audio->channels());
    constexpr std::uint32_t blockSize = 512;
    audio::platform::AudioFileWriter writer;
    const auto abort = [&] { writer.close(); std::error_code ignored; fs::remove(platform::pathFromUtf8(path), ignored); return false; };
    const double tune = slice.transpose + slice.fineTune / 100.0 + state.parameters[p::indexOf(p::Param::Transpose)]
        + state.parameters[p::indexOf(p::Param::FineTune)] / 100.0
        + state.parameters[p::indexOf(p::Param::KeyTrack)] * (slice.key - state.parameters[p::indexOf(p::Param::RootNote)]);
    const auto span = std::uint64_t(slice.end - slice.start);
    const double gate = std::clamp(state.parameters[p::indexOf(p::Param::Gate)], 0.0, 1.0);
    const auto played = std::uint64_t(std::max(1.0, std::ceil(double(span) * gate / std::pow(2.0, tune / 12.0))));
    const double release = slice.useGlobalEnvelope ? state.parameters[p::indexOf(p::Param::Release)] : slice.release;
    const bool looping = slice.loopMode > 0 || (slice.flags & p::kSliceLoop);
    const auto total = processed ? played + (looping ? std::uint64_t(std::max(0.003, release) * rate) : 0) : span;
    if (total > UINT32_MAX || !writer.open(path, rate, audio::ChannelCount(channels), total)) {
        error = "Could not create WAV."; return false;
    }
    std::vector<std::vector<float>> planes(channels, std::vector<float>(blockSize));
    std::vector<float*> outputs; std::vector<const float*> pointers;
    for (auto& plane : planes) { outputs.push_back(plane.data()); pointers.push_back(plane.data()); }
    p::SlicerInstance instrument;
    if (processed) {
        auto renderState = state;
        auto table = std::make_shared<p::SliceTable>(); table->frames = state.audio->frames(); table->count = 1;
        table->slices[0] = slice;
        // A held loop makes one traversal and then releases; it cannot leave an infinite export.
        renderState.table = table; table->rebuild(); instrument.restoreState(renderState);
        plugins::PluginProcessInfo info; info.sampleRate = rate; info.maxBlockSize = blockSize; info.offline = true;
        if (!instrument.activate(info)) { error = "Could not activate Slicer render."; return abort(); }
        instrument.startProcessing();
    }
    for (std::uint64_t at = 0; at < total; at += blockSize) {
        if (!running(keepGoing)) { error = "Export cancelled."; return abort(); }
        const auto count = std::uint32_t(std::min<std::uint64_t>(blockSize, total - at));
        if (!processed) {
            for (unsigned ch = 0; ch < channels; ++ch)
                std::copy_n(state.audio->channel(engine::ChannelCount(ch)) + slice.start + at, count, outputs[ch]);
        } else {
            std::array<plugins::PluginEvent, 2> events{}; std::size_t n = 0;
            if (at == 0) { auto& e = events[n++]; e.kind = plugins::PluginEvent::Kind::NoteOn; e.key = slice.key; e.value = 1.0; }
            if (played >= at && played < at + count) {
                auto& e = events[n++]; e.kind = plugins::PluginEvent::Kind::NoteOff; e.key = slice.key; e.frameOffset = std::uint32_t(played - at);
            }
            plugins::PluginProcessContext ctx; ctx.outputs = outputs.data(); ctx.outputChannels = std::uint16_t(channels);
            ctx.frames = count; ctx.offline = true; ctx.inputEvents = {events.data(), n};
            instrument.process(ctx);
        }
        if (!writer.write(pointers.data(), count)) { error = "Could not write WAV."; return abort(); }
    }
    if (!writer.close()) { error = "Could not finish WAV."; return abort(); }
    return true;
}

} // namespace daw::slicer
