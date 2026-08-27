// Standard MIDI File parsing, and the import that lands it in the document.
//
// Every file here is built byte by byte in the test — no fixtures. That way a
// case says exactly what it is about (a running-status run, a 2-byte delta, a
// truncated chunk) instead of hiding it inside a binary nobody can read in a
// diff.
#include "EngineController.hpp"
#include "MidiFile.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mf = daw::midifile;

static int failures = 0;
static bool check(bool condition, const char* what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
    return condition;
}

static bool near(double a, double b, double tolerance = 1e-9) {
    return std::fabs(a - b) < tolerance;
}

namespace {

using Bytes = std::vector<std::uint8_t>;

void append(Bytes& out, const Bytes& more) {
    out.insert(out.end(), more.begin(), more.end());
}

void be16(Bytes& out, int value) {
    out.push_back(std::uint8_t((value >> 8) & 0xFF));
    out.push_back(std::uint8_t(value & 0xFF));
}

void be32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(std::uint8_t((value >> shift) & 0xFF));
}

/// Variable-length quantity, most-significant group first.
void vlq(Bytes& out, std::uint32_t value) {
    std::uint32_t buffer = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80;
        buffer += (value & 0x7F);
    }
    while (true) {
        out.push_back(std::uint8_t(buffer & 0xFF));
        if (buffer & 0x80) buffer >>= 8;
        else break;
    }
}

Bytes header(int format, int tracks, int division, std::uint32_t length = 6) {
    Bytes out{'M', 'T', 'h', 'd'};
    be32(out, length);
    be16(out, format);
    be16(out, tracks);
    be16(out, division);
    // A header longer than six bytes is legal; the extra bytes must be skipped
    // by the declared length rather than assumed away.
    for (std::uint32_t i = 6; i < length; ++i) out.push_back(0);
    return out;
}

Bytes track(const Bytes& events, bool withEndOfTrack = true) {
    Bytes body = events;
    if (withEndOfTrack) {
        vlq(body, 0);
        append(body, {0xFF, 0x2F, 0x00});
    }
    Bytes out{'M', 'T', 'r', 'k'};
    be32(out, std::uint32_t(body.size()));
    append(out, body);
    return out;
}

/// delta, note-on, delta, note-off — the ordinary way to write one note.
void note(Bytes& out, std::uint32_t startDelta, std::uint32_t lengthTicks,
          int pitch, int velocity = 100, int channel = 0) {
    vlq(out, startDelta);
    out.push_back(std::uint8_t(0x90 | channel));
    out.push_back(std::uint8_t(pitch));
    out.push_back(std::uint8_t(velocity));
    vlq(out, lengthTicks);
    out.push_back(std::uint8_t(0x80 | channel));
    out.push_back(std::uint8_t(pitch));
    out.push_back(0x40);
}

void setTempo(Bytes& out, double bpm) {
    vlq(out, 0);
    append(out, {0xFF, 0x51, 0x03});
    const std::uint32_t micros = std::uint32_t(60000000.0 / bpm + 0.5);
    out.push_back(std::uint8_t((micros >> 16) & 0xFF));
    out.push_back(std::uint8_t((micros >> 8) & 0xFF));
    out.push_back(std::uint8_t(micros & 0xFF));
}

void trackName(Bytes& out, const std::string& name) {
    vlq(out, 0);
    append(out, {0xFF, 0x03});
    vlq(out, std::uint32_t(name.size()));
    for (char c : name) out.push_back(std::uint8_t(c));
}

