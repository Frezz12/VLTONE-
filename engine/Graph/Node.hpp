#pragma once

#include "Common/Types.hpp"
#include "Midi/MidiEvent.hpp"

#include <span>
#include <string>

namespace daw::engine {

/// Meaning of an audio edge at its consumer. Ordinary nodes may ignore this
/// and continue to sum every input. PluginNode separates Sidechain edges into
/// the first auxiliary input bus instead of leaking them into the dry signal.
enum class InputRole : std::uint8_t { Main = 0, Sidechain = 1 };

/// Compile-time MIDI participation. The graph uses this to reserve fixed event
/// storage only for nodes and edges that can actually carry MIDI. The default is
/// intentionally conservative so third-party/custom Node implementations keep
/// the historic behaviour until they explicitly opt out.
enum class MidiNodeRole : std::uint8_t {
    None = 0,
    Input = 1,
    Output = 2,
    InputOutput = 3,
};

constexpr bool acceptsMidi(MidiNodeRole role) noexcept {
    return (std::uint8_t(role) & std::uint8_t(MidiNodeRole::Input)) != 0;
}
constexpr bool producesMidi(MidiNodeRole role) noexcept {
    return (std::uint8_t(role) & std::uint8_t(MidiNodeRole::Output)) != 0;
}

/// Everything a node needs to know before audio starts flowing.
struct PrepareInfo {
    SampleRate sampleRate = 48000.0;
    FrameCount maxBlockSize = 512;
    ChannelCount channels = 2;
    bool offline = false;

    friend bool operator==(const PrepareInfo&, const PrepareInfo&) = default;
};

/// One block of work for a node. `inputs` are the output blocks of the nodes
/// feeding it, already delay-compensated and in a fixed order, so a node that
/// sums them produces bit-identical results on every run regardless of which
/// worker thread happened to produce them.
struct ProcessContext {
    AudioBlock output;
    std::span<const AudioBlock> inputs;
    /// Index-parallel with `inputs`.
    std::span<const InputRole> inputRoles;
    FrameCount frames = 0;
    SamplePos timelinePosition = 0;   // in samples, at the block start
    SampleRate sampleRate = 48000.0;
    bool playing = false;
    /// The realtime deadline is lifted (mixdown, freeze, bounce). It does not
    /// license a different signal: the offline render must match the live one
    /// sample for sample.
    bool offline = false;
    /// Musical time for this block. Derived from the transport once per block,
    /// so every node in the graph agrees on it.
    TransportInfo transport;

    /// The MIDI arriving on the same edges as the audio, in the same order.
    ///
    /// MIDI rides alongside audio rather than on edges of its own. In a DAW the
    /// two follow the same path — clip into instrument into effects — so a
    /// second set of connections would only ever mirror the first, and it would
    /// double the scheduler, the arena and the compensation logic to say the
    /// same thing. A node simply ignores whichever half it does not use. The
    /// price: a plugin with separate audio and MIDI input ports cannot tell
    /// which producer a note came from.
    std::span<const MidiBuffer* const> midiInputs;
    /// Where this node writes MIDI, if its MidiNodeRole includes Output. Cleared
    /// before every block; null for nodes which explicitly opt out.
    MidiBuffer* midiOutput = nullptr;
};

/// The single interface every unit of DSP implements — tracks, clips, plugins,
/// buses, meters, sends. The graph knows nothing else about them, so a new
/// module becomes a first-class citizen simply by implementing this.
class Node {
public:
    virtual ~Node() = default;

    virtual std::string_view name() const noexcept = 0;

    /// Channels this node writes. The compiler sizes buffers from the widest.
    virtual ChannelCount outputChannels() const noexcept { return 2; }

    /// Processing latency this node introduces, in samples. The compiler uses
    /// it to align every path through the graph (automatic PDC).
    virtual FrameCount latencySamples() const noexcept { return 0; }

    /// How long the node keeps producing after its input goes silent (reverb
    /// tails, delay feedback). Used by freeze/bounce to know when to stop.
    virtual FrameCount tailSamples() const noexcept { return 0; }

    /// True when the node produces signal without any input (sources).
    virtual bool isSource() const noexcept { return false; }

    /// Whether this node reads and/or writes MIDI. Audio-only built-ins return
    /// None, avoiding a reserved 512-event buffer per node. InputOutput remains
    /// the default for source compatibility with custom Node implementations.
    virtual MidiNodeRole midiRole() const noexcept {
        return MidiNodeRole::InputOutput;
    }

    /// Size scratch and pick coefficients. Control thread, may allocate.
    ///
    /// The graph calls this only when the settings actually changed — see
    /// `isPreparedFor`. Implementations may still assume it runs before the
    /// first `process`.
    virtual void prepare(const PrepareInfo&) {}
    virtual void reset() {}

    /// Stop and restart the node's own processing around a reconfiguration.
    /// Nothing in the engine calls these; they exist for nodes that wrap
    /// something with its own activation protocol (a hosted plugin's
    /// stopProcessing/startProcessing), driven by whoever owns that node.
    virtual void suspend() {}
    virtual void resume() {}

    /// Realtime. No allocation, no locks, no I/O.
    virtual void process(const ProcessContext& context) = 0;

    // ── Prepare bookkeeping, owned by AudioGraph::compile ──
    //
    // A rebuild re-adopts the same Node objects, so preparing every node on
    // every compile is not merely wasteful: `prepare()` reallocates scratch
    // that `process()` reads, and the previously published snapshot — pointing
    // at this very object — may still be rendering on the audio thread. The
    // stamp lives on the node rather than on the graph's slot because
    // `EngineController::rebuildGraph` throws the whole AudioGraph away and
    // builds a fresh one, which a slot-side stamp would not survive.

    bool isPreparedFor(const PrepareInfo& info) const noexcept {
        return m_prepared && m_preparedWith == info;
    }
    void markPrepared(const PrepareInfo& info) noexcept {
        m_preparedWith = info;
        m_prepared = true;
    }
    /// Force the next compile to re-prepare this node (its scratch needs are
    /// about to change — a plugin swapping its bus layout, say).
    void invalidatePrepare() noexcept { m_prepared = false; }

private:
    PrepareInfo m_preparedWith{};
    bool m_prepared = false;
};

} // namespace daw::engine
