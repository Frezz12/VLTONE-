#pragma once

#include "AudioRuntimeControl.hpp"
#include "AudioRuntimeState.hpp"
#include "AudioSessionSpec.hpp"
#include "ProcessAudioResources.hpp"

#include <span>
#include <variant>

namespace daw {

/// Complete input for a new runtime generation. Loading native checkpoints
/// must succeed before commitGraph; only then may the device start.
struct AudioSessionPacket {
    std::uint64_t generation = 0, revision = 0;
    double sampleRate = 48000;
    std::uint32_t blockSize = 512;
    bool offline = false;
    AudioSessionSpec session;
    std::vector<AudioPluginCheckpoint> checkpoints;
    std::vector<AudioTransportCommand> transport;
    /// Initial native state for newly prepared slots, applied before publication.
    /// Kept with the acknowledged projection so restart also retains PCM sources.
    std::vector<AudioPluginStateEdit> restores;
};

struct AudioFaderCommand {
    std::string channelId, clipId;
    AudioFaderTarget target = AudioFaderTarget::Channel;
    AudioFaderChange change;
};
struct AudioInputCommand { std::string channelId; AudioGraphSpec::Input input; };
struct AudioSendCommand { std::string channelId, sendId; float level = 0; bool enabled = true; };
struct AudioMidiCommand { std::string channelId; engine::MidiEvent event; };
struct AudioControlPacket {
    std::uint64_t generation = 0, requestId = 0;
    std::variant<AudioTransportCommand, AudioPreviewCommand, AudioFaderCommand,
        AudioInputCommand, AudioSendCommand, AudioMidiCommand> command;
};

/// Versioned little-endian values. All sizes are checked before allocation;
/// pointers, C++ object representations and prepared DSP state are excluded.
/// Throws on malformed data, unknown versions/types, missing PCM or limits.
inline constexpr std::size_t kMaxAudioSessionBytes = 512u * 1024u * 1024u;
/// Semantic checks shared by full sessions and partial content publications.
/// Throws before a runtime mutates any live schedule or automation snapshot.
void validateAudioContent(const AudioContentSpec& content);
std::vector<std::uint8_t> encodeAudioSession(const AudioSessionPacket& packet,
    ProcessAudioResources& resources);
AudioSessionPacket decodeAudioSession(std::span<const std::uint8_t> bytes,
    const std::filesystem::path& resourceDirectory, ProcessAudioResources::Cache* cache = nullptr);
std::vector<std::uint8_t> encodeAudioControl(const AudioControlPacket& packet);
AudioControlPacket decodeAudioControl(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encodeAudioCheckpoints(std::span<const AudioPluginCheckpoint> checkpoints);
std::vector<AudioPluginCheckpoint> decodeAudioCheckpoints(std::span<const std::uint8_t> bytes);

} // namespace daw
