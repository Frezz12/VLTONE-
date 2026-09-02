#pragma once

#include "Common/Types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

/// Plugin hosting: one internal interface, one implementation per format.
///
/// The vocabulary here is shaped after CLAP, because of the three formats it is
/// the closest to a superset — sample-accurate events in a single ordered
/// stream, explicit buses, stable string identities, state as an opaque byte
/// stream, and an explicit split between the main thread and the audio thread.
/// VST3 and AU are adapted *onto* this. Going the other way would mean
/// splitting CLAP's one event stream into separate note and parameter lists and
/// losing the ordering between them.
namespace daw::plugins {

/// `Internal` is the odd one out: it is not a plugin format at all but the
/// build's own instruments and effects, wearing the same interface so that
/// slots, state, automation and the picker menus need no second code path. It
/// is never scanned — see `InternalFactory`.
enum class Format : std::uint8_t {
    Unknown = 0,
    Clap = 1,
    Vst3 = 2,
    AudioUnit = 3,
    Internal = 4,
    Vst = 5
};

std::string_view toString(Format format) noexcept;
Format formatFromString(std::string_view name) noexcept;

/// Everything a scan learned about one plugin: enough to list it in a browser
/// and to re-instantiate it later without opening the module again.
struct PluginDescriptor {
    Format format = Format::Unknown;
    /// Format-native identity, and what a project file stores. CLAP's
    /// `clap_plugin_descriptor::id`, a VST3 class id, an AU type:subtype:manu.
    /// Resolved before `path`, so a plugin that moved on disk still loads.
    std::string uid;
    std::string path;        ///< module or bundle, as found by the scanner
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    bool isInstrument = false;
    /// Not filled in by any scanner: finding out costs a full instantiation,
    /// which is the expensive half of a scan, and nothing needs the answer
    /// until an editor is opened — at which point the instance is asked
    /// directly. Kept because the cache format carries it.
    bool hasEditor = false;
    bool wantsMidi = false;
    /// A scan-time guess, not the truth: finding the real widths means opening
    /// the plugin, which is the expensive half of a scan. Both are corrected
    /// from the plugin itself when an instance is created, and that corrected
    /// copy — `PluginInstance::descriptor()` — is what the graph must be sized
    /// from. A surround plugin reads 2 here and 6 there.
    std::uint16_t mainInputChannels = 2;
    std::uint16_t mainOutputChannels = 2;
    /// Identity of the file the scan read, for incremental rescans.
    std::uint64_t fileSize = 0;
    std::int64_t fileModifiedTime = 0;
    /// Version of the plugin's opaque save/load state contract when known.
    /// External formats normally leave this at zero; built-ins publish it.
    int stateSchemaVersion = 0;
};

struct ParameterInfo {
    std::uint32_t index = 0;
    /// Stable across sessions and across plugin versions — this, not `index`,
    /// is what a project file and an automation lane refer to.
    std::string id;
    std::string name;
    std::string unit;
    /// Plain units, the plugin's own scale. Kept plain rather than normalised
    /// because AU is plain-only and VST3 can convert; normalising everything
    /// would make VST3 the only format that round-trips exactly.
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool isAutomatable = true;
    bool isStepped = false;
    bool isBypass = false;
};

/// One timestamped thing inside a block. Trivially copyable on purpose: it
/// travels through a lock-free ring between the control and audio threads.
struct PluginEvent {
    enum class Kind : std::uint8_t {
        ParamValue,
        ParamGestureBegin,
        ParamGestureEnd,
        NoteOn,
        NoteOff,
        NoteChoke,
        /// MIDI CC, channel pressure, pitch bend or program change. `paramIndex`
        /// carries the VST3 ControllerNumbers value (0…131), `value` is 0…1.
        MidiController,
        PolyPressure,
    };

    Kind kind = Kind::ParamValue;
    std::uint32_t frameOffset = 0;   ///< from the start of the block
    std::uint32_t sortOrder = 0;     ///< host-assigned stable tie breaker
    std::uint32_t paramIndex = 0;
    std::int16_t channel = 0;
    std::int16_t key = 60;
    std::int32_t noteId = -1;        ///< -1 = address by key/channel
    /// Plain parameter value, or note velocity in 0…1.
    double value = 0.0;
    /// Host-only per-note stereo position, -1 ... 1. Format adapters may
    /// ignore it; internal instruments can render it without inventing a CC.
    double notePan = 0.0;
};
static_assert(std::is_trivially_copyable_v<PluginEvent>,
              "PluginEvent travels through a lock-free ring");

/// Channels per bus. Index 0 is the main bus; input index 1 is the host-routed
/// sidechain. Additional inputs and non-main outputs are still exposed to the
/// format and fed silence/discarded until the graph grows named auxiliary
/// buses beyond sidechain.
struct PluginBusLayout {
    std::vector<std::uint16_t> inputs;
    std::vector<std::uint16_t> outputs;
};

struct PluginProcessInfo {
    double sampleRate = 48000.0;
    std::uint32_t maxBlockSize = 512;
    bool offline = false;
};

/// What the format says after a successful process call. Only an explicit
/// format contract may return Sleep/Tail; output silence by itself is not
/// permission for a host to stop calling an arbitrary plugin.
enum class PluginProcessDisposition : std::uint8_t {
    Continue,
    Tail,
    Sleep,
};

/// Plugin → host, during `process`. Implementations must not block.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void push(const PluginEvent& event) noexcept = 0;
};

/// One block of work for a plugin. Planar, because that is what CLAP, VST3, AU
/// and the engine's own buffer arena all use.
struct PluginProcessContext {
    const float* const* inputs = nullptr;
    std::uint16_t inputChannels = 0;
    /// First auxiliary audio input bus. Null/zero means no sidechain signal;
    /// the format adapter still exposes the declared bus and feeds it silence.
    const float* const* sidechainInputs = nullptr;
    std::uint16_t sidechainInputChannels = 0;
    /// Exact zero masks for the two supplied buses. A set bit means every
    /// sample in that channel is 0.0f. Format adapters propagate these to
    /// VST3 silenceFlags, CLAP constant_mask and AU render flags.
    std::uint64_t inputSilenceMask = 0;
    std::uint64_t sidechainSilenceMask = 0;
    float* const* outputs = nullptr;
    std::uint16_t outputChannels = 0;
    std::uint32_t frames = 0;

    /// Sorted by `frameOffset`; a plugin may rely on that.
    std::span<const PluginEvent> inputEvents;
    EventSink* outputEvents = nullptr;

    engine::TransportInfo transport;
    std::int64_t sampleTime = 0;
    bool playing = false;
    bool offline = false;
};

} // namespace daw::plugins
