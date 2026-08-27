// MIDI through the engine, and from a clip into an instrument.
//
// The fixture's instrument half is the crudest synth that can be measured: it
// writes the held note's velocity as a DC level and silence otherwise. A host
// that loses a note-on writes silence, one that loses a note-off writes
// forever, and one that mistimes either puts the edge at the wrong sample — all
// three visible in one array of floats.
#include "Clap/ClapFactory.hpp"
#include "Engine/RealtimeEngine.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Host/PluginNode.hpp"
#include "Midi/MidiEvent.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace daw;
using namespace daw::plugins;
using namespace daw::engine;

static int failures = 0;
static void check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

namespace {

constexpr FrameCount kBlock = 128;

struct OutputBuffer {
    OutputBuffer(ChannelCount channels, FrameCount frames)
        : storage(std::size_t(channels) * frames, 0.0f), pointers(channels),
          channels(channels), frames(frames) {
        for (ChannelCount ch = 0; ch < channels; ++ch) {
            pointers[ch] = storage.data() + std::size_t(ch) * frames;
        }
    }
    AudioBlock block() { return AudioBlock(pointers.data(), channels, frames); }
    std::vector<float> storage;
    std::vector<float*> pointers;
    ChannelCount channels;
    FrameCount frames;
};

/// Records the MIDI it was handed, so a routing assertion can look at it.
class RecorderNode : public Node {
public:
    std::string_view name() const noexcept override { return "recorder"; }
    FrameCount latencySamples() const noexcept override { return m_latency; }
    void setLatency(FrameCount latency) { m_latency = latency; }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            std::fill_n(context.output.data(ch), context.frames, 0.0f);
        }
        received.clear();
        for (const MidiBuffer* buffer : context.midiInputs) {
            if (!buffer) continue;
            for (const MidiEvent& event : buffer->events()) received.push_back(event);
        }
    }

    std::vector<MidiEvent> received;

private:
    FrameCount m_latency = 0;
};

class PluginEventCapture final : public EventSink {
public:
    void push(const PluginEvent& event) noexcept override {
        if (count < events.size()) events[count++] = event;
    }

    std::array<PluginEvent, 512> events{};
    std::size_t count = 0;
};

/// A non-source node which originates MIDI from an ordinary audio input. Hosted
/// audio-to-MIDI plugins have this graph shape, so upstream MIDI reachability
/// must not decide whether their output buffer exists.
class AudioMidiGeneratorNode final : public Node {
public:
    std::string_view name() const noexcept override { return "audio-midi-generator"; }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            std::fill_n(context.output.data(ch), context.frames, 0.0f);
        }
        if (context.midiOutput) {
            context.midiOutput->push(MidiEvent::noteOn(0, 0, 73, 100));
        }
    }
};

class ModeMidiSourceNode final : public Node {
public:
    std::string_view name() const noexcept override { return "mode-midi-source"; }
    bool isSource() const noexcept override { return true; }
    MidiNodeRole midiRole() const noexcept override { return MidiNodeRole::Output; }

    void emitNextLive(std::uint8_t key) noexcept {
        m_liveKey.store(key, std::memory_order_relaxed);
        m_emitLive.store(true, std::memory_order_release);
    }
    void emitNextOffline(std::uint8_t key) noexcept {
        m_offlineKey.store(key, std::memory_order_relaxed);
        m_emitOffline.store(true, std::memory_order_release);
    }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            std::fill_n(context.output.data(ch), context.frames, 0.0f);
        }
        std::atomic<bool>& flag = context.offline ? m_emitOffline : m_emitLive;
        if (!context.midiOutput ||
            !flag.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        const std::uint8_t key = context.offline
                                     ? m_offlineKey.load(std::memory_order_relaxed)
                                     : m_liveKey.load(std::memory_order_relaxed);
        context.midiOutput->push(MidiEvent::noteOn(0, 0, key, 100));
    }

private:
    std::atomic<bool> m_emitLive{false};
    std::atomic<bool> m_emitOffline{false};
    std::atomic<std::uint8_t> m_liveKey{76};
    std::atomic<std::uint8_t> m_offlineKey{77};
};

class CountingMidiSinkNode final : public Node {
public:
    std::string_view name() const noexcept override { return "counting-midi-sink"; }

