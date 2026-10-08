#pragma once

#include "AudioRuntimeCalls.hpp"

#include <map>
#include <variant>

namespace daw::audio_rpc {

/// Parent-owned recovery values. Only acknowledged host controls and exact
/// built-in source postimages enter this journal. Foreign parameter writes
/// remain excluded: their acknowledgement does not confirm audio processing.
class DurableEdits {
    template<Method Id> struct Call {
        static constexpr auto method = Id;
        typename Binding<Id>::Inputs values;
    };
    using Host = std::variant<Call<Method::setPluginControls>, Call<Method::setPluginSlide>,
        Call<Method::configureChannelColor>, Call<Method::setMetronomeEnabled>, Call<Method::setMetronomeSample>>;
    struct Identity {
        AudioPluginAddress address;
        std::string uid, version, fingerprint;
        plugins::Format format = plugins::Format::Unknown;
    };
    struct Entry { Identity identity; Host host; std::size_t bytes = 0; };
    using Key = std::tuple<Method, std::string, std::string, bool>;
    using Entries = std::map<Key, Entry>;
public:
    static constexpr std::size_t maxEntries = 4096, maxBytes = 8u * 1024u * 1024u;
    struct Prepared {
        Method method{};
        Entries staged;
        std::optional<Identity> source;
        bool tracked() const { return source.has_value() || !staged.empty(); }
    };

    /// Decode and allocate before dispatch. Failure cannot follow native mutation.
    Prepared prepare(Method, std::span<const std::uint8_t>, const std::filesystem::path&,
        const AudioSessionPacket&) const;
    static bool accepted(const Prepared&, std::span<const std::uint8_t>,
        const std::filesystem::path&, ProcessAudioResources::Cache&);
    void acceptHost(Prepared&&);
    static std::vector<AudioPluginStateRequest> sourceRequests(const Prepared&, const AudioSessionPacket&);
    static void acceptSource(const Prepared&, std::span<const AudioPluginStateSnapshot>, AudioSessionPacket&);
    /// Complete projection supersedes represented controls. Right-side slide
    /// and the metronome PCM have no corresponding field in the current spec.
    void projected(const AudioSessionPacket&);
    audio::Result replay(AudioRuntimeProcess&, std::uint64_t, const AudioSessionPacket&) const;
    void clear() { entries.clear(); bytes = 0; }
    std::size_t size() const { return entries.size(); }
private:
    Entries entries;
    std::size_t bytes = 0;
    static Identity identity(const AudioSessionPacket&, const AudioPluginAddress&);
    static bool matches(const Identity&, const AudioSessionPacket&);
};

} // namespace daw::audio_rpc
