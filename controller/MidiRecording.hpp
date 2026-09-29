#pragma once

#include "Midi/MidiEvent.hpp"
#include "model/Document.hpp"

#include <map>
#include <tuple>

namespace daw {

enum class LiveMidiOrigin { Performance, Audition, Cleanup };

/// Captured before queuing a hardware message to the UI thread. The beat clock
/// is monotonic across loop wraps; wall time also runs while transport is idle.
struct MidiInputStamp {
    std::uint64_t timeNs = 0;
    double transportBeats = 0.0;
    double tempo = 120.0;
};

struct MidiPerformance {
    std::vector<NoteModel> notes;
    std::vector<ControllerLane> lanes;
    std::vector<SlideNoteModel> slideNotes;
    bool empty() const noexcept {
        return notes.empty() && lanes.empty();
    }
};

/// Control-thread-only builder. No document mutations or audio graph rebuilds.
class MidiRecording {
  public:
    MidiPerformance data;
    void event(std::uint64_t source, int status, int data1, int data2, double beat,
               std::uint64_t order);
    void parameter(const std::string &slot, const std::string &parameter, const std::string &name,
                   double value, double beat, std::uint64_t order);
    MidiPerformance finished(double endBeat) const;
    void close(double beat);
    void extendHeld(double beat);
    bool empty() const noexcept {
        return data.empty();
    }
    bool hasHeldNotes() const noexcept {
        return !m_held.empty();
    }

  private:
    std::map<std::tuple<std::uint64_t, int, int>, std::size_t> m_held;
    void point(int cc, int channel, int key, const std::string &slot, const std::string &parameter,
               const std::string &name, double value, double beat, std::uint64_t order);
};

MidiPerformance sliceMidiPerformance(const MidiPerformance &data, double fromBeat, double toBeat,
                                     bool newIds = true);
void mergeMidiPerformance(ClipModel &clip, MidiPerformance data, double offsetBeats,
                          double endBeats);
/// Materialize the audible source range, including left trims and comp pieces.
void sliceMidiClipContent(ClipModel &clip, double fromSeconds, double toSeconds, double tempo,
                          bool newIds);
std::vector<ClipModel> midiPlaybackClips(const TrackModel &track, double tempo);
bool sameMidiLaneTarget(const ControllerLane &a, const ControllerLane &b);

} // namespace daw
