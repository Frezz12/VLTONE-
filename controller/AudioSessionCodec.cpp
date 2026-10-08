#include "AudioSessionCodec.hpp"
#include "AudioValueCodec.hpp"

#include <nlohmann/json.hpp>

#include <set>

namespace daw::audio_value {

void fields(Writer& writer, plugins::mini::MiniModuleDefinition& value) {
    writer(plugins::mini::toJson(value).dump());
}
void fields(Reader& reader, plugins::mini::MiniModuleDefinition& value) {
    std::string text; reader(text);
    value = plugins::mini::fromJson(nlohmann::json::parse(text));
}
} // namespace daw::audio_value

namespace daw {
namespace {
using audio_value::Writer;
using audio_value::Reader;
using audio_value::invalid;
using audio_value::maxString;
static_assert(kMaxAudioSessionBytes == audio_value::maxBytes);
constexpr std::uint32_t sessionMagic = 0x53414c56, controlMagic = 0x43414c56,
    checkpointMagic = 0x50414c56, version = 3;
template<class Enum> void range(Enum value, Enum last) {
    if (std::uint32_t(value) > std::uint32_t(last)) invalid("unknown audio session enum");
}
void validate(const AudioTransportCommand& command) {
    range(command.action, AudioTransportCommand::Action::Duration);
    if (!std::isfinite(command.value)) invalid("invalid transport number");
}
void validate(const AudioGraphSpec::Input& input) {
    if (input.channel >= engine::kMaxChannels || !input.channelCount || input.channelCount > 2 || input.monitorMask > 3)
        invalid("invalid input routing");
}
void validate(const AudioSessionPacket& packet) {
    if (!packet.generation || !std::isfinite(packet.sampleRate) || packet.sampleRate < 1000 ||
        packet.sampleRate > 768000 || !packet.blockSize || packet.blockSize > engine::kMaxBlockSize)
        invalid("invalid audio session configuration");
    range(packet.session.hosting.mode, plugins::HostingMode::Isolated);
    std::unordered_set<std::string> channels, content;
    std::set<std::pair<std::string, std::string>> slots;
    for (const auto& channel : packet.session.graph.channels) {
        if (channel.id.empty() || channel.id == AudioGraphSpec::masterChannelId || !channels.insert(channel.id).second)
            invalid("duplicate audio channel");
        validate(channel.input);
        for (const auto& clip : channel.clipFx) range(clip.playbackInjection.stage, PlaybackInjectionStage::BeforeMasterFader);
    }
    channels.insert(AudioGraphSpec::masterChannelId);
    for (const auto& chain : packet.session.pluginChains) {
        range(chain.kind, AudioPluginChainSpec::Kind::ClipFx);
        if (!channels.contains(chain.channelId)) invalid("unknown plugin channel");
        for (const auto& slot : chain.slots) {
            if (slot.id.empty() || !slots.emplace(chain.channelId, slot.id).second) invalid("duplicate plugin slot");
            range(slot.descriptor.format, plugins::Format::Vst); range(slot.requiredFormat, plugins::Format::Vst);
            range(slot.channelMode, PluginChannelMode::DualMono);
            range(slot.loadPolicy, AudioPluginLoadPolicy::PlaceholderOnly);
            if (!slot.preferredChannels || slot.preferredChannels > 2) invalid("invalid plugin channel layout");
        }
    }
    for (const auto& channel : packet.session.channels) {
        if (!channels.contains(channel.id) || !content.insert(channel.id).second) invalid("duplicate or unknown content channel");
        validateAudioContent(channel.content);
    }
    std::set<std::pair<std::string, std::string>> checkpoints;
    for (const auto& checkpoint : packet.checkpoints) {
        const auto key = std::pair{checkpoint.channelId, checkpoint.slotId};
        if (!slots.contains(key) || !checkpoints.insert(key).second) invalid("duplicate or unknown checkpoint slot");
        range(checkpoint.format, plugins::Format::Vst);
    }
    for (const auto& command : packet.transport) validate(command);
    std::set<std::tuple<std::string, std::string, bool>> restored;
    for (const auto& edit : packet.restores) {
        const auto& address = edit.address;
        if ((address.instance && !edit.replaceExisting) || !slots.contains({address.channelId, address.slotId}) ||
            !restored.emplace(address.channelId, address.slotId, address.right).second)
            invalid("duplicate, unknown or instance-bound initial plugin state");
        if (address.right) {
            bool dual = false;
            for (const auto& chain : packet.session.pluginChains) if (chain.channelId == address.channelId)
                for (const auto& slot : chain.slots) if (slot.id == address.slotId)
                    dual = slot.channelMode == PluginChannelMode::DualMono;
            if (!dual) invalid("right-side state requires a dual-mono plugin");
        }
    }
}
void validate(const AudioControlPacket& packet) {
    if (!packet.generation || !packet.requestId) invalid("invalid audio control generation/request");
    std::visit([](const auto& command) {
        using T = std::decay_t<decltype(command)>;
        if constexpr (std::is_same_v<T, AudioTransportCommand>) validate(command);
        else if constexpr (std::is_same_v<T, AudioPreviewCommand>) range(command.action, AudioPreviewCommand::Action::SeekSeconds);
        else {
            if (command.channelId.empty()) invalid("empty audio command channel");
            if constexpr (std::is_same_v<T, AudioFaderCommand>) range(command.target, AudioFaderTarget::Clip);
            else if constexpr (std::is_same_v<T, AudioInputCommand>) validate(command.input);
            else if constexpr (std::is_same_v<T, AudioMidiCommand>) {
                if (command.event.data1 > 127 || command.event.data2 > 127 ||
                    command.event.status < 0x80 || command.event.status > 0xef)
                    invalid("invalid live MIDI event");
                range(command.event.pitch.shape, engine::curve::Shape::SCurve);
            }
        }
    }, packet.command);
}
void readHeader(Reader& reader, std::uint32_t expected) {
    std::uint32_t magic, wireVersion; reader(magic, wireVersion);
    if (magic != expected || wireVersion != version) invalid("incompatible audio protocol");
}
} // namespace

void validateAudioContent(const AudioContentSpec& content) {
    if (content.clips) {
        const auto check = [](const auto& clips) { if (clips) for (const auto& clip : *clips)
            if (clip.warp && !validWarp(*clip.warp)) invalid("invalid clip warp"); };
        check(content.clips->shared);
        for (const auto& [id, clips] : content.clips->individual) check(clips);
    }
    if (content.midi && content.midi->notes)
        for (const auto& note : *content.midi->notes) {
            if (note.key > 127 || note.velocity > 127 || note.channel > 15 || note.releaseVelocity > 127)
                invalid("invalid MIDI note");
            for (const auto& point : note.pitch) range(point.shape, engine::curve::Shape::SCurve);
        }
    if (content.midi && content.midi->controllers)
        for (const auto& controller : *content.midi->controllers) {
            if (controller.channel < 0 || controller.channel > 15 || controller.key < 0 || controller.key > 127 ||
                controller.cc < 0 || controller.cc > 131) invalid("invalid MIDI controller");
            for (const auto& point : controller.points) range(point.shape, engine::curve::Shape::SCurve);
        }
}

std::vector<std::uint8_t> encodeAudioSession(const AudioSessionPacket& packet, ProcessAudioResources& resources) {
    validate(packet);
    Writer payload(&resources); payload(packet);
    Writer output; output.elements = payload.elements;
    std::vector<ProcessAudioResources::Record> records;
    for (const auto& record : resources.records())
        if (payload.usedResources.contains(record.id)) records.push_back(record);
    output(sessionMagic, version, records); output.append(payload.bytes);
    return std::move(output.bytes);
}
AudioSessionPacket decodeAudioSession(std::span<const std::uint8_t> bytes, const std::filesystem::path& directory,
    ProcessAudioResources::Cache* cache) {
    if (bytes.size() > kMaxAudioSessionBytes) invalid("audio session exceeds byte limit");
    Reader reader{bytes}; readHeader(reader, sessionMagic);
    std::vector<ProcessAudioResources::Record> records; reader(records);
    auto nextCache = cache ? *cache : ProcessAudioResources::Cache{};
    const auto resources = ProcessAudioResources::load(directory, records, {}, &nextCache); reader.resources = &resources;
    AudioSessionPacket packet; reader(packet); reader.finish(); validate(packet);
    if (cache) *cache = std::move(nextCache);
    return packet;
}
std::vector<std::uint8_t> encodeAudioControl(const AudioControlPacket& packet) {
    validate(packet); Writer output; output(controlMagic, version, packet); return std::move(output.bytes);
}
AudioControlPacket decodeAudioControl(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > maxString) invalid("audio control exceeds byte limit");
    Reader reader{bytes}; readHeader(reader, controlMagic);
    AudioControlPacket packet; reader(packet); reader.finish(); validate(packet); return packet;
}
std::vector<std::uint8_t> encodeAudioCheckpoints(std::span<const AudioPluginCheckpoint> checkpoints) {
    Writer output; output(checkpointMagic, version); output.count(checkpoints.size());
    for (const auto& checkpoint : checkpoints) output(checkpoint);
    return std::move(output.bytes);
}
std::vector<AudioPluginCheckpoint> decodeAudioCheckpoints(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kMaxAudioSessionBytes) invalid("audio checkpoints exceed byte limit");
    Reader reader{bytes}; readHeader(reader, checkpointMagic);
    std::vector<AudioPluginCheckpoint> out; reader(out); reader.finish();
    return out;
}

} // namespace daw
