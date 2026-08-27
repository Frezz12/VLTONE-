#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Reading Standard MIDI Files.
///
/// Pure: no Qt, no engine, no document. It turns bytes into notes measured in
/// beats — which is already `NoteModel`'s unit, so nothing here needs a tempo to
/// place a note. What the file says about tempo is *reported* rather than
/// applied; whether the project follows it is the user's call, made where the
/// import happens.
///
/// Deliberately not handled: pitch bend, controllers and aftertouch (the
/// document has controller lanes — that is the obvious next increment), key and
/// time-signature meta events, and per-channel splitting.
namespace daw::midifile {

/// One note, with where it came from. `track` and `channel` are kept because
/// the importer puts each of the file's tracks on its own lane.
struct Note {
    int pitch = 60;
    double startBeats = 0.0;
    double lengthBeats = 0.0;
    int velocity = 100;
    int channel = 0;
    int track = 0;
};

struct File {
    int format = 0;              ///< 0 (one track) or 1 (parallel tracks)
    int trackCount = 0;          ///< as declared in the header
    int ticksPerQuarter = 96;
    /// The first set-tempo event's BPM, or 0 when the file carries none.
    double firstTempoBpm = 0.0;
    /// More than one set-tempo event: only the first is reported, so a caller
    /// that adopts the tempo can say the rest were dropped.
    bool hasTempoChanges = false;
    double lengthBeats = 0.0;    ///< end of the last note
    /// Sorted by start, then pitch. Merged across every track in the file.
    std::vector<Note> notes;
    /// One entry per MTrk chunk, in file order; empty string when the track
    /// carried no name meta event.
    std::vector<std::string> trackNames;

    /// How many of the file's tracks actually produced notes. A format-1 file
    /// usually spends its first track on tempo and nothing else.
    int tracksWithNotes() const;
};

/// Parse a file from disk. On failure `error` says why, in words a status bar
/// can show.
bool parse(const std::string& path, File& out, std::string& error);

/// Parse from memory — what the tests drive, so they need no fixture files.
bool parseBytes(const std::uint8_t* data, std::size_t size, File& out,
                std::string& error);

} // namespace daw::midifile