    void process(const ProcessContext& context) override {
        for (ChannelCount ch = 0; ch < context.output.numChannels(); ++ch) {
            std::fill_n(context.output.data(ch), context.frames, 0.0f);
        }
        for (const MidiBuffer* input : context.midiInputs) {
            if (!input) continue;
            for (const MidiEvent& event : input->events()) {
                if (event.isNoteOn()) {
                    noteOns.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    std::atomic<unsigned> noteOns{0};
};

PrepareInfo makeInfo() {
    PrepareInfo info;
    info.sampleRate = 48000.0;
    info.maxBlockSize = kBlock;
    info.channels = 2;
    return info;
}

/// 120 BPM at 48 kHz: one beat is 24000 samples, so a block of 128 is a small
/// fraction of a beat and note edges land well inside it.
TransportInfo transportAt(double beats) {
    TransportInfo transport;
    transport.tempo = 120.0;
    transport.ppqPosition = beats;
    return transport;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const PrepareInfo info = makeInfo();

    // ── Audio-only graph storage ──
    {
        AudioGraph graph;
        NodeId sink = kInvalidNode;
        for (int index = 0; index < 64; ++index) {
            sink = graph.addNode(std::make_unique<GainNode>("audio-only"));
        }
        graph.setSink(sink);
        auto compiled = graph.compile(info);
        check(compiled.has_value() && (*compiled)->midiBuffers.empty(),
              "audio-only nodes reserve no per-node MIDI event buffers");
        check(compiled.has_value() && (*compiled)->midiDelays.empty(),
              "audio-only graphs reserve no MIDI delay queues");
    }

    {
        AudioGraph graph;
        const NodeId audio = graph.addNode(std::make_unique<GainNode>("audio input"));
        const NodeId generator =
            graph.addNode(std::make_unique<AudioMidiGeneratorNode>());
        auto recorder = std::make_shared<RecorderNode>();
        const NodeId recorderId = graph.adoptNode(recorder);
        graph.connect(audio, generator);
        graph.connect(generator, recorderId);
        graph.setSink(recorderId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "an audio-fed MIDI generator graph compiles");
        if (compiled) {
            GraphProcessor processor(2);
            processor.setGraph(*compiled);
            OutputBuffer output(2, kBlock);
            processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
            check(recorder->received.size() == 1 &&
                      recorder->received.front().isNoteOn() &&
                      recorder->received.front().data1 == 73,
                  "a non-source generator keeps MIDI output without upstream MIDI");
        }
    }

    // ── The buffer ──
    {
        MidiBuffer buffer;
        buffer.reserve(4);
        check(buffer.push(MidiEvent::noteOn(10, 0, 60, 100)), "an event goes in");
        check(buffer.push(MidiEvent::noteOff(5, 0, 60)), "and another");
        buffer.sort();
        check(buffer.events()[0].frameOffset == 5 && buffer.events()[1].frameOffset == 10,
              "sorting puts them in time order");

        buffer.push(MidiEvent::noteOn(1, 0, 61, 100));
        buffer.push(MidiEvent::noteOn(2, 0, 62, 100));
        // Full: refusing is the only realtime-safe answer, because growing
        // would allocate on the audio thread.
        check(!buffer.push(MidiEvent::noteOn(3, 0, 63, 100)),
              "a full buffer refuses rather than reallocating");
        check(buffer.size() == 4, "and keeps what it had");
        check(buffer.droppedEvents() == 1,
              "buffer overflow is retained for diagnostics");

        check(buffer.push(MidiEvent::noteOff(3, 0, 61)),
              "a note-off displaces a lower-priority event at capacity");
        bool keptRelease = false;
        for (const MidiEvent& event : buffer.events()) {
            if (event.isNoteOff() && event.data1 == 61) keptRelease = true;
        }
        check(keptRelease && buffer.noteOffRescues() == 1,
              "the rescued release is observable and counted");
        check(buffer.droppedEvents() == 1,
              "rescuing a note-off is not reported as an incoming drop");

        MidiBuffer sameFrame;
        sameFrame.reserve(2);
        sameFrame.push(MidiEvent::noteOn(0, 0, 61, 100));
        sameFrame.push(MidiEvent::noteOn(0, 0, 60, 100));
        check(sameFrame.push(MidiEvent::noteOff(0, 0, 60)),
              "a same-frame release is rescued at capacity");
        sameFrame.sort();
        bool key60Held = false;
        for (const MidiEvent& event : sameFrame.events()) {
            if (event.data1 != 60) continue;
            if (event.isNoteOn()) key60Held = true;
            if (event.isNoteOff()) key60Held = false;
        }
        check(!key60Held,
              "a rescued same-frame note-off remains after its matching note-on");

        const MidiEvent zeroVelocity{0, 0x90, 60, 0};
        check(zeroVelocity.isNoteOff() && !zeroVelocity.isNoteOn(),
              "a note-on with zero velocity is a note-off");

        MidiBuffer releases;
        releases.reserve(2);
        releases.push(MidiEvent::noteOff(0, 0, 60));
        releases.push(MidiEvent::noteOff(0, 0, 61));
        check(!releases.push(MidiEvent::noteOff(0, 0, 62)) &&
                  releases.size() == 2 && releases.droppedEvents() == 1,
              "overflow never sacrifices one queued release for another");

        MidiBuffer dense;
        dense.reserve(128);
        for (std::uint32_t index = 0; index < 64; ++index) {
            dense.push(MidiEvent{63 - index, MidiEvent::kControlChange,
                                 std::uint8_t(index), 64});
        }
        for (std::uint32_t index = 0; index < 40; ++index) {
            dense.push(MidiEvent{80, MidiEvent::kControlChange,
                                 std::uint8_t(64 + index), 64});
        }
        dense.sort();
        bool ordered = true;
        for (std::size_t index = 1; index < dense.size(); ++index) {
            ordered &= dense.events()[index - 1].frameOffset <=
                       dense.events()[index].frameOffset;
        }
        bool stableTie = true;
        std::uint8_t expected = 64;
        for (const MidiEvent& event : dense.events()) {
            if (event.frameOffset != 80) continue;
            stableTie &= event.data1 == expected++;
        }
        check(ordered && stableTie,
              "dense MIDI uses bounded N-log-N sorting with stable equal-frame order");
    }

    // ── The delay ──
    {
        MidiDelay delay;
        delay.prepare(64, 16);
        MidiBuffer in;
        in.reserve(8);
        MidiBuffer out;
        out.reserve(8);

        in.push(MidiEvent::noteOn(10, 0, 60, 100));
        delay.process(in, out, kBlock);
        check(out.size() == 1 && out.events()[0].frameOffset == 74,
              "an event inside the block is shifted by the delay");

        // Past the end of the block: held, and emitted by the next one at the
        // offset that keeps its distance from the note it followed.
        in.clear();
        in.push(MidiEvent::noteOn(100, 0, 62, 100));
        delay.process(in, out, kBlock);
        check(out.empty(), "an event pushed past the block end is held back");
        in.clear();
        delay.process(in, out, kBlock);
        check(out.size() == 1 && out.events()[0].frameOffset == 36,
              "and arrives in the next block at the right offset");

        // Long plugin latency must not require touching the event in every
        // intervening block.  The observable timing is unchanged while the
        // delay internally keeps future events in absolute-time heap order.
        delay.prepare(kBlock * 4 + 17, 16);
        in.clear();
        in.push(MidiEvent::noteOn(3, 0, 65, 100));
        for (int block = 0; block < 4; ++block) {
            delay.process(in, out, kBlock);
            check(out.empty(), "a long-latency event remains pending");
            in.clear();
        }
        delay.process(in, out, kBlock);
        check(out.size() == 1 && out.events()[0].frameOffset == 20 &&
                  out.events()[0].data1 == 65,
              "a long-latency event keeps its exact final offset");

        delay.prepare(kBlock * 8, 2);
        in.clear();
        in.push(MidiEvent::noteOn(0, 0, 60, 100));
        in.push(MidiEvent::noteOn(1, 0, 61, 100));
        in.push(MidiEvent::noteOff(2, 0, 60));
        delay.process(in, out, kBlock);
        check(delay.pendingNoteOffRescues() == 1 &&
                  delay.droppedPendingEvents() == 0,
              "a full delay queue retains a note-off instead of risking a stuck voice");

        in.clear();
        in.push(MidiEvent{0, MidiEvent::kControlChange, 1, 64});
        delay.process(in, out, kBlock);
        check(delay.droppedPendingEvents() == 1,
              "delay-queue overflow is retained for diagnostics");
    }

    // ── The clip player ──
    {
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        // 24000 samples per beat: a note at beat 0.001 starts at sample 24.
        notes->push_back(MidiNote{0.001, 0.002, 64, 99, 0});
        player->setNotes(notes);

        AudioGraph graph;
        auto recorder = std::make_shared<RecorderNode>();
        const NodeId playerId = graph.adoptNode(player);
        const NodeId recorderId = graph.adoptNode(recorder);
        graph.connect(playerId, recorderId);
        graph.setSink(recorderId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a graph with a MIDI source compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(recorder->received.size() == 2, "the note's on and off both arrive");
        if (recorder->received.size() == 2) {
            check(recorder->received[0].isNoteOn() &&
                      recorder->received[0].frameOffset == 24,
                  "the note-on lands on the sample the beat falls on");
            check(recorder->received[1].isNoteOff() &&
                      recorder->received[1].frameOffset == 72,
                  "and the note-off at the end of its length");
        }

        // A note still sounding when the transport stops has to be released, or
        // the synth holds it forever — the one MIDI bug every user notices.
        auto held = std::make_shared<MidiClipPlayerNode::NoteList>();
        held->push_back(MidiNote{0.0, 100.0, 64, 99, 0});   // far longer than a block
        player->setNotes(held);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOn(),
              "a long note starts");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.001));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOff(),
              "and is released when the transport stops, not left hanging");

        // Same for a locate: what was sounding belonged to where the playhead
        // was, not to where it has jumped to.
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(!recorder->received.empty() && recorder->received[0].isNoteOn(),
              "it starts again from the top");
        processor.process(output.block(), kBlock, 0, true, true, transportAt(50.0));
        bool sawOff = false;
        bool sawChasedOn = false;
        for (const MidiEvent& event : recorder->received) {
            if (event.isNoteOff()) sawOff = true;
            if (event.isNoteOn() && event.frameOffset == 0)
                sawChasedOn = true;
        }
        check(sawOff, "a jump in the playhead releases what was sounding");
        check(sawChasedOn,
              "starting inside a held note chases it from the new playhead");

        processor.process(output.block(), kBlock, 0, false, true,
                          transportAt(50.0));
        check(recorder->received.size() == 1 &&
                  recorder->received[0].isNoteOff(),
              "a chased note is released when playback stops");

        // ── Live keys ──
        //
        // A key pressed on the piano roll's keyboard or the typing keyboard has
        // nothing to do with the playhead: it must sound with the transport
        // stopped, and it must not be swept up by the release that a stop does
        // to the clip's own notes.
        player->setNotes(std::make_shared<MidiClipPlayerNode::NoteList>());
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.empty(), "nothing sounds by itself when stopped");

        check(player->sendLiveNoteOn(67, 100), "a live note is queued");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOn() &&
                  recorder->received[0].data1 == 67,
              "and reaches the instrument with the transport stopped");

        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.empty(),
              "a stopped transport does not release a live note");

        check(player->sendLiveNoteOff(67), "the key comes up");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(recorder->received.size() == 1 && recorder->received[0].isNoteOff() &&
                  recorder->received[0].data1 == 67,
              "and the note ends");

        // Ordinary live events are bounded and refuse rather than overwrite. A
        // release has a separate coalescing mailbox: once an on was accepted,
        // queue saturation must never make its off disappear.
        check(player->sendLiveNoteOn(71, 100),
              "the live overflow fixture queues its tracked note-on");
        int queued = 1;
        while (player->sendLiveNoteOn(60, 100)) ++queued;
        check(queued > 0 && queued < 512, "the live queue fills and then refuses");
        check(player->sendLiveNoteOff(71),
              "a live note-off survives a full ordinary-event ring");
        processor.process(output.block(), kBlock, 0, false, true, transportAt(0.0));
        check(int(recorder->received.size()) == queued + 1,
              "every queued event and the emergency release arrive once");
        bool key71Held = false;
        for (const MidiEvent& event : recorder->received) {
            if (event.data1 != 71) continue;
            if (event.isNoteOn()) key71Held = true;
            if (event.isNoteOff()) key71Held = false;
        }
        check(!key71Held,
              "the full live queue preserves on-before-off ordering for the key");

        // If live input fills a block entirely with releases, the transport's
        // own stop release has no legal victim to evict. It must remain pending
        // and retry next block rather than being forgotten as though it landed.
        auto overflowHeld =
            std::make_shared<MidiClipPlayerNode::NoteList>();
        constexpr std::uint8_t kTimelineChannel = 15;
        constexpr std::uint8_t kTimelineKey = 126;
        overflowHeld->push_back(
            MidiNote{0.0, 100.0, kTimelineKey, 99, kTimelineChannel});
        player->setNotes(overflowHeld);
        processor.process(output.block(), kBlock, 0, true, true,
                          transportAt(0.0));
        check(recorder->received.size() == 1 &&
                  recorder->received[0].isNoteOn() &&
                  recorder->received[0].data1 == kTimelineKey,
              "the release-overflow fixture starts its timeline note");

        int queuedReleases = 0;
        bool acceptedEveryRelease = true;
        for (int identity = 0;
             queuedReleases < int(kMidiEventsPerBlock); ++identity) {
            const auto channel = std::uint8_t((identity / 128) & 0x0F);
            const auto key = std::uint8_t(identity % 128);
            if (channel == kTimelineChannel && key == kTimelineKey) continue;
            acceptedEveryRelease &= player->sendLiveNoteOff(key, channel);
            ++queuedReleases;
        }
        check(acceptedEveryRelease, "saturated live releases are accepted");
        processor.process(output.block(), kBlock, 0, false, true,
                          transportAt(0.0));
        bool prematureTimelineRelease = false;
        for (const MidiEvent& event : recorder->received) {
            prematureTimelineRelease |=
                event.isNoteOff() && event.channel() == kTimelineChannel &&
                event.data1 == kTimelineKey;
        }
        check(recorder->received.size() == kMidiEventsPerBlock &&
                  !prematureTimelineRelease,
              "an all-release block defers the transport's extra note-off");

        processor.process(output.block(), kBlock, 0, false, true,
                          transportAt(0.0));
        check(recorder->received.size() == 1 &&
                  recorder->received[0].isNoteOff() &&
                  recorder->received[0].channel() == kTimelineChannel &&
                  recorder->received[0].data1 == kTimelineKey,
              "the deferred transport release retries on the next block");

        // A seek deep into a large arrangement must not rescan every earlier
        // note. Only the ancient long note is still active; the interval index
        // rejects the 50k expired notes in logarithmically many subtree checks.
        auto indexed = std::make_shared<MidiClipPlayerNode::NoteList>();
        indexed->reserve(100001);
        indexed->push_back(MidiNote{0.0, 100001.0, 41, 100, 0});
        for (int beat = 1; beat <= 100000; ++beat) {
            indexed->push_back(
                MidiNote{double(beat), 0.1, std::uint8_t(42 + beat % 40),
                         90, 0});
        }
        player->setNotes(indexed);
        processor.process(output.block(), kBlock, 0, true, true,
                          transportAt(50000.5));
        std::size_t chasedLongNotes = 0;
        for (const MidiEvent& event : recorder->received) {
            if (event.isNoteOn() && event.data1 == 41) ++chasedLongNotes;
        }
        check(chasedLongNotes == 1,
              "the indexed chase still finds a long note across 50k expired notes");
        check(player->lastChaseSubtreesVisitedForTest() < 256,
              "the discontinuity chase visits sublinear indexed branches");
    }

    // ── MIDI is compensated by the same amount as the audio ──
    //
    // Without this the notes stay where they were while the audio moves, and an
    // instrument behind a compensated effect plays early by exactly that
    // effect's latency.
    {
        AudioGraph graph;
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        notes->push_back(MidiNote{0.0, 0.002, 64, 99, 0});
        player->setNotes(notes);

        auto recorder = std::make_shared<RecorderNode>();
        const NodeId playerId = graph.adoptNode(player);
        const NodeId recorderId = graph.adoptNode(recorder);
        // A second path into the recorder, through a latency, forces the
        // compiler to delay the direct one to match.
        const NodeId slow = graph.addNode(std::make_unique<LatencyNode>("slow", 32));
        graph.connect(playerId, recorderId);
        graph.connect(playerId, slow);
        graph.connect(slow, recorderId);
        graph.setSink(recorderId);

        auto compiled = graph.compile(info);
        check(compiled.has_value() && !(*compiled)->delays.empty(),
              "the early path gets a compensation delay");
        check((*compiled)->midiDelays.size() == (*compiled)->delays.size(),
              "and a MIDI delay to go with it");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));

        // The note leaves the player at offset 0 and arrives at 32, because the
        // direct edge was the early one and got the compensating delay.
        //
        // It arrives exactly once, not twice: MIDI is *not* passed through a
        // node automatically. `LatencyNode` writes audio and nothing else, so
        // the second path carries no notes. That is the rule — a node forwards
        // MIDI only if it chooses to — and it is what makes an instrument in an
        // insert chain the thing that consumes the notes rather than the first
        // effect that happens to be in front of it.
        std::size_t noteOns = 0;
        FrameCount arrivedAt = 0;
        for (const MidiEvent& event : recorder->received) {
            if (!event.isNoteOn()) continue;
            ++noteOns;
            arrivedAt = event.frameOffset;
        }
        check(noteOns == 1, "a node that writes no MIDI does not forward any either");
        check(arrivedAt == 32,
              "and the compensated edge shifts its notes by the same 32 samples as its audio");

        PrepareInfo changedRate = info;
        changedRate.sampleRate = 44100.0;
        auto rateChanged = graph.compile(changedRate, compiled->get());
        check(rateChanged.has_value() && !(*rateChanged)->midiDelays.empty() &&
                  (*rateChanged)->delays.front() != (*compiled)->delays.front() &&
                  (*rateChanged)->midiDelays.front() !=
                      (*compiled)->midiDelays.front(),
              "sample-rate changes discard both audio and MIDI PDC history");
    }

    // Pending compensation events belong to the playhead that queued them. A
    // locate/loop discontinuity must discard them before they surface several
    // blocks later at the new position.
    {
        AudioGraph graph;
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto recorder = std::make_shared<RecorderNode>();
        const NodeId playerId = graph.adoptNode(player);
        const NodeId recorderId = graph.adoptNode(recorder);
        const NodeId slow = graph.addNode(
            std::make_unique<LatencyNode>("long slow path", kBlock * 4));
        graph.connect(playerId, recorderId);
        graph.connect(playerId, slow);
        graph.connect(slow, recorderId);
        graph.setSink(recorderId);
        auto compiled = graph.compile(info);
        check(compiled.has_value() && !(*compiled)->midiDelays.empty(),
              "the seek fixture has pending MIDI compensation state");
        if (compiled) {
            GraphProcessor processor(2);
            processor.setGraph(*compiled);
            OutputBuffer output(2, kBlock);
            player->sendLiveNoteOn(75, 100);
            processor.process(output.block(), kBlock, 0, false, false,
                              transportAt(0.0));
            check(recorder->received.empty(),
                  "a long-PDC live event starts out pending");

            bool staleAfterSeek = false;
            for (int block = 0; block < 5; ++block) {
                processor.process(output.block(), kBlock, 10000, false, false,
                                  transportAt(10.0));
                for (const MidiEvent& event : recorder->received) {
                    if (event.isNoteOn() && event.data1 == 75) staleAfterSeek = true;
                }
            }
            check(!staleAfterSeek,
                  "a seek clears MIDI queued by the previous PDC timeline");
        }
    }

    // Offline rendering borrows the live graph, but its PDC queues are separate
    // render state in time: neither direction may inherit pending MIDI.
    {
        auto buildFixture = [&](RealtimeEngine& engine,
                                const std::shared_ptr<ModeMidiSourceNode>& source,
                                const std::shared_ptr<CountingMidiSinkNode>& sink) {
            engine.prepare(48000.0, kBlock, 2);
            const NodeId sourceId = engine.graph().adoptNode(source);
            const NodeId sinkId = engine.graph().adoptNode(sink);
            const NodeId slow = engine.graph().addNode(
                std::make_unique<LatencyNode>("offline long PDC", kBlock * 4));
            engine.graph().connect(sourceId, sinkId);
            engine.graph().connect(sourceId, slow);
            engine.graph().connect(slow, sinkId);
            engine.graph().setSink(sinkId);
            return bool(engine.commitGraph());
        };

        RealtimeEngine engine(1);
        auto source = std::make_shared<ModeMidiSourceNode>();
        auto sink = std::make_shared<CountingMidiSinkNode>();
        check(buildFixture(engine, source, sink),
              "the offline MIDI-state fixture compiles");
        OutputBuffer output(2, kBlock);

        source->emitNextLive(76);
        engine.renderBlock(output.block(), nullptr, 0, kBlock);
        check(sink->noteOns.load(std::memory_order_relaxed) == 0,
              "the live event is pending before bounce");
        engine.renderOffline(
            0, SamplePos(kBlock) * 5, kBlock,
            [](const AudioBlock&, FrameCount) { return true; });
        check(sink->noteOns.load(std::memory_order_relaxed) == 0,
              "live pending MIDI never leaks into an offline render");

        source->emitNextOffline(77);
        engine.renderOffline(
            0, SamplePos(kBlock), kBlock,
            [](const AudioBlock&, FrameCount) { return true; });
        check(sink->noteOns.load(std::memory_order_relaxed) == 0,
              "the final offline block leaves its event pending only inside bounce");
        for (int block = 0; block < 5; ++block) {
            engine.renderBlock(output.block(), nullptr, 0, kBlock);
        }
        check(sink->noteOns.load(std::memory_order_relaxed) == 0,
              "offline pending MIDI never leaks back into live playback");
    }

    // ── CLAP musical events round-trip through a real plugin ──
    //
    // The fixture echoes its musical input through clap_output_events. This
    // catches both silent host-input drops and silent plugin-output drops while
    // keeping the audio-thread path allocation-free.
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* tone = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (candidate.isInstrument) tone = &candidate;
        }

        auto instance = tone ? factory.create(*tone) : nullptr;
        PluginProcessInfo processInfo;
        processInfo.sampleRate = 48000.0;
        processInfo.maxBlockSize = kBlock;
        const bool ready = instance && instance->activate(processInfo);
        check(ready, "the CLAP event round-trip fixture activates");
        if (ready) {
            instance->startProcessing();

            std::array<PluginEvent, 8> input{};
            input[0].kind = PluginEvent::Kind::NoteOn;
            input[0].frameOffset = 3;
            input[0].channel = 2;
            input[0].key = 61;
            input[0].noteId = 44;
            input[0].value = 0.75;
            input[1] = input[0];
            input[1].kind = PluginEvent::Kind::NoteOff;
            input[1].frameOffset = 14;
            input[1].value = 0.25;
            input[2] = input[0];
            input[2].kind = PluginEvent::Kind::NoteChoke;
            input[2].frameOffset = 22;
            input[2].value = 0.0;

            input[3].kind = PluginEvent::Kind::MidiController;
            input[3].frameOffset = 30;
            input[3].channel = 4;
            input[3].paramIndex = 74;
            input[3].value = 64.0 / 127.0;
            input[4] = input[3];
            input[4].frameOffset = 40;
            input[4].paramIndex = 128;
            input[4].value = 50.0 / 127.0;
            input[5] = input[3];
            input[5].frameOffset = 50;
            input[5].paramIndex = 129;
            input[5].value = 8192.0 / 16383.0;
            input[6] = input[3];
            input[6].frameOffset = 60;
            input[6].paramIndex = 130;
            input[6].value = 9.0 / 127.0;

            input[7].kind = PluginEvent::Kind::PolyPressure;
            input[7].frameOffset = 70;
            input[7].channel = 5;
            input[7].key = 67;
            input[7].noteId = 77;
            input[7].value = 80.0 / 127.0;

            PluginEventCapture capture;
            OutputBuffer audio(2, kBlock);
            PluginProcessContext context;
            context.outputs = audio.pointers.data();
            context.outputChannels = 2;
            context.frames = kBlock;
            context.inputEvents = input;
            context.outputEvents = &capture;
            instance->process(context);

            check(capture.count == input.size(),
                  "CLAP returns every note, controller and pressure event");
            const auto near = [](double a, double b) {
                return std::fabs(a - b) < 1e-9;
            };
            const bool notesOk =
                capture.count >= 3 &&
                capture.events[0].kind == PluginEvent::Kind::NoteOn &&
                capture.events[0].frameOffset == 3 &&
                capture.events[0].channel == 2 && capture.events[0].key == 61 &&
                capture.events[0].noteId == 44 && near(capture.events[0].value, 0.75) &&
                capture.events[1].kind == PluginEvent::Kind::NoteOff &&
                capture.events[1].frameOffset == 14 &&
                near(capture.events[1].value, 0.25) &&
                capture.events[2].kind == PluginEvent::Kind::NoteChoke &&
                capture.events[2].frameOffset == 22;
            check(notesOk, "CLAP note on/off/choke preserve voice identity and timing");

            const bool controllersOk =
                capture.count >= 7 &&
                capture.events[3].kind == PluginEvent::Kind::MidiController &&
                capture.events[3].paramIndex == 74 && capture.events[3].channel == 4 &&
                near(capture.events[3].value, 64.0 / 127.0) &&
                capture.events[4].kind == PluginEvent::Kind::MidiController &&
                capture.events[4].paramIndex == 128 &&
                near(capture.events[4].value, 50.0 / 127.0) &&
                capture.events[5].kind == PluginEvent::Kind::MidiController &&
                capture.events[5].paramIndex == 129 &&
                near(capture.events[5].value, 8192.0 / 16383.0) &&
                capture.events[6].kind == PluginEvent::Kind::MidiController &&
                capture.events[6].paramIndex == 130 &&
                near(capture.events[6].value, 9.0 / 127.0);
            check(controllersOk,
                  "CLAP raw MIDI preserves CC, channel pressure, pitch bend and program");

            const bool pressureOk =
                capture.count >= 8 &&
                capture.events[7].kind == PluginEvent::Kind::PolyPressure &&
                capture.events[7].frameOffset == 70 &&
                capture.events[7].channel == 5 && capture.events[7].key == 67 &&
                capture.events[7].noteId == 77 &&
                near(capture.events[7].value, 80.0 / 127.0);
            check(pressureOk,
                  "CLAP poly pressure uses a sample-accurate note expression");

            // The old adapter had only 320 note slots even though PluginNode
            // accepts a much denser block. Exercise the shared preallocated
            // scratch above that boundary so a busy arpeggiator cannot lose
            // the tail of its event block.
            std::array<PluginEvent, 400> denseNotes{};
            for (std::size_t index = 0; index < denseNotes.size(); ++index) {
                PluginEvent& note = denseNotes[index];
                note.kind = PluginEvent::Kind::NoteOn;
                note.frameOffset = std::uint32_t(index * kBlock / denseNotes.size());
                note.channel = std::int16_t(index % 16);
                note.key = std::int16_t(36 + index % 72);
                note.noteId = std::int32_t(index);
                note.value = 0.5;
            }
            capture.count = 0;
            context.inputEvents = denseNotes;
            instance->process(context);
            check(capture.count == denseNotes.size() &&
                      capture.events[capture.count - 1].noteId ==
                          std::int32_t(denseNotes.size() - 1),
                  "CLAP shared scratch keeps a dense 400-note block intact");

            instance->stopProcessing();
            instance->deactivate();
        }
    }

    // ── Audio effects do not swallow MIDI on the way to a later instrument ──
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* effect = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (!candidate.isInstrument) effect = &candidate;
        }
        check(effect != nullptr, "the fixture has an audio effect for MIDI pass-through");
        if (effect) {
            auto player = std::make_shared<MidiClipPlayerNode>();
            auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
            notes->push_back(MidiNote{0.0, 0.25, 60, 100, 0});
            player->setNotes(notes);
            auto fx = std::make_shared<PluginNode>("audio fx", factory.create(*effect));
            auto recorder = std::make_shared<RecorderNode>();

            AudioGraph graph;
            const NodeId playerId = graph.adoptNode(player);
            const NodeId fxId = graph.adoptNode(fx);
            const NodeId recorderId = graph.adoptNode(recorder);
            graph.connect(playerId, fxId);
            graph.connect(fxId, recorderId);
            graph.setSink(recorderId);
            auto compiled = graph.compile(info);
            check(compiled.has_value(), "a MIDI clip → audio effect chain compiles");
            if (compiled) {
                GraphProcessor processor(2);
                processor.setGraph(*compiled);
                OutputBuffer output(2, kBlock);
                processor.process(output.block(), kBlock, 0, true, true,
                                  transportAt(0.0));
                bool received = false;
                for (const MidiEvent& event : recorder->received) {
                    if (event.isNoteOn()) received = true;
                }
                check(received,
                      "an audio-only plugin passes MIDI to a downstream VSTi");
            }
        }
    }

    // A project must keep playing through a missing plugin placeholder. The
    // audio path already behaved as a bypass; MIDI must follow the same rule or
    // a downstream instrument goes silent while the plugin is unavailable.
    {
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        notes->push_back(MidiNote{0.0, 0.25, 67, 100, 0});
        player->setNotes(notes);
        auto missing = std::make_shared<PluginNode>("missing", nullptr);
        auto recorder = std::make_shared<RecorderNode>();

        AudioGraph graph;
        const NodeId playerId = graph.adoptNode(player);
        const NodeId missingId = graph.adoptNode(missing);
        const NodeId recorderId = graph.adoptNode(recorder);
        graph.connect(playerId, missingId);
        graph.connect(missingId, recorderId);
        graph.setSink(recorderId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a missing-plugin MIDI route compiles");
        if (compiled) {
            GraphProcessor processor(2);
            processor.setGraph(*compiled);
            OutputBuffer output(2, kBlock);
            processor.process(output.block(), kBlock, 0, true, true,
                              transportAt(0.0));
            check(!recorder->received.empty() &&
                      recorder->received.front().isNoteOn(),
                  "a missing plugin placeholder remains transparent to MIDI");
        }
    }

    // ── A clip into an instrument, end to end ──
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* tone = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (candidate.isInstrument) tone = &candidate;
        }
        check(tone != nullptr, "the fixture's instrument is scanned as one");
        if (!tone) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        auto instance = factory.create(*tone);
        check(instance != nullptr, "the instrument instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        check(instance->descriptor().mainInputChannels == 0,
              "the live instance reports no audio input, so the graph treats it as a source");

        auto synth = std::make_shared<PluginNode>("tone", std::move(instance));
        auto player = std::make_shared<MidiClipPlayerNode>();
        auto notes = std::make_shared<MidiClipPlayerNode::NoteList>();
        // Starts at the top of the block, lasts well past its end.
        notes->push_back(MidiNote{0.0, 1.0, 64, 127, 0});
        player->setNotes(notes);

        AudioGraph graph;
        const NodeId playerId = graph.adoptNode(player);
        const NodeId synthId = graph.adoptNode(synth);
        graph.connect(playerId, synthId);
        graph.setSink(synthId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "a clip-into-instrument graph compiles");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);
        for (float& sample : output.storage) sample = -5.0f;   // stale contents
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));

        // Velocity 127 arrives at the plugin as 1.0, and the fixture holds it
        // as a DC level for as long as the note is on.
        check(std::fabs(output.storage[kBlock - 1] - 1.0f) < 1e-3f,
              "a note in a clip makes the instrument sound");
        check(std::fabs(output.storage[0] - 1.0f) < 1e-3f,
              "from the first sample of the block the note starts on");

        // And it stops when the note does.
        auto shortNote = std::make_shared<MidiClipPlayerNode::NoteList>();
        shortNote->push_back(MidiNote{0.0, 0.001, 64, 127, 0});   // ends at sample 24
        player->setNotes(shortNote);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        check(std::fabs(output.storage[kBlock - 1]) < 1e-3f,
              "and falls silent again when the note ends");
    }

    // ── Automation drives a plugin parameter ──
    //
    // The curve is evaluated on the audio thread, against this block's
    // playhead, rather than pushed from the control thread — a curve is a
    // function of the playhead, and only `process` knows it to the sample.
    // Pushing it through the event ring would quantise every automated
    // parameter to the UI's 33 ms tick.
    {
        ClapFactory factory;
        const std::vector<PluginDescriptor> found = factory.inspect(DAW_TEST_CLAP_PATH);
        const PluginDescriptor* gain = nullptr;
        for (const PluginDescriptor& candidate : found) {
            if (!candidate.isInstrument) gain = &candidate;
        }
        check(gain != nullptr, "the fixture's effect is there to automate");
        if (!gain) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }

        auto instance = factory.create(*gain);
        check(instance != nullptr, "it instantiates");
        if (!instance) {
            std::printf("\nFAILURES PRESENT\n");
            return 1;
        }
        // Parameter 0 is Gain, 0…2. The plugin has 64 samples of latency, so
        // what comes out at sample i was multiplied by the gain in force at
        // i - 64 — which is exactly what makes the timing assertion below mean
        // something.
        auto node = std::make_shared<PluginNode>("gain", std::move(instance));

        auto curves = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve curve;
        curve.parameterIndex = 0;
        curve.defaultValue = 1.0;
        // A step, which needs three points: two at the same level make the
        // segment flat, and the third is the jump. Two points alone would be a
        // ramp — the curve is interpolated, not held.
        curve.points.emplace_back(0.0, 0.25);
        curve.points.emplace_back(0.999, 0.25);
        curve.points.emplace_back(1.0, 1.0);
        curves->push_back(curve);
        node->setAutomation(curves);

        AudioGraph graph;
        const NodeId sourceId = graph.addNode(std::make_unique<SourceNode>(
            "src",
            [](void*, const AudioBlock& output, FrameCount frames, SamplePos) {
                for (ChannelCount ch = 0; ch < output.numChannels(); ++ch) {
                    float* data = output.data(ch);
                    for (FrameCount i = 0; i < frames; ++i) data[i] = 1.0f;
                }
            },
            nullptr));
        const NodeId gainId = graph.adoptNode(node);
        graph.connect(sourceId, gainId);
        graph.setSink(gainId);
        auto compiled = graph.compile(info);
        check(compiled.has_value(), "an automated plugin compiles into the graph");

        GraphProcessor processor(4);
        processor.setGraph(*compiled);
        OutputBuffer output(2, kBlock);

        // Two blocks: the first fills the plugin's 64-sample delay line, the
        // second carries signal that has been through the gain.
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        processor.process(output.block(), kBlock, kBlock, true, true,
                          transportAt(double(kBlock) / 24000.0));
        check(std::fabs(output.storage[kBlock - 1] - 0.25f) < 1e-3f,
              "the curve's value at the playhead is what the plugin applies");

        // Beat 1 is sample 24000. Run up to it, then straddle it: the step has
        // to be applied *inside* the block it falls in, not deferred to the
        // next one. The plugin's 64 samples of latency mean the change shows up
        // 64 samples later in the output than in the gain, which is why both
        // values have to appear across two blocks rather than one.
        const SamplePos across = 24000 - 32;
        for (SamplePos at = 0; at < across; at += kBlock) {
            processor.process(output.block(), kBlock, at, true, true,
                              transportAt(double(at) / 24000.0));
        }
        bool sawLow = false;
        bool sawHigh = false;
        for (SamplePos at = across; at < across + 3 * kBlock; at += kBlock) {
            processor.process(output.block(), kBlock, at, true, true,
                              transportAt(double(at) / 24000.0));
            for (FrameCount i = 0; i < kBlock; ++i) {
                if (std::fabs(output.storage[i] - 0.25f) < 1e-3f) sawLow = true;
                if (std::fabs(output.storage[i] - 1.0f) < 1e-3f) sawHigh = true;
            }
        }
        check(sawLow && sawHigh,
              "a breakpoint inside a block is applied inside that block, not at its edge");

        // A curve with no points holds its default rather than jumping to zero.
        auto flat = std::make_shared<PluginNode::AutomationCurves>();
        PluginNode::AutomationCurve empty;
        empty.parameterIndex = 0;
        empty.defaultValue = 2.0;
        flat->push_back(empty);
        node->setAutomation(flat);
        processor.process(output.block(), kBlock, 0, true, true, transportAt(0.0));
        processor.process(output.block(), kBlock, kBlock, true, true,
                          transportAt(double(kBlock) / 24000.0));
        check(std::fabs(output.storage[kBlock - 1] - 2.0f) < 1e-3f,
              "a curve with no breakpoints holds its default value");
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
