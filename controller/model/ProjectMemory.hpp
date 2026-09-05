#pragma once
#include "model/Document.hpp"

namespace daw {
// Conservative accounting for the dominant variable-size snapshot payloads.
// Includes headroom for string allocation, object metadata and model indexes.
inline std::size_t estimatedProjectBytes(const ProjectModel& project) {
    std::size_t bytes = sizeof(project) + project.name.size() + project.author.size() +
        project.aiInstructions.size() + project.coverImagePath.size();
    const auto inserts = [&](const auto& slots) {
        std::size_t result = slots.capacity() * sizeof(InsertModel);
        for (const auto& slot : slots) {
            result += slot.path.size() + slot.name.size() + slot.uid.size() +
                slot.stateFile.size() + slot.rightStateFile.size() +
                (slot.parameters.capacity() + slot.rightParameters.capacity()) * sizeof(InsertParameter);
            for (const auto& value : slot.parameters) result += value.id.size();
            for (const auto& value : slot.rightParameters) result += value.id.size();
        }
        return result;
    };
    bytes += inserts(project.masterInserts) + project.tracks.capacity() * sizeof(TrackModel);
    for (const auto& track : project.tracks) {
        bytes += track.name.size() + track.id.size() + track.clips.capacity() * sizeof(ClipModel) +
            track.sends.capacity() * sizeof(SendModel) + inserts(track.inserts) +
            inserts(track.samplerFx.inserts) +
            (track.instrument.parameters.capacity() + track.instrument.rightParameters.capacity()) * sizeof(InsertParameter);
        for (const auto& clip : track.clips) {
            bytes += clip.name.size() + clip.id.size() + clip.filePath.size() +
                clip.notes.capacity() * sizeof(NoteModel) +
                clip.lanes.capacity() * sizeof(ControllerLane) +
                clip.takes.capacity() * sizeof(TakeModel) +
                clip.comp.capacity() * sizeof(CompSegment) +
                clip.automation.points.capacity() * sizeof(AutomationPoint) +
                inserts(clip.inserts) + inserts(clip.offlineProcess.chain);
            for (const auto& note : clip.notes) bytes += note.id.size();
            for (const auto& lane : clip.lanes)
                bytes += lane.points.capacity() * sizeof(AutomationPoint);
            for (const auto& take : clip.takes)
                bytes += take.notes.capacity() * sizeof(NoteModel) + take.filePath.size() + take.name.size();
        }
    }
    return bytes * 2;
}
} // namespace daw
