#pragma once

#include "AudioSessionCodec.hpp"
#include "Host/PluginTypes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <variant>

#pragma push_macro("slots")
#undef slots

namespace daw::audio_value {

inline constexpr std::size_t maxBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t maxDecodedBytes = 512u * 1024u * 1024u;
inline constexpr std::size_t maxElements = 2u * 1024u * 1024u;
inline constexpr std::size_t maxString = 16u * 1024u * 1024u;
[[noreturn]] inline void invalid(const char* message) { throw std::invalid_argument(message); }

/// Define fields(Archive&, Value&) beside the value type or in audio_value.
/// The writer only visits field references; it never mutates their values.
/// Variable containers consume one shared budget for the complete message.
template<class Archive> void fields(Archive& archive, ProcessAudioResources::Record& value) {
    archive(value.id, value.fileName, value.channels, value.frames, value.sampleRate);
}

struct Writer;
struct Reader;
void fields(Writer&, plugins::mini::MiniModuleDefinition&);
void fields(Reader&, plugins::mini::MiniModuleDefinition&);
// These field lists only visit references. The writer never mutates them.
#define FIELDS(Type, ...) template<class A> void fields(A& a, Type& v) { a(__VA_ARGS__); }
FIELDS(plugins::HostingConfiguration, v.mode, v.executable)
FIELDS(plugins::PluginDescriptor, v.format, v.uid, v.path, v.name, v.vendor, v.version,
    v.category, v.isInstrument, v.hasEditor, v.wantsMidi, v.mainInputChannels,
    v.mainOutputChannels, v.fileSize, v.fileModifiedTime, v.stateSchemaVersion,
    v.parameterSchema, v.parameterFingerprint, v.producesMidi)
FIELDS(InsertParameter, v.id, v.value, v.restoreAfterState)
FIELDS(AudioPluginAddress, v.channelId, v.slotId, v.right, v.instance)
FIELDS(AudioPluginStateRestore, v.state, v.stateFile, v.contentDirectory,
    v.sourcePath, v.source, v.tolerateErrors, v.applyAllParameters, v.clearPending)
FIELDS(AudioPluginStateEdit, v.address, v.state, v.parameters, v.replaceExisting)
FIELDS(AudioPluginSpec, v.id, v.uid, v.name, v.descriptor, v.requiredFormat,
    v.requireExactVersion, v.requiredVersion, v.requiredParameterFingerprint,
    v.parameters, v.rightParameters, v.miniModule, v.miniModuleMode, v.profileSeed,
    v.channelMode, v.preferredChannels, v.slideDelivery, v.slideBendRange,
    v.slideReleaseReserve, v.mix, v.bypassed, v.loadPolicy, v.unavailableReason)
FIELDS(AudioPluginChainSpec, v.channelId, v.kind, v.clipId, v.slots)
FIELDS(SendModel, v.id, v.destinationTrackId, v.level, v.preFader, v.enabled)
FIELDS(PlaybackInjection, v.stage, v.anchorChannelId)
FIELDS(AudioGraphSpec::MiniModuleRoute, v.slotId, v.postFx)
FIELDS(AudioGraphSpec::SidechainRoute, v.slotId, v.sourceChannelIds)
FIELDS(AudioGraphSpec::ClipFx, v.id, v.name, v.gain, v.pan, v.playbackInjection)
FIELDS(AudioGraphSpec::Input, v.present, v.enabled, v.channel, v.channelCount, v.monitorMask)
FIELDS(AudioGraphSpec::Channel, v.id, v.name, v.outputBusId, v.acceptsMidi, v.capturing,
    v.input, v.volume, v.pan, v.silent, v.mono, v.samplerOwned, v.samplerVolume,
    v.samplerPan, v.sends, v.miniModules, v.sidechains, v.clipFx, v.frozenAudio, v.frozenFrames)
FIELDS(AudioGraphSpec, v.channels, v.masterVolume, v.masterPan, v.masterMiniModules,
    v.masterSidechains, v.metronomeEnabled, v.auditionCapture)
FIELDS(WarpMarker, v.id, v.sourceSeconds, v.targetBeats, v.locked)
FIELDS(ClipWarpModel, v.enabled, v.preservePitch, v.mode, v.baselineDurationSeconds, v.sensitivity, v.markers)
FIELDS(engine::ClipPlacement, v.audio, v.startSample, v.offsetSamples, v.lengthSamples,
    v.fadeInSamples, v.fadeOutSamples, v.fadeEqualPower, v.fadeInCurve, v.fadeOutCurve,
    v.tapeStartSamples, v.tapeStopSamples, v.sourceStartFrame, v.sourceEndFrame,
    v.stretchMode, v.stretchTime, v.stretchPitch, v.formant, v.loopMode, v.loopStart,
    v.loopEnd, v.gain, v.pan, v.muted, v.clipId, v.warp, v.warpTempo)
FIELDS(engine::curve::Point, v.beats, v.value, v.shape, v.curve, v.phaseFrom, v.phaseTo, v.priority)
FIELDS(engine::MidiNote, v.startBeats, v.lengthBeats, v.key, v.velocity, v.channel,
    v.pan, v.releaseVelocity, v.startOrder, v.endOrder, v.noteId, v.pitch)
FIELDS(engine::MidiClipPlayerNode::ControlPoint, v.beats, v.value, v.shape, v.curve, v.order)
FIELDS(engine::MidiClipPlayerNode::ControlCurve, v.startBeats, v.endBeats,
    v.defaultValue, v.cc, v.channel, v.key, v.points)
FIELDS(engine::LevelCurve, v.points, v.defaultValue, v.active)
FIELDS(engine::LevelAutomation, v.gain, v.pan, v.mute)
FIELDS(AudioContentSpec::Clips, v.shared, v.individual)
FIELDS(AudioContentSpec::Midi, v.notes, v.controllers, v.timelineSuppressed)
FIELDS(AudioContentSpec::PluginCurve, v.slotId, v.parameterId, v.defaultValue, v.points)
FIELDS(AudioContentSpec::Levels, v.fader, v.sends)
FIELDS(AudioContentSpec, v.clips, v.midi, v.plugins, v.levels)
FIELDS(AudioSessionSpec::Channel, v.id, v.content)
FIELDS(AudioSessionSpec, v.hosting, v.pluginChains, v.graph, v.channels)
FIELDS(AudioPluginCheckpoint::Side, v.hasState, v.state, v.parameters, v.pending, v.projectState)
FIELDS(AudioPluginCheckpoint, v.channelId, v.slotId, v.uid, v.format, v.left, v.right)
FIELDS(AudioTransportCommand, v.action, v.position, v.end, v.value, v.numerator,
    v.denominator, v.enabled, v.prepare)
FIELDS(AudioPreviewCommand, v.action, v.value)
FIELDS(AudioFaderChange, v.gain, v.pan, v.silent, v.mono)
FIELDS(AudioFaderCommand, v.channelId, v.clipId, v.target, v.change)
FIELDS(AudioInputCommand, v.channelId, v.input)
FIELDS(AudioSendCommand, v.channelId, v.sendId, v.level, v.enabled)
FIELDS(engine::PitchRamp, v.from, v.to, v.phaseFrom, v.phaseTo, v.tension,
    v.segmentStart, v.shapeFrom, v.shapeTo, v.priority, v.frames, v.shape, v.active, v.edited)
FIELDS(engine::MidiEvent, v.frameOffset, v.status, v.data1, v.data2, v.sortOrder,
    v.notePan, v.musicalOrder, v.noteId, v.isPitchExpression, v.pitch, v.isNoteChoke)
FIELDS(AudioMidiCommand, v.channelId, v.event)
FIELDS(AudioControlPacket, v.generation, v.requestId, v.command)
FIELDS(AudioSessionPacket, v.generation, v.revision, v.sampleRate, v.blockSize,
    v.offline, v.session, v.checkpoints, v.transport, v.restores)
#undef FIELDS

struct Writer {
    static constexpr bool reading = false;
    explicit Writer(ProcessAudioResources* resourceStore = nullptr) : resources(resourceStore) {}
    std::vector<std::uint8_t> bytes;
    ProcessAudioResources* resources = nullptr;
    std::size_t elements = 0;
    std::unordered_set<std::string> usedResources;
    void count(std::size_t n) {
        if (n > maxElements - elements) invalid("audio session collection exceeds limit");
        elements += n; one(std::uint32_t(n));
    }
    void append(std::span<const std::uint8_t> data) {
        if (data.size() > maxBytes - bytes.size()) invalid("audio session exceeds byte limit");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    template<class... T> void operator()(const T&... value) { (one(value), ...); }
    template<class T> void one(const T& value) {
        if constexpr (std::is_same_v<T, bool>) one(std::uint8_t(value));
        else if constexpr (std::is_enum_v<T>) one(std::uint32_t(value));
        else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            const U bits = std::bit_cast<U>(value);
            std::uint8_t data[sizeof(T)];
            for (unsigned i = 0; i < sizeof(T); ++i) data[i] = std::uint8_t(bits >> (8 * i));
            append(data);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) invalid("nonfinite audio session number");
            using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            one(std::bit_cast<U>(value));
        } else fields(*this, const_cast<T&>(value));
    }
    void one(const std::string& value) {
        if (value.size() > maxString) invalid("audio session string exceeds limit");
        one(std::uint32_t(value.size()));
        append({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    void one(const std::vector<std::uint8_t>& value) {
        if (value.size() > plugins::kMaxPluginStateBytes) invalid("plugin checkpoint exceeds limit");
        one(std::uint32_t(value.size())); append(value);
    }
    template<class T> void one(const std::vector<T>& value) {
        count(value.size());
        for (const auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(const std::array<T, N>& value) {
        for (const auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(const T (&value)[N]) {
        for (const auto& item : value) one(item);
    }
    template<class... T> void one(const std::tuple<T...>& value) {
        std::apply([&](const auto&... item) { (*this)(item...); }, value);
    }
    template<std::size_t N> void one(const std::bitset<N>& value) {
        for (std::size_t first = 0; first < N; first += 8) {
            std::uint8_t byte = 0;
            for (std::size_t bit = 0; bit < std::min(std::size_t{8}, N - first); ++bit)
                if (value[first + bit]) byte |= std::uint8_t(1u << bit);
            one(byte);
        }
    }
    template<class T, class U> void one(const std::pair<T, U>& value) { (*this)(value.first, value.second); }
    template<class T> void one(const std::optional<T>& value) { one(bool(value)); if (value) one(*value); }
    template<class T> void one(const std::shared_ptr<const T>& value) { one(bool(value)); if (value) one(*value); }
    void one(const std::shared_ptr<const engine::SampleBuffer>& value) {
        if (!value) { one(std::string{}); return; }
        if (!resources) invalid("audio resources are unavailable");
        auto id = resources->put(value);
        if (!id.empty()) usedResources.insert(id);
        one(id);
    }
    template<class T> void one(const std::unordered_map<std::string, T>& value) {
        count(value.size());
        std::vector<const typename std::unordered_map<std::string, T>::value_type*> ordered;
        for (const auto& item : value) ordered.push_back(&item);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
        for (const auto* item : ordered) (*this)(item->first, item->second);
    }
    template<class... T> void one(const std::variant<T...>& value) {
        one(std::uint32_t(value.index())); std::visit([&](const auto& item) { one(item); }, value);
    }
};

struct Reader {
    static constexpr bool reading = true;
    explicit Reader(std::span<const std::uint8_t> data, const ProcessAudioResources::Samples* samples = nullptr)
        : bytes(data), resources(samples) {
        if (bytes.size() > maxBytes) invalid("audio session exceeds byte limit");
    }
    std::span<const std::uint8_t> bytes;
    const ProcessAudioResources::Samples* resources = nullptr;
    std::size_t cursor = 0, elements = 0, allocatedBytes = 0;
    // Encoded length cannot bound native storage: a vector of rich structs
    // may occupy far more bytes than its count and truncated wire fields.
    // Charge all dynamic owners cumulatively before allocating. PCM mappings
    // have their own validated resource limits and do not enter this budget.
    void chargeAllocation(std::size_t count, std::size_t elementBytes) {
        if (elementBytes && count > (maxDecodedBytes - allocatedBytes) / elementBytes)
            invalid("decoded audio values exceed allocation limit");
        allocatedBytes += count * elementBytes;
    }
    std::span<const std::uint8_t> take(std::size_t count) {
        if (count > bytes.size() - cursor) invalid("truncated audio session");
        const auto data = bytes.subspan(cursor, count); cursor += count; return data;
    }
    std::uint32_t count() {
        std::uint32_t n; one(n);
        if (n > maxElements - elements || n > bytes.size() - cursor) invalid("audio session collection exceeds limit");
        elements += n; return n;
    }
    template<class... T> void operator()(T&... value) { (one(value), ...); }
    template<class T> void one(T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t byte; one(byte); if (byte > 1) invalid("invalid audio session boolean"); value = byte != 0;
        } else if constexpr (std::is_enum_v<T>) {
            std::uint32_t number; one(number);
            if (number > std::uint64_t(std::numeric_limits<std::underlying_type_t<T>>::max()))
                invalid("audio enum representation overflow");
            value = T(number);
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U bits = 0; const auto data = take(sizeof(T));
            for (unsigned i = 0; i < sizeof(T); ++i) bits |= U(data[i]) << (8 * i);
            value = std::bit_cast<T>(bits);
        } else if constexpr (std::is_floating_point_v<T>) {
            using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
            U bits; one(bits); value = std::bit_cast<T>(bits);
            if (!std::isfinite(value)) invalid("nonfinite audio session number");
        } else fields(*this, value);
    }
    void one(std::string& value) {
        std::uint32_t n; one(n); if (n > maxString) invalid("audio session string exceeds limit");
        const auto data = take(n);
        if (n) chargeAllocation(std::size_t(n) + 1, sizeof(char));
        value.assign(reinterpret_cast<const char*>(data.data()), data.size());
    }
    void one(std::vector<std::uint8_t>& value) {
        std::uint32_t n; one(n); if (n > plugins::kMaxPluginStateBytes) invalid("plugin checkpoint exceeds limit");
        const auto data = take(n); chargeAllocation(n, sizeof(std::uint8_t));
        value.assign(data.begin(), data.end());
    }
    template<class T> void one(std::vector<T>& value) {
        const auto n = count(); chargeAllocation(n, sizeof(T));
        value.resize(n); for (auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(std::array<T, N>& value) {
        for (auto& item : value) one(item);
    }
    template<class T, std::size_t N> void one(T (&value)[N]) {
        for (auto& item : value) one(item);
    }
    template<class... T> void one(std::tuple<T...>& value) {
        std::apply([&](auto&... item) { (*this)(item...); }, value);
    }
    template<std::size_t N> void one(std::bitset<N>& value) {
        value.reset();
        for (std::size_t first = 0; first < N; first += 8) {
            std::uint8_t byte; one(byte);
            const auto used = std::min(std::size_t{8}, N - first);
            if ((unsigned(byte) >> used) != 0) invalid("invalid audio bitset padding");
            for (std::size_t bit = 0; bit < used; ++bit) value[first + bit] = (byte & (1u << bit)) != 0;
        }
    }
    template<class T, class U> void one(std::pair<T, U>& value) { (*this)(value.first, value.second); }
    template<class T> void one(std::optional<T>& value) {
        bool present; one(present); if (present) { value.emplace(); one(*value); } else value.reset();
    }
    template<class T> void one(std::shared_ptr<const T>& value) {
        bool present; one(present);
        if (present) {
            chargeAllocation(1, sizeof(T) + 2 * sizeof(void*));
            auto item = std::make_shared<T>(); one(*item); value = std::move(item);
        } else value.reset();
    }
    void one(std::shared_ptr<const engine::SampleBuffer>& value) {
        std::string id; one(id); if (id.empty()) { value.reset(); return; }
        if (!resources) invalid("audio resources are unavailable");
        const auto found = resources->find(id);
        if (found == resources->end()) invalid("unknown audio resource ID");
        value = found->second;
    }
    template<class T> void one(std::unordered_map<std::string, T>& value) {
        const auto n = count();
        // Include node links/hash and bucket pointers as well as key/value
        // storage. Nested strings and containers are charged independently.
        chargeAllocation(n, sizeof(typename std::unordered_map<std::string, T>::value_type) + 4 * sizeof(void*));
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key; T item; (*this)(key, item);
            if (!value.emplace(std::move(key), std::move(item)).second) invalid("duplicate audio session key");
        }
    }
    template<std::size_t I = 0, class... T> void variant(std::variant<T...>& value, std::uint32_t index) {
        if constexpr (I == sizeof...(T)) invalid("unknown audio control command");
        else if (index == I) { value.template emplace<I>(); one(std::get<I>(value)); }
        else variant<I + 1>(value, index);
    }
    template<class... T> void one(std::variant<T...>& value) { std::uint32_t index; one(index); variant(value, index); }
    void finish() const { if (cursor != bytes.size()) invalid("trailing audio session data"); }
};

/// These helpers have no framing/version of their own. The enclosing protocol
/// supplies it, so existing session/control/checkpoint wire formats stay stable.
/// Decoding returns fully owned values and rejects any unconsumed trailing data.
template<class... Values>
std::vector<std::uint8_t> encode(const Values&... values) {
    Writer writer; writer(values...); return std::move(writer.bytes);
}
template<class... Values>
std::tuple<std::remove_cvref_t<Values>...> decode(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    std::tuple<std::remove_cvref_t<Values>...> values;
    std::apply([&](auto&... value) { reader(value...); }, values);
    reader.finish(); return values;
}

/// Only referenced immutable PCM records enter this message's manifest. The
/// persistent registry still pins older generations until their owner retires.
template<class... Values>
std::vector<std::uint8_t> encodeResources(ProcessAudioResources& resources, const Values&... values) {
    Writer payload(&resources); payload(values...);
    Writer output; output.elements = payload.elements;
    std::vector<ProcessAudioResources::Record> records;
    for (const auto& record : resources.records())
        if (payload.usedResources.contains(record.id)) records.push_back(record);
    output(records); output.append(payload.bytes);
    return std::move(output.bytes);
}
template<class... Values>
std::tuple<std::remove_cvref_t<Values>...> decodeResources(std::span<const std::uint8_t> bytes,
    const std::filesystem::path& directory, ProcessAudioResources::Cache* cache = nullptr) {
    Reader reader(bytes);
    std::vector<ProcessAudioResources::Record> records; reader(records);
    auto nextCache = cache ? *cache : ProcessAudioResources::Cache{};
    const auto resources = ProcessAudioResources::load(directory, records, {}, &nextCache);
    reader.resources = &resources;
    std::tuple<std::remove_cvref_t<Values>...> values;
    std::apply([&](auto&... value) { reader(value...); }, values);
    reader.finish();
    if (cache) *cache = std::move(nextCache);
    return values;
}

} // namespace daw::audio_value

#pragma pop_macro("slots")