bool parse(const Bytes& bytes, mf::File& out, std::string& error) {
    return mf::parseBytes(bytes.data(), bytes.size(), out, error);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr int kPPQ = 96;

    // ── Format 0: one track, one note ──
    {
        Bytes events;
        note(events, 0, kPPQ, 60);              // a quarter note at beat 0
        note(events, kPPQ, kPPQ / 2, 64);       // an eighth, one beat later
        Bytes file = header(0, 1, kPPQ);
        append(file, track(events));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error), "a format 0 file parses");
        check(parsed.notes.size() == 2, "both notes come through");
        if (parsed.notes.size() == 2) {
            check(parsed.notes[0].pitch == 60 && near(parsed.notes[0].startBeats, 0.0) &&
                      near(parsed.notes[0].lengthBeats, 1.0),
                  "ticks become beats against the division");
            check(parsed.notes[1].pitch == 64 &&
                      near(parsed.notes[1].startBeats, 2.0) &&
                      near(parsed.notes[1].lengthBeats, 0.5),
                  "and a delta is measured from the previous event, not the bar");
            check(parsed.notes[0].velocity == 100,
                  "the note-on's velocity is the note's, not the note-off's");
        }
        check(near(parsed.lengthBeats, 2.5), "the file's length is its last end");
        check(parsed.ticksPerQuarter == kPPQ, "the division is reported");
    }

    // ── Format 1: parallel tracks, each with its own tick origin ──
    {
        Bytes conductor;
        setTempo(conductor, 96.0);
        trackName(conductor, "Conductor");

        Bytes bass;
        trackName(bass, "Bass");
        note(bass, 0, kPPQ, 36);

        Bytes lead;
        trackName(lead, "Lead");
        note(lead, kPPQ * 2, kPPQ, 72);

        Bytes file = header(1, 3, kPPQ);
        append(file, track(conductor));
        append(file, track(bass));
        append(file, track(lead));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error), "a format 1 file parses");
        check(parsed.notes.size() == 2, "notes from every track are merged");
        check(parsed.trackNames.size() == 3 && parsed.trackNames[0] == "Conductor" &&
                  parsed.trackNames[2] == "Lead",
              "track names are kept, in file order");
        if (parsed.notes.size() == 2) {
            check(parsed.notes[0].pitch == 36 && parsed.notes[0].track == 1,
                  "each note remembers which track it came from");
            check(near(parsed.notes[1].startBeats, 2.0) && parsed.notes[1].track == 2,
                  "a track's ticks are its own, not a continuation of the last");
        }
        check(parsed.tracksWithNotes() == 2,
              "a conductor track carrying only tempo counts for nothing");
        check(near(parsed.firstTempoBpm, 96.0, 0.01) && !parsed.hasTempoChanges,
              "the tempo is reported");
    }

    // ── Running status, and a note-on with velocity 0 as the note-off ──
    {
        Bytes events;
        vlq(events, 0);
        events.insert(events.end(), {0x90, 60, 100});   // status given once
        vlq(events, kPPQ);
        events.insert(events.end(), {60, 0});           // running status, vel 0 = off
        vlq(events, 0);
        events.insert(events.end(), {62, 100});         // still running: another on
        vlq(events, kPPQ);
        events.insert(events.end(), {62, 0});

        Bytes file = header(0, 1, kPPQ);
        append(file, track(events));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error), "a file using running status parses");
        check(parsed.notes.size() == 2, "running status yields both notes");
        if (parsed.notes.size() == 2) {
            check(near(parsed.notes[0].lengthBeats, 1.0) &&
                      near(parsed.notes[1].startBeats, 1.0),
                  "a note-on with velocity 0 ends the note");
        }
    }

    // ── Two notes open on the same key: the off pops the most recent ──
    {
        Bytes events;
        vlq(events, 0);
        events.insert(events.end(), {0x90, 60, 100});
        vlq(events, kPPQ);
        events.insert(events.end(), {0x90, 60, 80});    // second on, same key
        vlq(events, kPPQ);
        events.insert(events.end(), {0x80, 60, 0});     // ends the second
        vlq(events, kPPQ);
        events.insert(events.end(), {0x80, 60, 0});     // ends the first

        Bytes file = header(0, 1, kPPQ);
        append(file, track(events));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error), "overlapping same-pitch notes parse");
        check(parsed.notes.size() == 2, "and stay two notes");
        // Sorted by start: the long one first.
        if (parsed.notes.size() == 2) {
            check(near(parsed.notes[0].startBeats, 0.0) &&
                      near(parsed.notes[0].lengthBeats, 3.0),
                  "the first note runs to the last note-off");
            check(near(parsed.notes[1].startBeats, 1.0) &&
                      near(parsed.notes[1].lengthBeats, 1.0) &&
                      parsed.notes[1].velocity == 80,
                  "and the inner one is closed by the first note-off (LIFO)");
        }
    }

    // ── Multi-byte deltas, unknown meta, unknown chunk, extra header bytes ──
    {
        Bytes events;
        vlq(events, 0);
        events.insert(events.end(), {0xFF, 0x7F, 0x02, 0xAB, 0xCD});   // unknown meta
        note(events, 300, 200, 67);   // deltas past 127: two VLQ bytes each

        Bytes unknownChunk{'X', 'Y', 'Z', 'Z'};
        be32(unknownChunk, 4);
        append(unknownChunk, {1, 2, 3, 4});

        Bytes file = header(0, 1, kPPQ, /*length=*/8);   // two extra header bytes
        append(file, unknownChunk);
        append(file, track(events));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error),
              "extra header bytes and an unknown chunk are skipped by length");
        check(parsed.notes.size() == 1 &&
                  near(parsed.notes[0].startBeats, 300.0 / kPPQ) &&
                  near(parsed.notes[0].lengthBeats, 200.0 / kPPQ),
              "an unknown meta event is skipped and multi-byte deltas decode");
    }

    // ── A note still held when the track ends ──
    {
        Bytes events;
        vlq(events, 0);
        events.insert(events.end(), {0x90, 60, 100});
        vlq(events, kPPQ * 2);
        events.insert(events.end(), {0x90, 62, 100});   // no note-offs at all

        Bytes file = header(0, 1, kPPQ);
        append(file, track(events));

        mf::File parsed;
        std::string error;
        check(parse(file, parsed, error), "a file missing its note-offs parses");
        check(parsed.notes.size() == 2, "the held notes are still notes");
        if (parsed.notes.size() == 2) {
            check(parsed.notes[0].lengthBeats > 0.0 &&
                      parsed.notes[1].lengthBeats > 0.0,
                  "and none of them is zero-length");
        }
    }

    // ── Files that must be refused, with a reason ──
    {
        mf::File parsed;
        std::string error;
        check(!parse(Bytes{'n', 'o', 'p', 'e'}, parsed, error) && !error.empty(),
              "a file that is not MIDI is refused");

        Bytes format2 = header(2, 1, kPPQ);
        append(format2, track({}));
        check(!parse(format2, parsed, error) &&
                  error.find("format 2") != std::string::npos,
              "format 2 is refused by name");

        Bytes smpte = header(0, 1, 0xE728);   // high bit set: SMPTE timing
        append(smpte, track({}));
        check(!parse(smpte, parsed, error) &&
                  error.find("SMPTE") != std::string::npos,
              "SMPTE timing is refused by name");

        Bytes zero = header(0, 1, 0);
        append(zero, track({}));
        check(!parse(zero, parsed, error), "a zero division is refused");

        // A chunk that claims more bytes than the file holds: the good track
        // before it must survive.
        Bytes events;
        note(events, 0, kPPQ, 60);
        Bytes truncated = header(1, 2, kPPQ);
        append(truncated, track(events));
        Bytes liar{'M', 'T', 'r', 'k'};
        be32(liar, 9999);
        append(liar, {0x00, 0x90, 60, 100});
        append(truncated, liar);
        check(parse(truncated, parsed, error) && parsed.notes.size() >= 1,
              "a truncated trailing chunk does not lose the tracks before it");
    }

    // ── Into the document ──
    {
        daw::EngineController controller;
        check(controller.initialize(48000, 512, /*openDevice=*/false).isOk(),
              "controller initialises offline");
        const std::string trackId =
            controller.addTrack(daw::TrackKind::Instrument, "Keys");

        Bytes single;
        note(single, 0, kPPQ, 60);
        note(single, 0, kPPQ * 4, 64);   // still sounding well past the last start
        Bytes file = header(0, 1, kPPQ);
        append(file, track(single));

        const std::string path =
            (std::filesystem::temp_directory_path() / "daw-midifile-test.mid").string();
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()),
                      std::streamsize(file.size()));
        }

        const size_t tracksBefore = controller.project().tracks.size();
        daw::midifile::File info;
        const std::vector<std::string> clips =
            controller.importMidiFile(path, trackId, 2.0, &info);
        check(clips.size() == 1, "a single-track file lands as one clip");
        check(controller.project().tracks.size() == tracksBefore,
              "and makes no new tracks");

        const daw::TrackModel* model = controller.project().findTrack(trackId);
        const daw::ClipModel* clip =
            model && !model->clips.empty() ? &model->clips.front() : nullptr;
        if (check(clip != nullptr, "the clip is on the track it was aimed at")) {
            check(clip->kind == daw::ClipKind::Midi, "as a MIDI clip");
            check(clip->notes.size() == 2, "with the file's notes");
            check(near(clip->startSeconds, 2.0), "at the position asked for");
            // The clip must outlast its last note: syncTrackNotes drops a note
            // that starts at or past the clip's end and truncates one that
            // overruns it, so a clip cut at the last note-off would silence it.
            const double beats =
                daw::secondsToBeats(clip->durationSeconds, controller.project().tempo);
            check(beats >= 5.0 - 1e-9,
                  "and long enough to hold the last note to its end");
        }

        controller.undo();
        model = controller.project().findTrack(trackId);
        check(model && model->clips.empty(), "one undo takes the whole import back");
    }

    // ── A larger multi-track file spreads over lanes as one edit ──
    {
        daw::EngineController controller;
        controller.initialize(48000, 512, false);
        const std::string trackId =
            controller.addTrack(daw::TrackKind::Instrument, "Keys");
        const std::string untouchedTrackId =
            controller.addTrack(daw::TrackKind::Midi, "Untouched MIDI");
        controller.setTrackColor(untouchedTrackId, 0x123456);
        controller.setTrackVolumeLive(untouchedTrackId, 0.73f);
        const std::string untouchedClipId =
            controller.addMidiClip(untouchedTrackId, 7.0, 1.5);
        daw::NoteModel untouchedNote;
        untouchedNote.id = "untouched-note";
        untouchedNote.pitch = 91;
        untouchedNote.startBeats = 0.25;
        untouchedNote.lengthBeats = 0.75;
        untouchedNote.velocity = 57;
        controller.setClipNotes(untouchedTrackId, untouchedClipId,
                                {untouchedNote}, "Seed Unrelated MIDI Clip");
        controller.setClipName(untouchedTrackId, untouchedClipId,
                               "Must Survive Import History");
        const std::string historyBeforeImport = controller.undoLabel();

        const daw::TrackModel* untouchedBefore =
            controller.project().findTrack(untouchedTrackId);
        const daw::ClipModel untouchedClipBefore =
            untouchedBefore && !untouchedBefore->clips.empty()
                ? untouchedBefore->clips.front()
                : daw::ClipModel{};

        Bytes conductor;
        setTempo(conductor, 140.0);
        constexpr int kParts = 12;
        Bytes file = header(1, kParts + 1, kPPQ);
        append(file, track(conductor));
        for (int part = 0; part < kParts; ++part) {
            Bytes events;
            trackName(events, "Part " + std::to_string(part + 1));
            // Different counts make it easy to catch accidental cross-lane
            // mixing while still exercising a realistically dense import.
            for (int n = 0; n <= part; ++n) {
                note(events, 0, kPPQ / 4, 36 + part, 80 + (n % 20));
            }
            append(file, track(events));
        }

        const std::string path =
            (std::filesystem::temp_directory_path() / "daw-midifile-multi.mid").string();
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()),
                      std::streamsize(file.size()));
        }

        daw::midifile::File info;
        const std::vector<std::string> clips =
            controller.importMidiFile(path, trackId, 0.0, &info);
        check(clips.size() == kParts,
              "each of the file's many note tracks becomes a clip");
        check(controller.project().tracks.size() == kParts + 1,
              "all extra parts get exactly one lane of their own");
        check(controller.undoLabel() == "Import MIDI File",
              "the whole import adds one named history step");
        check(near(info.firstTempoBpm, 140.0, 0.01) &&
                  near(controller.project().tempo, 120.0),
              "the file's tempo is reported, not imposed");

        // These live properties deliberately change after the import without
        // adding another history entry. A whole-project snapshot would erase
        // them when undoing the import; a local delta must leave them alone.
        controller.setTrackVolumeLive(untouchedTrackId, 0.37f);
        controller.setTrackMuted(untouchedTrackId, true);
        check(controller.undoLabel() == "Import MIDI File",
              "unrelated live state does not obscure the import history step");

        auto untouchedIsIntact = [&] {
            const daw::TrackModel* lane =
                controller.project().findTrack(untouchedTrackId);
            if (!lane || lane->id != untouchedTrackId ||
                lane->kind != daw::TrackKind::Midi ||
                lane->name != "Untouched MIDI" || lane->color != 0x123456 ||
                !near(lane->volume, 0.37f) || !lane->muted ||
                lane->clips.size() != 1) {
                return false;
            }
            const daw::ClipModel& clip = lane->clips.front();
            return clip.id == untouchedClipBefore.id &&
                   clip.name == untouchedClipBefore.name &&
                   near(clip.startSeconds, untouchedClipBefore.startSeconds) &&
                   near(clip.durationSeconds,
                        untouchedClipBefore.durationSeconds) &&
                   clip.kind == untouchedClipBefore.kind &&
                   clip.color == untouchedClipBefore.color &&
                   clip.notes == untouchedClipBefore.notes;
        };

        auto importedCorrectly = [&] {
            if (controller.project().tracks.size() != kParts + 1 ||
                clips.size() != kParts) {
                return false;
            }
            for (int part = 0; part < kParts; ++part) {
                const std::size_t laneIndex =
                    part == 0 ? 0 : std::size_t(part + 1);
                const daw::TrackModel& lane =
                    controller.project().tracks[laneIndex];
                if (lane.clips.size() != 1 ||
                    lane.clips.front().id != clips[std::size_t(part)] ||
                    lane.clips.front().notes.size() != std::size_t(part + 1)) {
                    return false;
                }
                for (const daw::NoteModel& imported : lane.clips.front().notes) {
                    if (imported.pitch != 36 + part) return false;
                }
                if (part == 0) {
                    if (lane.id != trackId || lane.name != "Keys") return false;
                } else if (lane.name != "Part " + std::to_string(part + 1)) {
                    return false;
                }
            }
            return true;
        };
        const bool initiallyCorrect = importedCorrectly();
        check(initiallyCorrect,
              "source order, lane names, clip ids and note groups stay exact");
        check(untouchedIsIntact(),
              "an unrelated MIDI track and clip remain intact after import");
        check(info.tracksWithNotes() == kParts,
              "the parsed file reports every populated source track");

        controller.undo();
        check(controller.project().tracks.size() == 2 &&
                  controller.project().tracks.front().id == trackId &&
                  controller.project().tracks.front().clips.empty(),
              "one undo removes every imported clip and extra lane together");
        check(untouchedIsIntact(),
              "import undo preserves unrelated post-import live state and clip data");
        check(controller.undoLabel() == historyBeforeImport &&
                  controller.redoLabel() == "Import MIDI File",
              "one undo returns directly to the preceding history step");
        check(controller.canRedo(), "the whole import remains one redoable edit");

        controller.redo();
        const bool redoCorrect = importedCorrectly();
        check(redoCorrect,
              "one redo restores every lane, clip and grouped note exactly");
        check(untouchedIsIntact(),
              "import redo still leaves the unrelated MIDI model untouched");
        check(controller.undoLabel() == "Import MIDI File",
              "one redo restores the single import history step");
        if (redoCorrect) {
            check(controller.project().tracks.back().clips.front().notes.back().velocity ==
                      80 + ((kParts - 1) % 20),
                  "redo also preserves the final source note's properties");
        }
    }

    // ── A single-track import joins and extends its Pattern owner ──
    {
        daw::EngineController controller;
        controller.initialize(48000, 512, false);
        const std::string patternId = controller.addPattern("Import Pattern");
        const std::string childId =
            controller.addTrack(daw::TrackKind::Midi, "Pattern Child");
        controller.moveTrackToFolder(childId, patternId);

        const std::string unrelatedId =
            controller.addTrack(daw::TrackKind::Midi, "Outside Pattern");
        const std::string unrelatedClipId =
            controller.addMidiClip(unrelatedId, 5.0, 1.0);
        daw::NoteModel unrelatedNote;
        unrelatedNote.id = "outside-pattern-note";
        unrelatedNote.pitch = 47;
        unrelatedNote.startBeats = 0.0;
        unrelatedNote.lengthBeats = 0.5;
        unrelatedNote.velocity = 66;
        controller.setClipNotes(unrelatedId, unrelatedClipId, {unrelatedNote},
                                "Seed Outside Pattern");
        const std::string historyBeforeImport = controller.undoLabel();

        const daw::TrackModel* patternBefore =
            controller.project().findTrack(patternId);
        const std::string ownerId =
            patternBefore && !patternBefore->clips.empty()
                ? patternBefore->clips.front().id
                : std::string();
        const double ownerDurationBefore =
            patternBefore && !patternBefore->clips.empty()
                ? patternBefore->clips.front().durationSeconds
                : 0.0;

        Bytes events;
        note(events, 0, kPPQ, 65);
        Bytes file = header(0, 1, kPPQ);
        append(file, track(events));
        const std::string path =
            (std::filesystem::temp_directory_path() /
             "daw-midifile-pattern-single.mid")
                .string();
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()),
                      std::streamsize(file.size()));
        }

        const std::vector<std::string> clips =
            controller.importMidiFile(path, childId, 1.5);
        const daw::TrackModel* child = controller.project().findTrack(childId);
        const daw::TrackModel* pattern = controller.project().findTrack(patternId);
        const daw::TrackModel* unrelated =
            controller.project().findTrack(unrelatedId);
        check(clips.size() == 1 && child && child->parentId == patternId &&
                  child->outputBusId == patternId && child->clips.size() == 1 &&
                  child->clips.front().id == clips.front() &&
                  child->clips.front().patternClipId == ownerId,
              "a single MIDI import keeps its target inside the Pattern owner");
        check(pattern && pattern->clips.size() == 1 &&
                  pattern->clips.front().id == ownerId &&
                  near(pattern->clips.front().durationSeconds, 3.5),
              "the existing Pattern clip grows to contain the imported source");
        check(controller.liveNoteOn(childId, 65, 100) &&
                  controller.liveNoteOff(childId, 65),
              "the Pattern child remains live after single-track import");

        controller.setTrackVolumeLive(unrelatedId, 0.29f);
        controller.setTrackMuted(unrelatedId, true);
        auto unrelatedSurvives = [&] {
            const daw::TrackModel* lane =
                controller.project().findTrack(unrelatedId);
            return lane && lane->parentId.empty() && lane->muted &&
                   near(lane->volume, 0.29f) && lane->clips.size() == 1 &&
                   lane->clips.front().id == unrelatedClipId &&
                   lane->clips.front().notes.size() == 1 &&
                   lane->clips.front().notes.front() == unrelatedNote;
        };
        check(unrelated && unrelatedSurvives(),
              "single Pattern import leaves unrelated MIDI state intact");

        controller.undo();
        child = controller.project().findTrack(childId);
        pattern = controller.project().findTrack(patternId);
        check(child && child->parentId == patternId && child->clips.empty() &&
                  pattern && pattern->clips.size() == 1 &&
                  pattern->clips.front().id == ownerId &&
                  near(pattern->clips.front().durationSeconds,
                       ownerDurationBefore),
              "one undo removes the import and restores the Pattern boundary");
        check(unrelatedSurvives() &&
                  controller.undoLabel() == historyBeforeImport &&
                  controller.redoLabel() == "Import MIDI File",
              "Pattern import undo preserves unrelated live state and history");

        controller.redo();
        child = controller.project().findTrack(childId);
        pattern = controller.project().findTrack(patternId);
        check(child && child->clips.size() == 1 &&
                  child->clips.front().id == clips.front() &&
                  child->clips.front().patternClipId == ownerId && pattern &&
                  near(pattern->clips.front().durationSeconds, 3.5) &&
                  unrelatedSurvives(),
              "one redo restores Pattern membership without touching other MIDI");
        check(controller.liveNoteOn(childId, 67, 90) &&
                  controller.liveNoteOff(childId, 67),
              "the restored single-track Pattern import is live");
    }

    // ── Multi-track import stays ordered in a nested Pattern subtree ──
    {
        daw::EngineController controller;
        controller.initialize(48000, 512, false);
        const std::string patternId = controller.addPattern("Nested Import");
        const std::string folderId = controller.addFolder(false, "Sources");
        controller.moveTrackToFolder(folderId, patternId);
        const std::string targetId =
            controller.addTrack(daw::TrackKind::Instrument, "Nested Target");
        controller.moveTrackToFolder(targetId, folderId);

        const std::string unrelatedId =
            controller.addTrack(daw::TrackKind::Midi, "Root MIDI");
        const std::string unrelatedClipId =
            controller.addMidiClip(unrelatedId, 8.0, 1.0);
        daw::NoteModel rootNote;
        rootNote.id = "root-midi-note";
        rootNote.pitch = 38;
        rootNote.startBeats = 0.125;
        rootNote.lengthBeats = 0.5;
        rootNote.velocity = 72;
        controller.setClipNotes(unrelatedId, unrelatedClipId, {rootNote},
                                "Seed Root MIDI");
        const std::string historyBeforeImport = controller.undoLabel();

        constexpr int kParts = 3;
        Bytes conductor;
        setTempo(conductor, 110.0);
        Bytes file = header(1, kParts + 1, kPPQ);
        append(file, track(conductor));
        for (int part = 0; part < kParts; ++part) {
            Bytes events;
            trackName(events, "Nested " + std::to_string(part + 1));
            for (int n = 0; n <= part; ++n) {
                note(events, 0, kPPQ / 4, 50 + part, 85 + n);
            }
            append(file, track(events));
        }
        const std::string path =
            (std::filesystem::temp_directory_path() /
             "daw-midifile-pattern-multi.mid")
                .string();
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data()),
                      std::streamsize(file.size()));
        }

        const std::vector<std::string> clips =
            controller.importMidiFile(path, targetId, 3.0);
        const daw::TrackModel* pattern = controller.project().findTrack(patternId);
        const std::string ownerId =
            pattern && pattern->clips.size() == 2
                ? pattern->clips.back().id
                : std::string();
        check(pattern && pattern->clips.size() == 2 &&
                  pattern->clips.back().kind == daw::ClipKind::Pattern &&
                  near(pattern->clips.back().startSeconds, 3.0) &&
                  near(pattern->clips.back().durationSeconds, 2.0),
              "an import outside existing bounds creates one Pattern owner clip");

        std::vector<std::string> importedTrackIds;
        bool subtreeCorrect = clips.size() == kParts &&
                              controller.project().tracks.size() == 6;
        for (int part = 0; part < kParts && subtreeCorrect; ++part) {
            const std::size_t trackIndex = std::size_t(part + 2);
            const daw::TrackModel& lane = controller.project().tracks[trackIndex];
            importedTrackIds.push_back(lane.id);
            subtreeCorrect = lane.parentId == folderId &&
                             lane.outputBusId == patternId &&
                             lane.clips.size() == 1 &&
                             lane.clips.front().id == clips[std::size_t(part)] &&
                             lane.clips.front().patternClipId == ownerId &&
                             lane.clips.front().notes.size() ==
                                 std::size_t(part + 1);
            if (part == 0) {
                subtreeCorrect = subtreeCorrect && lane.id == targetId &&
                                 lane.name == "Nested Target";
            } else {
                subtreeCorrect =
                    subtreeCorrect &&
                    lane.name == "Nested " + std::to_string(part + 1);
            }
        }
        subtreeCorrect =
            subtreeCorrect && controller.project().tracks.back().id == unrelatedId;
        check(subtreeCorrect,
              "all imported lanes stay ordered beside the nested Pattern target");

        bool allLive = importedTrackIds.size() == kParts;
        for (const std::string& laneId : importedTrackIds) {
            allLive = controller.liveNoteOn(laneId, 70, 100) &&
                      controller.liveNoteOff(laneId, 70) && allLive;
        }
        check(allLive, "every imported Pattern lane is live after graph publish");

        controller.setTrackVolumeLive(unrelatedId, 0.41f);
        controller.setTrackMuted(unrelatedId, true);
        auto rootMidiSurvives = [&] {
            const daw::TrackModel* lane =
                controller.project().findTrack(unrelatedId);
            return lane && lane->parentId.empty() && lane->muted &&
                   near(lane->volume, 0.41f) && lane->clips.size() == 1 &&
                   lane->clips.front().id == unrelatedClipId &&
                   lane->clips.front().notes.size() == 1 &&
                   lane->clips.front().notes.front() == rootNote;
        };

        controller.undo();
        pattern = controller.project().findTrack(patternId);
        const daw::TrackModel* target = controller.project().findTrack(targetId);
        bool importedLanesGone = true;
        for (std::size_t i = 1; i < importedTrackIds.size(); ++i) {
            importedLanesGone &=
                controller.project().findTrack(importedTrackIds[i]) == nullptr;
        }
        check(pattern && pattern->clips.size() == 1 && target &&
                  target->parentId == folderId && target->clips.empty() &&
                  importedLanesGone,
              "one undo removes nested lanes and their created Pattern owner");
        check(rootMidiSurvives() &&
                  controller.undoLabel() == historyBeforeImport &&
                  controller.redoLabel() == "Import MIDI File",
              "nested import undo preserves unrelated root MIDI and history");

        controller.redo();
        pattern = controller.project().findTrack(patternId);
        bool redoCorrect = pattern && pattern->clips.size() == 2 &&
                           pattern->clips.back().id == ownerId &&
                           controller.project().tracks.size() == 6 &&
                           controller.project().tracks.back().id == unrelatedId;
        for (int part = 0; part < kParts && redoCorrect; ++part) {
            const daw::TrackModel& lane =
                controller.project().tracks[std::size_t(part + 2)];
            redoCorrect = lane.id == importedTrackIds[std::size_t(part)] &&
                          lane.parentId == folderId &&
                          lane.outputBusId == patternId &&
                          lane.clips.size() == 1 &&
                          lane.clips.front().id == clips[std::size_t(part)] &&
                          lane.clips.front().patternClipId == ownerId;
        }
        check(redoCorrect && rootMidiSurvives(),
              "one redo restores Pattern owner, lane order and unrelated state");
        allLive = redoCorrect;
        for (const std::string& laneId : importedTrackIds) {
            allLive = controller.liveNoteOn(laneId, 71, 90) &&
                      controller.liveNoteOff(laneId, 71) && allLive;
        }
        check(allLive, "every restored nested Pattern lane is live");
    }

    std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILURES PRESENT\n",
                failures);
    return failures == 0 ? 0 : 1;
}
