#pragma once

#include "Common/Types.hpp"
#include "Midi/MidiEvent.hpp"

#include <span>
#include <string>
#include <cmath>

namespace daw::engine {

/// Meaning of an audio edge at its consumer. Ordinary nodes may ignore this
/// and continue to sum every input. PluginNode separates Sidechain edges into
/// the first auxiliary input bus instead of leaking them into the dry signal.
enum class InputRole : std::uint8_t { Main = 0, Sidechain = 1 };

/// Offline-only opt-in. Ordered nodes own their DSP state and depend solely on
/// their context and immutable session data. Capture nodes expose their last
/// block to the file sink and must not advance until that block is consumed.
/// Unknown nodes retain the block-synchronous renderer.
enum class OfflineNodePolicy : std::uint8_t { Barrier, Ordered, Capture };

/// Compile-time MIDI participation. The graph uses this to reserve fixed event
/// storage only for nodes and edges that can actually carry MIDI. The default is
/// intentionally conservative so third-party/custom Node implementations keep
/// the historic behaviour until they explicitly opt out.
enum class MidiNodeRole : std::uint8_t {
    None = 0,
    Input = 1,
    Output = 2,
    InputOutput = 3,
    /// Forwards upstream events, but cannot generate any without MIDI input.
    Passthrough = 7,
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
    SamplePos timelinePosition = 0;   // timeline sample at this node's input
    SampleRate sampleRate = 48000.0;
    bool playing = false;
    /// The realtime deadline is lifted (mixdown, freeze, bounce). It does not
    /// license a different signal: the offline render must match the live one
    /// sample for sample.
    bool offline = false;
    /// Musical time of the aligned inputs, adjusted for upstream latency.
    TransportInfo transport;

    void compensateInputLatency(FrameCount latency) noexcept {
        if (latency == 0 || (!playing && !offline) || sampleRate <= 0.0 ||
            transport.tempo <= 0.0) return;
        const double samplesPerBeat = 60.0 * sampleRate / transport.tempo;
        const double originalBeat = transport.ppqPosition;
        timelinePosition -= SamplePos(latency);
        transport.ppqPosition -= double(latency) / samplesPerBeat;
        // During a live cycle, delayed audio at the loop's beginning belongs
        // to the preceding cycle's end. Offline passes run a linear range.
        const double loopLength = transport.loopEndPpq - transport.loopStartPpq;
        if (!offline && transport.looping && loopLength > 0.0 &&
            originalBeat >= transport.loopStartPpq &&
            transport.ppqPosition < transport.loopStartPpq) {
            double offset = std::fmod(transport.ppqPosition - transport.loopStartPpq, loopLength);
            if (offset < 0.0) offset += loopLength;
            const double wrapped = transport.loopStartPpq + offset;
            timelinePosition += SamplePos(std::llround((wrapped - transport.ppqPosition) * samplesPerBeat));
            transport.ppqPosition = wrapped;
        }
        const double barLength = transport.timeSigDenominator > 0
            ? double(transport.timeSigNumerator) * 4.0 / transport.timeSigDenominator : 4.0;
        transport.barStartPpq = barLength > 0.0
            ? std::floor(transport.ppqPosition / barLength) * barLength : 0.0;
    }

    double ppqAtOffset(FrameCount frame) const noexcept {
        double beat = transport.ppqPosition;
        if (sampleRate > 0.0 && transport.tempo > 0.0)
            beat += double(frame) * transport.tempo / (sampleRate * 60.0);
        if (!offline && playing && transport.looping &&
            transport.loopEndPpq > transport.loopStartPpq && beat >= transport.loopEndPpq)
            beat = transport.loopStartPpq + std::fmod(beat - transport.loopStartPpq,
                transport.loopEndPpq - transport.loopStartPpq);
        return beat;
    }

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
    /// Opt-in for fusing cheap, audio-only one-in/one-out chain nodes into one
    /// scheduler job. This never changes their DSP calls or summation order.
    virtual bool canFuseTask() const noexcept { return false; }

    virtual OfflineNodePolicy offlineNodePolicy() const noexcept {
        return OfflineNodePolicy::Barrier;
    }

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
    /// Control thread, before prepare: connectivity is not the same as silence.
    /// A hosted compressor may select its detector from aux-bus activation.
    virtual void setSidechainConnected(bool) {}
    virtual void reset() {}
    /// Offline control thread, between completed blocks only. A callback may
    /// invalidate preparation; the pass must then stop rather than use stale
    /// routing/latency. During preparation the engine can compile it again.
    virtual Status serviceOffline() { return {}; }
    /// Queried after preparation and after a complete offline block. The live
    /// callback's fallback audio is not evidence of a successful export.
    virtual Status offlineStatus() const noexcept { return {}; }
    /// Last live block's DSP status; fallback silence is still a failed block.
    virtual Status processStatus() const noexcept { return {}; }
    /// Control thread: warm immutable source data before play/locate. Must not
    /// mutate live DSP state; the published graph may still be processing.
    virtual void preparePlayback(SamplePos) {}

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
