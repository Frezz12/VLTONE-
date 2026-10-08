#include "EngineController.hpp"
#include "Host/PitchDelivery.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "MidiRecording.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
#include "ProjectSerializer.hpp"
#include "SlideJson.hpp"
#include "SlideNotes.hpp"
#include <cmath>
#include <cstdio>
#include <numbers>
using namespace daw;
using namespace daw::plugins;
static int failures = 0;
static void check(bool ok, const char *name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static bool close(double a, double b, double tolerance = .00001) {
    return std::abs(a - b) <= tolerance;
}
static void useSyntheticDecoder() {
    sampler::setSampleDecoder([](const std::string &) {
        auto sample = std::make_shared<engine::SampleBuffer>(1, 96000, 48000);
        for (unsigned i = 0; i < 96000; ++i)
            sample->writableChannel(0)[i] = float(i / 96000.);
        return sample;
    });
}
int main() {
    NoteModel base;
    base.id = newUuid();
    base.pitch = 60;
    base.startBeats = 0;
    base.lengthBeats = 8;
    NoteModel other = base;
    other.id = newUuid();
    other.pitch = 64;
    auto up = slides::create({base, other}, 1, 2, 72, {base.id}, true);
    check(up.targetNoteIds.size() == 1, "Chord uses the selected active voices");
    up = slides::create({base, other}, 1, 2, 72, {}, true);
    check(up.referenceNoteId == other.id && up.targetNoteIds.size() == 2,
          "Nearest sounding anchor and fixed chord set");
    up = slides::create({base, other}, 1, 2, 72, {base.id, other.id}, true);
    up.referenceNoteId = base.id;
    auto a = slides::compile({base, other}, {up});
    check(close(engine::curve::valueAt(a[base.id], 3, 0), 12) &&
              close(engine::curve::valueAt(a[other.id], 4, 0), 12),
          "Chord preserves intervals and holds its final pitch");
    auto down = slides::create({base}, 2, 1, 55, {base.id}, false);
    down.points.front().curve = .6;
    auto both = slides::compile({base}, {up, down});
    bool history = true;
    for (double beat = 1; beat < 2; beat += .001)
        history &= close(engine::curve::valueAt(a[base.id], beat, 0),
                         engine::curve::valueAt(both[base.id], beat, 0));
    check(history, "Overlapping slide does not rewrite the preceding S-curve");
    check(close(engine::curve::valueAt(both[base.id], 2, 0), 6) &&
              close(engine::curve::valueAt(both[base.id], 4, 0), -5),
          "Takeover is continuous and the replaced slide never resumes");
    auto orphan = up;
    orphan.referenceNoteId = newUuid();
    check(slides::compile({base}, {orphan}).empty(), "Orphan never creates a sounding note");
    check(slides::fromJson(slides::toJson(up)) == up, "Slide JSON round trip");
    MidiPerformance phrase{{base}, {}, {up, down}};
    auto fragment = sliceMidiPerformance(phrase, 2.2, 5, true);
    auto cut = slides::compile(fragment.notes, fragment.slideNotes);
    bool trajectory = true;
    for (double beat = 2.2; beat < 5; beat += .003)
        trajectory &=
            close(engine::curve::valueAt(both[base.id], beat, 0),
                  engine::curve::valueAt(cut[fragment.notes.front().id], beat - 2.2, 0), .01);
    check(trajectory, "Cut through a curved slide retains the reached pitch within one cent");
    auto cloneNotes = phrase.notes;
    auto cloneSlides = phrase.slideNotes;
    slides::reidentify(cloneNotes, cloneSlides);
    check(cloneNotes[0].id != base.id && cloneSlides[0].referenceNoteId == cloneNotes[0].id,
          "Copy remaps slide references with note identities");
    {
        ProjectModel model;
        TrackModel track;
        track.id = newUuid();
        track.kind = TrackKind::Midi;
        ClipModel clip;
        clip.id = newUuid();
        clip.kind = ClipKind::Midi;
        clip.notes = {base, other};
        clip.slideNotes = {up, down};
        TakeModel take;
        take.id = newUuid();
        take.notes = clip.notes;
        take.slideNotes = clip.slideNotes;
        slides::reidentify(take.notes, take.slideNotes);
        clip.takes = {take};
        track.clips = {clip};
        track.instrument.format = PluginFormat::Internal;
        track.instrument.uid = "daw.sampler";
        track.instrument.slideDelivery = 2;
        track.instrument.slideBendRange = 48;
        model.tracks = {track};
        std::string saved;
        ProjectModel loaded;
        auto serialized = ProjectSerializer::serializeDocument(model, saved);
        auto restored = ProjectSerializer::deserializeDocument(loaded, saved);
        check(serialized && restored && loaded.tracks[0].clips[0].slideNotes == clip.slideNotes &&
                  loaded.tracks[0].clips[0].takes[0].slideNotes == take.slideNotes &&
                  loaded.tracks[0].instrument.slideBendRange == 48,
              "Project format 11 preserves clip/take slides and delivery settings");
    }
    engine::MidiNote note;
    note.startBeats = 0;
    note.lengthBeats = 8;
    note.noteId = 42;
    note.pitch = both[base.id];
    for (unsigned block : {64, 128, 512, 8192}) {
        engine::MidiClipPlayerNode player;
        player.prepare({48000, block, 2});
        player.setNotes(std::make_shared<const engine::MidiClipPlayerNode::NoteList>(
            engine::MidiClipPlayerNode::NoteList{note}));
        engine::MidiBuffer buffer;
        buffer.reserve(engine::pitchEventCapacity(block, 48000));
        bool accurate = true;
        int ons = 0;
        engine::PitchRamp current;
        std::uint64_t since = 0;
        for (unsigned cursor = 0; cursor < 96000; cursor += block) {
            engine::ProcessContext ctx;
            ctx.frames = std::min(block, 96000 - cursor);
            ctx.playing = true;
            ctx.sampleRate = 48000;
            ctx.transport.tempo = 120;
            ctx.transport.ppqPosition = cursor / 24000.;
            ctx.midiOutput = &buffer;
            buffer.clear();
            player.process(ctx);
            auto events = buffer.events();
            std::size_t i = 0;
            for (unsigned frame = 0; frame < ctx.frames; ++frame) {
                while (i < events.size() && events[i].frameOffset == frame) {
                    if (events[i].isNoteOn())
                        ++ons;
                    if (events[i].isPitchExpression) {
                        current = events[i].pitch;
                        since = 0;
                    }
                    ++i;
                }
                accurate &=
                    close(current.at(std::uint32_t(since++)),
                          engine::curve::valueAt(note.pitch, (cursor + frame) / 24000., 0), .01);
            }
        }
        check(accurate && ons == 1,
              "Sample-accurate trajectory across buffer sizes without retrigger");
        auto edited = note;
        edited.pitch.back().value = 4;
        player.setNotes(std::make_shared<const engine::MidiClipPlayerNode::NoteList>(
            engine::MidiClipPlayerNode::NoteList{edited}));
        buffer.clear();
        engine::ProcessContext ctx;
        ctx.frames = block;
        ctx.playing = true;
        ctx.transport.tempo = 120;
        ctx.transport.ppqPosition = 4;
        ctx.midiOutput = &buffer;
        player.process(ctx);
        check(std::none_of(buffer.events().begin(), buffer.events().end(),
                           [](const auto &e) { return e.isNoteOn() || e.isNoteOff(); }),
              "Curve edit preserves the sounding instance");
    }
    for (unsigned block : {64, 128, 512, 8192}) {
        const double tempo = 137.23, spb = 48000 * 60 / tempo;
        engine::MidiNote n;
        n.noteId = 90;
        n.lengthBeats = 2;
        n.pitch = {{0, 0},
                   {.001319, 11},
                   {.003143, -3},
                   {.174195, 7, engine::curve::Shape::SCurve, .8},
                   {.349153, -12}};
        engine::MidiClipPlayerNode player;
        player.prepare({48000, block, 2});
        player.setNotes(std::make_shared<const engine::MidiClipPlayerNode::NoteList>(
            engine::MidiClipPlayerNode::NoteList{n}));
        engine::MidiBuffer buffer;
        buffer.reserve(engine::pitchEventCapacity(block, 48000));
        engine::PitchRamp ramp;
        unsigned phase = 0;
        double error = 0;
        for (unsigned start = 0; start < 20000; start += block) {
            engine::ProcessContext context;
            context.frames = std::min(block, 20000 - start);
            context.sampleRate = 48000;
            context.playing = true;
            context.transport.tempo = tempo;
            context.transport.ppqPosition = start / spb;
            context.midiOutput = &buffer;
            buffer.clear();
            player.process(context);
            std::size_t next = 0;
            for (unsigned frame = 0; frame < context.frames; ++frame) {
                while (next < buffer.events().size() &&
                       buffer.events()[next].frameOffset == frame) {
                    if (buffer.events()[next].isPitchExpression) {
                        ramp = buffer.events()[next].pitch;
                        phase = 0;
                    }
                    ++next;
                }
                error = std::max(
                    error, std::abs(ramp.at(phase++) -
                                    engine::curve::valueAt(n.pitch, (start + frame) / spb, 0)));
            }
        }
        check(error < .00001, "Fractional tempo and off-grid knots produce the same sample "
                              "trajectory at every buffer size");
    }
    PitchDelivery delivery;
    delivery.prepare(engine::pitchEventCapacity(8192, 48000));
    std::vector<PluginEvent> chord;
    for (int i = 0; i < 16; ++i) {
        PluginEvent e;
        e.kind = PluginEvent::Kind::NoteOn;
        e.key = 60;
        e.channel = 0;
        e.noteId = i;
        e.value = 1;
        chord.push_back(e);
    }
    auto mpe = delivery.process(chord, 128, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
    int count = 0;
    std::array<bool, 16> used{};
    for (auto &e : mpe)
        if (e.kind == PluginEvent::Kind::NoteOn) {
            ++count;
            used[e.channel] = true;
        }
    check(count == 15 && !used[0] && delivery.overloaded,
          "MPE uses channels 2–16 and skips overload without stealing voices");
    PluginEvent off;
    off.kind = PluginEvent::Kind::NoteOff;
    off.noteId = 0;
    off.channel = 0;
    off.key = 60;
    delivery.process({&off, 1}, 128, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
    PluginEvent on = chord.front();
    on.noteId = 40;
    auto held =
        delivery.process({&on, 1}, 128, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
    check(std::none_of(held.begin(), held.end(),
                       [](const auto &e) { return e.kind == PluginEvent::Kind::NoteOn; }),
          "MPE reserves a releasing voice's channel");
    delivery.reset();
    PluginEvent pitch;
    pitch.kind = PluginEvent::Kind::NotePitch;
    pitch.noteId = 0;
    pitch.channel = 0;
    pitch.key = 60;
    pitch.pitch.from = pitch.pitch.to = 1;
    pitch.pitch.active = true;
    PluginEvent bend;
    bend.kind = PluginEvent::Kind::MidiController;
    bend.paramIndex = 129;
    bend.channel = 0;
    bend.value = (8192. + 4095.5) / 16383.;
    std::vector<PluginEvent> sum{chord[0], bend, pitch};
    auto output = delivery.process(sum, 64, 48000, SlideDelivery::PitchBend, 2, 2, 0, false);
    double last = 0;
    for (const auto &e : output)
        if (e.kind == PluginEvent::Kind::MidiController && e.channel == 0 && e.paramIndex == 129)
            last = e.value;
    check(close(last, 1), "Slide plus channel automation is summed exactly once in semitones");
    check(PitchDelivery::resolve(SlideDelivery::Auto, {}) == SlideDelivery::Off &&
              PitchDelivery::resolve(SlideDelivery::Auto, {true, false, false, false}) ==
                  SlideDelivery::NoteExpression,
          "Auto uses declared capabilities");
    // Measure actual resampled audio, including the one-shot release after Note
    // Off. A ramp-valued source exposes its read position without adding any
    // test-only DSP API.
    useSyntheticDecoder();
    for (unsigned block : {64, 128, 512, 8192}) {
        sampler::SamplerInstance instrument;
        PluginProcessInfo info;
        info.sampleRate = 48000;
        info.maxBlockSize = 8192;
        instrument.activate(info);
        instrument.startProcessing();
        instrument.loadSample("/synthetic/slide-ramp.wav");
        engine::MidiNote n;
        n.noteId = 100;
        n.key = 60;
        n.velocity = 127;
        n.lengthBeats = .25;
        n.pitch = {{0, 0}, {.05, 7}, {.12, -5}, {.20, 12}};
        engine::MidiClipPlayerNode player;
        player.prepare({48000, block, 2});
        player.setNotes(std::make_shared<const engine::MidiClipPlayerNode::NoteList>(
            engine::MidiClipPlayerNode::NoteList{n}));
        engine::MidiBuffer midi;
        midi.reserve(engine::pitchEventCapacity(block, 48000));
        std::vector<PluginEvent> events;
        events.reserve(engine::pitchEventCapacity(block, 48000));
        std::vector<float> left(block), right(block);
        float *channels[]{left.data(), right.data()};
        double source = 0, maxError = 0;
        for (unsigned cursor = 0; cursor < 24000; cursor += block) {
            unsigned frames = std::min(block, 24000 - cursor);
            engine::ProcessContext scheduler;
            scheduler.frames = frames;
            scheduler.playing = true;
            scheduler.sampleRate = 48000;
            scheduler.transport.tempo = 120;
            scheduler.transport.ppqPosition = cursor / 24000.;
            scheduler.midiOutput = &midi;
            midi.clear();
            player.process(scheduler);
            events.clear();
            for (const auto &input : midi.events()) {
                PluginEvent event;
                event.noteId = input.noteId;
                event.frameOffset = input.frameOffset;
                event.channel = input.channel();
                event.key = input.data1;
                if (input.isPitchExpression) {
                    event.kind = PluginEvent::Kind::NotePitch;
                    event.pitch = input.pitch;
                } else if (input.isNoteOn()) {
                    event.kind = PluginEvent::Kind::NoteOn;
                    event.value = input.data2 / 127.;
                } else if (input.isNoteOff())
                    event.kind = PluginEvent::Kind::NoteOff;
                else
                    continue;
                events.push_back(event);
            }
            PluginProcessContext ctx;
            ctx.outputs = channels;
            ctx.outputChannels = 2;
            ctx.frames = frames;
            ctx.inputEvents = events;
            ctx.playing = true;
            instrument.process(ctx);
            for (unsigned i = 0; i < frames; ++i) {
                maxError = std::max(maxError, std::abs(double(left[i]) - source / 96000.));
                double beat = std::min(.25, (cursor + i) / 24000.);
                source += std::exp2(engine::curve::valueAt(n.pitch, beat, 0) / 12.);
            }
        }
        check(maxError < .000002,
              "Sampler audio keeps source position through a wave and holds its "
              "release pitch at every buffer size");
    }
    // Legato changes read speed of one voice; it does not add another attack or
    // restart the file.
    {
        sampler::SamplerInstance instrument;
        PluginProcessInfo info;
        info.sampleRate = 48000;
        info.maxBlockSize = 8192;
        instrument.activate(info);
        instrument.startProcessing();
        instrument.loadSample("/synthetic/legato.wav");
        instrument.setParameter(sampler::indexOf(sampler::SlideParam::Legato), 1);
        instrument.setParameter(sampler::indexOf(sampler::SlideParam::TimeMs), 0);
        auto run = [&](PluginEvent event) {
            std::vector<float> l(512), r(512);
            float *channels[]{l.data(), r.data()};
            PluginProcessContext ctx;
            ctx.frames = 512;
            ctx.outputs = channels;
            ctx.outputChannels = 2;
            ctx.inputEvents = {&event, 1};
            instrument.process(ctx);
            return l;
        };
        PluginEvent first;
        first.kind = PluginEvent::Kind::NoteOn;
        first.key = 60;
        first.noteId = 1;
        first.value = 1;
        run(first);
        auto second = first;
        second.key = 72;
        second.noteId = 2;
        auto high = run(second);
        second.kind = PluginEvent::Kind::NoteOff;
        auto returned = run(second);
        check(close(high[0], 512. / 96000., .000002) && close(high[511], 1534. / 96000., .000002) &&
                  close(returned[0], 1536. / 96000., .000002) &&
                  close(returned[511], 2047. / 96000., .000002),
              "Legato reuses its voice and returns to the previous held note");
    }

    {
        auto muted = up;
        muted.muted = true;
        auto fragment = sliceMidiPerformance({{base}, {}, {muted}}, 1.5, 4, true);
        check(!fragment.slideNotes.empty() && fragment.slideNotes[0].muted &&
                  fragment.slideNotes[0].referenceNoteId == fragment.notes[0].id,
              "Cut preserves muted slide data and remaps its linked note");
        auto changed = base;
        changed.startBeats = 2;
        changed.lengthBeats = 16;
        changed.pitch += 3;
        auto followed = std::vector<SlideNoteModel>{up};
        slides::followNotes(followed, {base}, {changed}, true);
        check(close(followed[0].startBeats, 4) && close(followed[0].lengthBeats, 4) &&
                  close(followed[0].points.back().value, up.points.back().value + 3),
              "Stretch and transpose preserve the relative slide trajectory");
        auto drawn = slides::create({base}, 0, 2, 72);
        drawn.points.clear();
        for (int i = 0; i <= 1200; ++i) {
            double t = i / 1200.;
            drawn.points.push_back({t, 60 + 5 * std::sin(t * 8 * std::numbers::pi)});
        }
        auto raw = drawn.points;
        slides::normalize(drawn);
        double error = 0;
        for (const auto &point : raw)
            error = std::max(
                error, std::abs(automationValueAt(drawn.points, point.beats, 60) - point.value));
        check(drawn.points.size() <= 256 && error <= .01,
              "Freehand simplification measures cent error against the complete original stroke");
    }
    {
        EngineController c{EngineController::TestRuntime{}};
        c.initialize(48000, 512, false);
        auto track = c.addTrack(TrackKind::Midi, "Slide edits"), id = c.addMidiClip(track, 0, 12);
        c.setClipMidiObjects(track, id, {base}, {up}, "Fixture");
        const auto depth = c.undoDepth();
        c.beginNoteEdit(track, id);
        auto longer = base;
        longer.lengthBeats *= 2;
        c.setClipNotes(track, id, {longer}, "Stretch Notes", true);
        c.endNoteEdit("Stretch Notes");
        auto &clip = const_cast<ClipModel &>(c.project().findTrack(track)->clips.front());
        check(close(clip.slideNotes[0].lengthBeats, 4) && close(clip.slideNotes[0].startBeats, 2) &&
                  c.undoDepth() == depth + 1,
              "Interactive stretch changes notes and slide time in one undo action");
        c.undo();
        check(clip.notes == std::vector<NoteModel>{base} &&
                  clip.slideNotes == std::vector<SlideNoteModel>{up},
              "Stretch undo restores both objects");
        c.removeNote(track, id, base.id);
        check(clip.slideNotes.size() == 1 && slides::compile(clip.notes, clip.slideNotes).empty(),
              "Deleting the base retains its inactive slide");
        c.undo();
        check(!slides::compile(clip.notes, clip.slideNotes).empty(),
              "Undoing base deletion reactivates its slide");
        TakeModel one;
        one.id = newUuid();
        one.notes = {base};
        one.slideNotes = {up};
        one.lengthSeconds = 12;
        TakeModel two = one;
        two.id = newUuid();
        slides::reidentify(two.notes, two.slideNotes);
        clip.takes = {one, two};
        clip.comp = {{one.id, 0, 12, newUuid()}};
        auto edited = one.slideNotes;
        edited[0].points.back().value = 75;
        c.setClipSlideNotes(track, id, edited, "Edit take slide");
        clip.comp.front().takeId = two.id;
        c.undo();
        check(clip.takes[0].slideNotes == one.slideNotes &&
                  clip.takes[1].slideNotes == two.slideNotes,
              "Curve undo targets its original take after the active take changes");
    }
    {
        constexpr unsigned frames = 512;
        engine::MidiClipPlayerNode player;
        player.prepare({48000, frames, 2});
        engine::MidiClipPlayerNode::NoteList dense;
        for (int voice = 0; voice < 128; ++voice) {
            engine::MidiNote n;
            n.noteId = voice;
            n.lengthBeats = 8;
            for (unsigned i = 0; i < frames; ++i)
                n.pitch.push_back({i / 24000., double(i % 12)});
            dense.push_back(std::move(n));
        }
        player.setNotes(std::make_shared<const engine::MidiClipPlayerNode::NoteList>(dense));
        engine::MidiBuffer midi;
        midi.reserve(engine::pitchEventCapacity(frames, 48000));
        engine::ProcessContext context;
        context.frames = frames;
        context.playing = true;
        context.sampleRate = 48000;
        context.transport.tempo = 120;
        context.midiOutput = &midi;
        player.process(context);
        check(midi.size() == 128 * (frames + 1),
              "Prepared queues carry 128 dense pitch trajectories without losing nodes");
        engine::MidiDelay delay;
        delay.prepare(700, engine::pitchEventCapacity(1212, 48000));
        engine::MidiBuffer delayed;
        delayed.reserve(engine::pitchEventCapacity(frames, 48000));
        delay.process(midi, delayed, frames);
        midi.clear();
        delay.process(midi, delayed, frames);
        check(delay.droppedPendingEvents() == 0 &&
                  std::any_of(delayed.events().begin(), delayed.events().end(),
                              [](const auto &e) { return e.isPitchExpression && e.noteId == 127; }),
              "Latency compensation retains pitch identity and segment phase");
    }
    {
        engine::MidiClipPlayerNode player;
        player.prepare({48000, 128, 2});
        auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
        auto n = note;
        n.lengthBeats = 8;
        notes->push_back(n);
        player.setNotes(notes);
        engine::MidiBuffer midi;
        midi.reserve(engine::pitchEventCapacity(128, 48000));
        engine::ProcessContext c;
        c.frames = 128;
        c.playing = true;
        c.sampleRate = 48000;
        c.transport.tempo = 120;
        c.midiOutput = &midi;
        bool exact = true;
        for (double beat : {2.25, 1.75, 3.5, 0.0}) {
            c.transport.ppqPosition = beat;
            midi.clear();
            player.process(c);
            auto pitch = std::find_if(midi.events().begin(), midi.events().end(),
                                      [](const auto &e) { return e.isPitchExpression; });
            exact &= pitch != midi.events().end() &&
                     close(pitch->pitch.at(0), engine::curve::valueAt(n.pitch, beat, 0));
        }
        c.playing = false;
        midi.clear();
        player.process(c);
        check(exact && std::any_of(midi.events().begin(), midi.events().end(),
                                   [](const auto &e) { return e.isNoteOff(); }),
              "Seek, loop wrap and stop chase or release the actual reached pitch");
    }
    {
        useSyntheticDecoder();
        sampler::SamplerInstance instrument;
        PluginProcessInfo info;
        info.sampleRate = 48000;
        info.maxBlockSize = 512;
        instrument.activate(info);
        instrument.startProcessing();
        check(instrument.loadSample("/synthetic/legato-curve.wav"), "Legato curve sample loads");
        instrument.setParameter(sampler::indexOf(sampler::SlideParam::Legato), 1);
        instrument.setParameter(sampler::indexOf(sampler::SlideParam::TimeMs), 100);
        instrument.setParameter(sampler::indexOf(sampler::SlideParam::Shape), 0);
        auto run = [&](PluginEvent event, unsigned frames) {
            std::vector<float> l(frames), r(frames);
            float *channels[]{l.data(), r.data()};
            PluginProcessContext context;
            context.frames = frames;
            context.outputs = channels;
            context.outputChannels = 2;
            context.inputEvents = {&event, 1};
            instrument.process(context);
            return l;
        };
        PluginEvent on;
        on.kind = PluginEvent::Kind::NoteOn;
        on.key = 60;
        on.noteId = 1;
        on.value = 1;
        run(on, 512);
        on.key = 72;
        on.noteId = 2;
        run(on, 512);
        double source = 512;
        for (unsigned i = 0; i < 512; ++i)
            source += std::exp2(i / 4800.);
        const double reached = 60 + 12 * 512 / 4800.;
        double error = 0;
        for (unsigned start = 0; start < 512; start += 128) {
            PluginEvent pitch;
            pitch.kind = PluginEvent::Kind::NotePitch;
            pitch.noteId = 2;
            pitch.key = 72;
            pitch.pitch.active = true;
            pitch.pitch.from = 0;
            pitch.pitch.to = 12;
            pitch.pitch.segmentStart = 1;
            pitch.pitch.phaseFrom = start / 512.;
            pitch.pitch.phaseTo = (start + 128) / 512.;
            pitch.pitch.frames = 128;
            auto audio = run(pitch, 128);
            for (unsigned i = 0; i < 128; ++i) {
                error = std::max(error, std::abs(audio[i] - source / 96000.));
                source += std::exp2((reached + (84 - reached) * (start + i) / 512. - 60) / 12.);
            }
        }
        check(error < .000002, "Explicit slide takes over unfinished Legato at the current pitch "
                               "without restarting or a block-boundary jump");
    }
    {
        PitchDelivery d;
        d.prepare(4096);
        PluginEvent first = chord[0];
        first.noteId = 23;
        PluginEvent p;
        p.kind = PluginEvent::Kind::NotePitch;
        p.noteId = 23;
        p.key = 60;
        p.pitch.active = true;
        p.pitch.from = p.pitch.to = 7;
        PluginEvent end = first;
        end.kind = PluginEvent::Kind::NoteOff;
        end.frameOffset = 1;
        std::vector<PluginEvent> events{first, p, end};
        d.process(events, 64, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
        PluginEvent automation;
        automation.kind = PluginEvent::Kind::MidiController;
        automation.paramIndex = 129;
        automation.value = 1;
        auto released =
            d.process({&automation, 1}, 64, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
        check(std::none_of(released.begin(), released.end(),
                           [](const auto &e) {
                               return e.kind == PluginEvent::Kind::MidiController &&
                                      e.paramIndex == 129 && e.channel == 1;
                           }),
              "MPE preserves a releasing voice's bend when channel automation changes");
        d.voiceEnded(-1, 1, 60);
        auto freed =
            d.process({&first, 1}, 64, 48000, SlideDelivery::MPE, 48, 2, 0xffffffffu, false);
        check(std::any_of(freed.begin(), freed.end(),
                          [](const auto &e) {
                              return e.kind == PluginEvent::Kind::NoteOn && e.channel == 1;
                          }),
              "Plugin Note End releases its MPE channel immediately");
    }

    {
        useSyntheticDecoder();
        sampler::SamplerInstance instrument;
        PluginProcessInfo info;
        info.sampleRate = 48000;
        info.maxBlockSize = 128;
        instrument.activate(info);
        instrument.startProcessing();
        check(instrument.loadSample("/synthetic/smoothing.wav"), "Smoothing sample loads");
        auto run = [&](std::span<const PluginEvent> events, unsigned frames) {
            std::vector<float> l(frames), r(frames);
            float *channels[]{l.data(), r.data()};
            PluginProcessContext c;
            c.frames = frames;
            c.outputs = channels;
            c.outputChannels = 2;
            c.inputEvents = events;
            instrument.process(c);
            return l;
        };
        PluginEvent on;
        on.kind = PluginEvent::Kind::NoteOn;
        on.key = 60;
        on.noteId = 1;
        on.value = 1;
        PluginEvent pitch;
        pitch.kind = PluginEvent::Kind::NotePitch;
        pitch.noteId = 1;
        pitch.pitch.active = true;
        pitch.pitch.from = pitch.pitch.to = 11;
        const PluginEvent start[]{on, pitch};
        run(start, 64);
        double source = 64 * std::exp2(11. / 12), error = 0;
        pitch.pitch.from = pitch.pitch.to = 0;
        for (unsigned block = 0; block < 4; ++block) {
            pitch.pitch.edited = block == 0;
            auto audio = run({&pitch, 1}, 32);
            for (unsigned i = 0; i < 32; ++i) {
                error = std::max(error, std::abs(audio[i] - source / 96000.));
                const double correction = 11 * std::max(0., 1 - (block * 32 + i) / 96.);
                source += std::exp2(correction / 12);
            }
        }
        check(error < .000002,
              "Live edit smoothing survives short blocks and ends after exactly two milliseconds");
        instrument.reset();
        auto second = on;
        second.noteId = 2;
        const PluginEvent samePitch[]{on, second};
        run(samePitch, 64);
        pitch.pitch = {};
        pitch.pitch.active = true;
        pitch.pitch.from = pitch.pitch.to = 12;
        auto chordAudio = run({&pitch, 1}, 64);
        check(close(chordAudio.front(), 128. / 96000., .000002) &&
                  close(chordAudio.back(), 317. / 96000., .000002),
              "Equal-pitch sampler voices respond only to their own note identity");
        PitchDelivery adapter;
        adapter.prepare(2048);
        on.noteId = 145;
        auto off = on;
        off.kind = PluginEvent::Kind::NoteOff;
        off.frameOffset = 1;
        const PluginEvent phrase[]{on, off};
        auto out =
            adapter.process(phrase, 64, 48000, SlideDelivery::NoteExpression, 2, 2, 0, false);
        auto started = std::find_if(out.begin(), out.end(), [](const auto &e) {
            return e.kind == PluginEvent::Kind::NoteOn;
        });
        check(started != out.end() && adapter.sourceNoteId(started->noteId) == 145,
              "Native plugin output maps back to the source identity after a same-block release");
    }
    std::printf("%d slide failures\n", failures);
    return failures ? 1 : 0;
}
