#pragma once
#include "model/Document.hpp"
#include <unordered_set>

namespace daw {
// Conservative accounting for the dominant variable-size snapshot payloads.
// Includes headroom for string allocation, object metadata and model indexes.
inline std::size_t estimatedProjectBytes(const ProjectModel& project) {
    std::size_t bytes = sizeof(project) + project.name.size() + project.author.size() +
        project.aiInstructions.size() + project.coverImagePath.size() +
        project.notebookHtml.size() +
        project.notebookCues.capacity() * sizeof(NotebookCueModel);
    for (const auto& cue : project.notebookCues) bytes += cue.text.size();
    const auto audioDocument = [](const AudioEditDocument& doc) {
        std::size_t result=doc.sources.capacity()*sizeof(AudioEditSource)+doc.regions.capacity()*sizeof(AudioEditRegion)+doc.revision.size();
        for(const auto& source:doc.sources)result+=source.id.size()+source.filePath.size()+source.assetId.size()+source.sha256.size();
        for(const auto& region:doc.regions)result+=region.id.size()+region.sourceId.size()+region.envelopes.capacity()*sizeof(AudioEditEnvelope);
        return result;
    };
    const auto sidechains = [&](const InsertModel& slot) {
        std::size_t result = slot.sidechainTrackIds.capacity() * sizeof(std::string);
        for (const auto& id : slot.sidechainTrackIds) result += id.size();
        return result + audioDocument(slot.audioEdit);
    };
    const auto inserts = [&](const auto& slots) {
        std::size_t result = slots.capacity() * sizeof(InsertModel);
        for (const auto& slot : slots) {
            result += sidechains(slot) + slot.path.size() + slot.name.size() + slot.uid.size() +
                slot.stateFile.size() + slot.rightStateFile.size() +
                (slot.parameters.capacity() + slot.rightParameters.capacity()) * sizeof(InsertParameter);
            result += slot.rackParameterIds.capacity() * sizeof(std::string);
            for (const auto& id : slot.rackParameterIds) result += id.size();
            for (const auto& value : slot.parameters) result += value.id.size();
            for (const auto& value : slot.rightParameters) result += value.id.size();
        }
        return result;
    };
    const auto groups = [](const auto& rackGroups) {
        std::size_t result=rackGroups.capacity()*sizeof(RackGroupModel);
        for (const auto& group:rackGroups) {
            result+=group.id.size()+group.name.size()+group.insertIds.capacity()*sizeof(std::string);
            for (const auto& id:group.insertIds) result+=id.size();
        }
        return result;
    };
    bytes += groups(project.masterRackGroups);
    bytes += inserts(project.masterInserts) + project.tracks.capacity() * sizeof(TrackModel);
    std::unordered_set<const ClipContent*> accountedContents;
    const auto accountTrack = [&](const TrackModel& track) {
        bytes += groups(track.rackGroups);
        bytes += track.name.size() + track.id.size() + track.iconId.size() + track.clips.capacity() * sizeof(ClipModel) +
            track.sends.capacity() * sizeof(SendModel) + inserts(track.inserts) +
            inserts(track.samplerFx.inserts) + sidechains(track.instrument) +
            (track.instrument.parameters.capacity() + track.instrument.rightParameters.capacity()) * sizeof(InsertParameter);
        for (const auto& clip : track.clips) {
            bytes += clip.name.size() + clip.id.size() + clip.contentId.size() +
                clip.patternClipId.size() + clip.patternPartId.size() +
                clip.warp.markers.capacity() * sizeof(WarpMarker) +
                inserts(clip.inserts) + inserts(clip.offlineProcess.chain);
            // Linked placements own one payload in an undo snapshot. Counting
            // it for every view would evict history as the group grows.
            const auto* content = clip.contentStorage().get();
            if (accountedContents.insert(content).second) {
                bytes += sizeof(ClipContent) + clip.filePath.size() +
                    clip.notes.capacity() * sizeof(NoteModel) +
                    clip.slideNotes.capacity() * sizeof(SlideNoteModel) +
                    clip.lanes.capacity() * sizeof(ControllerLane) +
                    clip.takes.capacity() * sizeof(TakeModel) +
                    clip.comp.capacity() * sizeof(CompSegment) +
                    clip.automation.points.capacity() * sizeof(AutomationPoint) +
                    audioDocument(clip.audioEdit) +
                    content->patternParts.capacity() * sizeof(PatternPartModel);
                for (const auto& part : content->patternParts)
                    bytes += part.id.size() + part.trackId.size() + part.contentId.size();
                for (const auto& note : clip.notes) bytes += note.id.size();
                for (const auto& slide : clip.slideNotes) {
                    bytes += slide.id.size() + slide.referenceNoteId.size() +
                        slide.targetNoteIds.capacity() * sizeof(std::string) +
                        slide.points.capacity() * sizeof(AutomationPoint);
                    for (const auto& id : slide.targetNoteIds) bytes += id.size();
                }
                for (const auto& lane : clip.lanes)
                    bytes += lane.points.capacity() * sizeof(AutomationPoint);
                for (const auto& take : clip.takes)
                    bytes += take.notes.capacity() * sizeof(NoteModel) + take.filePath.size() + take.name.size();
            }
            bytes += clip.offlineHistory.capacity() * sizeof(OfflineRenderVersion) + clip.offlineVersionId.size();
            for (const auto& version : clip.offlineHistory) {
                bytes += version.id.size() + version.parentId.size() + version.label.size() +
                    version.source.filePath.size() +
                    version.source.takes.capacity() * sizeof(TakeModel) +
                    version.source.comp.capacity() * sizeof(CompSegment);
                bytes+=audioDocument(version.source.audioEdit);
                for (const auto& take : version.source.takes)
                    bytes += take.notes.capacity() * sizeof(NoteModel) + take.filePath.size() + take.name.size();
            }
        }
    };
    for (const auto& track : project.tracks) accountTrack(track);
    bytes += project.clipLibrary.capacity() * sizeof(ClipLibraryEntry);
    for (const auto& entry : project.clipLibrary) {
        bytes += entry.id.size() + entry.name.size() + entry.tracks.capacity() * sizeof(TrackModel);
        for (const auto& track : entry.tracks) accountTrack(track);
    }
    return bytes * 2;
}
} // namespace daw
