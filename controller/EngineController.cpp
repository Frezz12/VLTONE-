#include "EngineController.hpp"
#include "ChannelStripPreset.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/CollaborationState.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "plugins/PluginConvert.hpp"

#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerPrecompute.hpp"
#include "DSP/Resampler.hpp"
#include "Nodes/BasicNodes.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace daw {

namespace {

/// Every value the host itself writes into a plugin parameter, under
/// `DAW_PLUGIN_DIAGNOSTICS=1`. A knob that moves on its own is either the
/// plugin doing it or the host, and this is the line that tells the two apart.
bool diagnosePluginParameters() {
    static const bool on = std::getenv("DAW_PLUGIN_DIAGNOSTICS") != nullptr;
    return on;
}

void logParameterWrite(const char* source, plugins::PluginInstance* instance,
                       std::int32_t index, double plainValue) {
    if (!diagnosePluginParameters() || !instance) return;
    const std::span<const plugins::ParameterInfo> parameters = instance->parameters();
    const char* name = index >= 0 && std::size_t(index) < parameters.size()
                           ? parameters[std::size_t(index)].name.c_str()
                           : "?";
    std::fprintf(stderr, "[param] %-9s %s :: %s (index %d) <- %.6f\n", source,
                 instance->descriptor().name.c_str(), name, index, plainValue);
}

std::string pluginStateFileName(const std::string& stem,
                                const std::vector<std::uint8_t>& chunk) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint8_t byte : chunk) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(hash));
    return stem + "-" + text + ".bin";
}

void snapshotParameters(plugins::PluginInstance& instance,
                        std::vector<InsertParameter>& destination) {
    const std::span<const plugins::ParameterInfo> parameters = instance.parameters();
    destination.reserve(destination.size() + parameters.size());

    // A recovery snapshot used to linearly scan every parameter already in the
    // document for every parameter reported by the plugin. Large instruments
    // routinely expose thousands of parameters, turning a one-second recovery
    // tick into quadratic GUI-thread work. Build the membership set once.
    std::unordered_set<std::string> storedIds;
    storedIds.reserve(destination.size() + parameters.size());
    for (const InsertParameter& stored : destination) {
        if (!stored.id.empty()) storedIds.insert(stored.id);
    }
    for (const plugins::ParameterInfo& parameter : parameters) {
        if (parameter.id.empty()) continue;
        // Host writes and editor notifications are mirrored into the document
        // before the audio thread necessarily consumes their queued event. Keep
        // that newer value; querying the processor in this short interval would
        // roll the recovery snapshot back by one block.
        if (!storedIds.insert(parameter.id).second) continue;
        destination.push_back(
            InsertParameter{parameter.id, instance.parameterValue(parameter.index)});
    }
}

std::string defaultTrackName(TrackKind kind) {
    switch (kind) {
        case TrackKind::Audio: return "Audio";
        case TrackKind::Instrument: return "Instrument";
        case TrackKind::Midi: return "MIDI";
        case TrackKind::Pattern: return "Pattern";
        case TrackKind::Automation: return "Automation";
        case TrackKind::Bus: return "Bus";
        case TrackKind::Aux: return "Aux";
        case TrackKind::Group: return "Group";
        case TrackKind::Master: return "Master";
        case TrackKind::Folder: return "Folder";
    }
    return "Track";
}

uint32_t defaultTrackColor(TrackKind kind) {
    // Distinct but similarly weighted colours: track kind is recognisable at a
    // glance without any lane becoming brighter or more important than the
    // others in the dark arrangement.
    switch (kind) {
        case TrackKind::Audio:      return 0x4A90D9;
        case TrackKind::Instrument: return 0x8B7BE8;
        case TrackKind::Midi:       return 0x43B3A5;
        case TrackKind::Pattern:    return 0xB47AE6;
        case TrackKind::Automation: return 0xD6A24A;
        case TrackKind::Bus:        return 0xD47C55;
        case TrackKind::Aux:        return 0x54A9C1;
        case TrackKind::Group:      return 0x7B86C9;
        case TrackKind::Master:     return 0xC56778;
        case TrackKind::Folder:     return 0x77849A;
    }
    return 0x4A90D9;
}

void inheritAutomationLaneColors(ProjectModel& project) {
    for (TrackModel& lane : project.tracks) {
        if (!isAutomationLane(lane) || lane.parentId.empty()) continue;
        const TrackModel* owner = project.findTrack(lane.parentId);
        if (!owner) continue;
        lane.color = owner->color;
        for (ClipModel& clip : lane.clips) clip.color = owner->color;
    }
}

/// Flatten one automation clip's curve into (beat, plain value) pairs on the
/// timeline, breaking every bent or S-shaped segment into short straight runs.
///
/// The plugin automation path interpolates linearly and nothing else, on the
/// audio thread, for every plugin on every block. Teaching it about shapes
/// would put `std::pow` there; densifying once here, while the graph is being
/// compiled, costs nothing at playback and is exact to within a step.
///
/// The clip is a window on its curve: a point past the clip's end does not
/// sound, and the value is held from the clip's start to its first point.
template <typename ToPlain>
void appendCurvePoints(std::vector<std::pair<double, double>>& out,
                       const ClipModel& clip, double beatsPerSecond,
                       const ToPlain& toPlain) {
    const double startBeats = clip.startSeconds * beatsPerSecond;
    const double lengthBeats = clip.durationSeconds * beatsPerSecond;
    const auto& points = clip.automation.points;

    // The clip's own edge, so a curve stops driving where the clip stops and
    // whatever follows it takes over cleanly.
    out.emplace_back(startBeats,
                     toPlain(automationValueAt(points, 0.0,
                                               clip.automation.defaultValue)));

    for (std::size_t i = 0; i < points.size(); ++i) {
        const AutomationPoint& point = points[i];
        if (point.beats > lengthBeats) break;
        out.emplace_back(startBeats + point.beats, toPlain(point.value));

        if (i + 1 >= points.size()) continue;
        const AutomationPoint& next = points[i + 1];
        const int steps = engine::curve::densifySteps(toCurveShape(point.shape),
                                                      point.curve);
        if (steps <= 1) continue;
        const double span = next.beats - point.beats;
        for (int step = 1; step < steps; ++step) {
            const double at = point.beats + span * double(step) / double(steps);
            if (at > lengthBeats) break;
            out.emplace_back(startBeats + at,
                             toPlain(automationValueAt(points, at,
                                                       clip.automation.defaultValue)));
        }
    }

    // And the far edge, so the last value is held to the clip's end rather than
    // to its last breakpoint.
    if (lengthBeats > 0.0) {
        out.emplace_back(startBeats + lengthBeats,
                         toPlain(automationValueAt(points, lengthBeats,
                                                   clip.automation.defaultValue)));
    }
}

/// A track a recording can land on. A summing folder has a channel — it is a
/// bus — but it plays other tracks' audio, never its own, so arming one would
/// have nowhere to put the file.
bool isRecordable(const TrackModel& track) {
    return carriesAudio(track) && !isFolder(track);
}

/// A template is configuration, not an arrangement. Pattern containers are
/// structural clips, so they are regenerated by repairPatternClips after a
/// template is opened or imported rather than persisted here.
void stripTemplateArrangement(ProjectModel& project,
                              const std::string& templateName = {}) {
    if (!templateName.empty()) project.name = templateName;
    project.loopStartSeconds = 0.0;
    project.loopEndSeconds = 0.0;
    project.loopEnabled = false;
    for (TrackModel& track : project.tracks) track.clips.clear();
}

/// Give an imported track set an identity of its own. Track, send and plugin
/// slot ids are all document-global references; remapping only track ids would
/// leave automation/sidechains and sampler FX pointing into the template (or,
/// in the worst case, at an unrelated object in the destination project).
void remapTemplateTrackIds(ProjectModel& project) {
    std::unordered_map<std::string, std::string> tracks;
    std::unordered_map<std::string, std::string> slots;

    for (TrackModel& track : project.tracks) {
        const std::string previous = track.id;
        track.id = newUuid();
        if (!previous.empty()) tracks[previous] = track.id;
    }

    auto remapSlotId = [&slots](InsertModel& slot) {
        if (slot.id.empty()) return;
        const std::string previous = slot.id;
        slot.id = newUuid();
        slots[previous] = slot.id;
    };
    for (TrackModel& track : project.tracks) {
        remapSlotId(track.instrument);
        for (InsertModel& slot : track.samplerFx.inserts) remapSlotId(slot);
        for (InsertModel& slot : track.inserts) remapSlotId(slot);
    }

    const auto mappedTrack = [&tracks](const std::string& id) {
        const auto found = tracks.find(id);
        return found == tracks.end() ? std::string{} : found->second;
    };
    const auto remapSlotReferences = [&tracks](InsertModel& slot) {
        if (slot.sidechainTrackId.empty()) return;
        const auto found = tracks.find(slot.sidechainTrackId);
        slot.sidechainTrackId = found == tracks.end() ? std::string{}
                                                      : found->second;
    };

    for (TrackModel& track : project.tracks) {
        track.parentId = mappedTrack(track.parentId);
        track.outputBusId = mappedTrack(track.outputBusId);

        std::erase_if(track.sends, [&](SendModel& send) {
            const std::string destination = mappedTrack(send.destinationTrackId);
            if (destination.empty()) return true;
            send.id = newUuid();
            send.destinationTrackId = destination;
            return false;
        });

        if (!track.samplerFx.ownerInstrumentId.empty()) {
            const auto found = slots.find(track.samplerFx.ownerInstrumentId);
            track.samplerFx.ownerInstrumentId =
                found == slots.end() ? std::string{} : found->second;
        }
        remapSlotReferences(track.instrument);
        for (InsertModel& slot : track.samplerFx.inserts)
            remapSlotReferences(slot);
        for (InsertModel& slot : track.inserts) remapSlotReferences(slot);
    }
}

/// Shortest a clip may be trimmed or split to, so an edge drag can never
/// collapse a clip to nothing.
constexpr double kMinClipSeconds = 0.02;

/// The same idea for notes: a thirty-second note is the shortest a resize drag
/// can produce, so a note can never become invisible and unselectable.
constexpr double kMinNoteBeats = 1.0 / 32.0;
constexpr int kMinPitch = 0;
constexpr int kMaxPitch = 127;

std::string lowercaseExtension(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) {
                       return char(std::tolower(value));
                   });
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    return extension;
}

std::string audioContentType(const std::string& extension) {
    if (extension == "wav" || extension == "wave") return "audio/wav";
    if (extension == "flac") return "audio/flac";
    if (extension == "aif" || extension == "aiff" || extension == "aifc")
        return "audio/aiff";
    if (extension == "mp3") return "audio/mpeg";
    if (extension == "ogg" || extension == "oga" || extension == "opus")
        return "audio/ogg";
    if (extension == "caf") return "audio/x-caf";
    if (extension == "w64") return "audio/wav";
    return {};
}

std::optional<collab::SharedAssetMutationRequest> sharedAudioRequest(
    const std::string& sourcePath) {
    if (sourcePath.empty()) return std::nullopt;
    std::error_code error;
    fs::path path = platform::pathFromUtf8(sourcePath);
    if (path.is_relative()) path = fs::absolute(path, error);
    if (error) return std::nullopt;
    const fs::path canonical = fs::weakly_canonical(path, error);
    if (!error) path = canonical;
    error.clear();
    if (!fs::is_regular_file(path, error) || error) return std::nullopt;

    audio::platform::AudioFileInfo info;
    if (!audio::platform::probeAudioFile(platform::pathToUtf8(path), info) ||
        info.sampleRate <= 0.0 || info.channels == 0 || info.frames == 0) {
        return std::nullopt;
    }
    const std::string extension = lowercaseExtension(path);
    const std::string contentType = audioContentType(extension);
    const std::string displayName = platform::pathToUtf8(path.filename());
    if (contentType.empty() || displayName.empty()) return std::nullopt;

    collab::SharedAssetMutationRequest request;
    request.requestId = newUuid();
    request.assetId = newUuid();
    request.sourcePath = platform::pathToUtf8(path);
    request.displayName = displayName;
    request.contentType = contentType;
    request.codec = extension;
    request.sampleRate = info.sampleRate;
    request.channels = std::uint32_t(info.channels);
    request.frames = std::uint64_t(info.frames);
    return request;
}

AssetRef expectedSharedAudioAsset(
    const collab::SharedAssetMutationRequest& request) {
    AssetRef asset;
    asset.assetId = request.assetId;
    asset.kind = AssetKind::Audio;
    asset.originalName = request.displayName;
    asset.mimeType = request.contentType;
    asset.codec = request.codec;
    asset.sampleRate = request.sampleRate;
    asset.channels = request.channels;
    asset.frames = request.frames;
    return asset;
}

bool completeSharedAudioAsset(const AssetRef& asset,
                              const AssetRef& expected) {
    const bool hashReady = asset.sha256.size() == 64 &&
        std::all_of(asset.sha256.begin(), asset.sha256.end(),
                    [](unsigned char value) {
                        return (value >= '0' && value <= '9') ||
                               (value >= 'a' && value <= 'f');
                    });
    return hashReady && asset.byteSize > 0 &&
           asset.assetId == expected.assetId &&
           asset.kind == AssetKind::Audio &&
           asset.originalName == expected.originalName &&
           asset.mimeType == expected.mimeType &&
           asset.codec == expected.codec &&
           asset.sampleRate == expected.sampleRate &&
           asset.channels == expected.channels &&
           asset.frames == expected.frames;
}

/// The subset of a clip's editor state that is baked into its audio.
///
/// Everything a clip's editor can move that is *not* here — the stretch, the
/// pitch, the formant, the loop points — is applied while the clip plays and
/// costs nothing to change. Only this struct decides whether a bake has to be
/// redone, which is why the cache compares it directly.
plugins::sampler::PrecomputeSettings clipPrecomputeSettings(
    const ClipSampleEditModel& s) {
    plugins::sampler::PrecomputeSettings out;
    out.boost = s.boost; out.eqLow = s.eqLow; out.eqMid = s.eqMid;
    out.eqHigh = s.eqHigh; out.ringMix = s.ringMix; out.ringFreq = s.ringFreq;
    out.cut = s.cut; out.res = s.res; out.reverbType = s.reverbType;
    out.reverb = s.reverb; out.stereoDelay = s.stereoDelay; out.pogo = s.pogo;
    out.removeDc = s.removeDc; out.reversePolarity = s.reversePolarity;
    out.normalize = s.normalize; out.fadeStereo = s.fadeStereo;
    out.reverse = s.reverse; out.swapStereo = s.swapStereo;
    return out;
}

void appendCommand(const std::shared_ptr<collab::BatchCommand>& batch,
                   collab::CommandBody body) {
    collab::ProjectCommand child;
    child.body = std::move(body);
    batch->commands.push_back(std::move(child));
}

bool supportedSharedBuiltin(const InsertModel& insert) {
    return insert.format == PluginFormat::Internal &&
           (insert.uid == "daw.sampler" || insert.uid == "daw.equalizer" ||
            insert.uid == "daw.gravity");
}

bool cleanSharedInsert(const InsertModel& source, InsertModel& copy,
                       bool mintId) {
    if (!source.isLoaded() || !supportedSharedBuiltin(source)) return false;
    copy = source;
    if (mintId) copy.id = newUuid();
    copy.path.clear();
    copy.stateFile.clear();
    copy.rightStateFile.clear();
    copy.editorChannel = PluginEditorChannel::Left;
    copy.windowX = copy.windowY = copy.windowWidth = copy.windowHeight = 0;
    copy.windowOpen = false;
    return true;
}

void setSharedSampleBinding(InsertModel& insert, const AssetRef& asset) {
    auto found = std::find_if(
        insert.assetBindings.begin(), insert.assetBindings.end(),
        [](const PluginAssetBinding& binding) {
            return binding.key == "sample";
        });
    PluginAssetBinding binding{"sample", asset, true};
    if (found == insert.assetBindings.end())
        insert.assetBindings.push_back(std::move(binding));
    else
        *found = std::move(binding);
}

bool appendSharedPluginCopy(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const collab::PluginLocation& location, const InsertModel& source,
    const std::string& afterId, InsertModel* copied = nullptr) {
    InsertModel clean;
    if (!cleanSharedInsert(source, clean, true)) return false;
    appendCommand(batch, collab::AddPluginInsert{location, clean, afterId});
    if (copied) *copied = clean;
    return true;
}

bool appendSharedChainReplacement(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const collab::PluginLocation& location,
    const std::vector<InsertModel>& current,
    const std::vector<EngineController::ChainSlotSnapshot>& source) {
    for (const InsertModel& insert : current) {
        if (!insert.isLoaded() || !supportedSharedBuiltin(insert)) return false;
        appendCommand(batch,
                      collab::DeletePluginInsert{location, insert.id});
    }
    std::string anchor;
    for (const EngineController::ChainSlotSnapshot& slot : source) {
        InsertModel clean;
        if (!appendSharedPluginCopy(batch, location, slot.model, anchor,
                                    &clean)) {
            return false;
        }
        anchor = clean.id;
    }
    return true;
}

bool appendSharedClip(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const std::string& trackId, const ClipModel& clip,
    const std::string& afterId) {
    appendCommand(batch, collab::AddClip{
        trackId, clip.id, clip.kind, clip.name, clip.startSeconds,
        clip.durationSeconds, clip.color, afterId});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clip.id, collab::ClipProperty::OffsetSeconds,
        clip.offsetSeconds});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clip.id, collab::ClipProperty::Gain, double(clip.gain)});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clip.id, collab::ClipProperty::Pan, double(clip.pan)});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clip.id, collab::ClipProperty::Muted, clip.muted});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clip.id, collab::ClipProperty::CompCrossfadeMs,
        clip.compCrossfadeMs});
    appendCommand(batch, collab::SetClipFade{
        trackId, clip.id, clip.fadeInSeconds, clip.fadeOutSeconds});
    appendCommand(batch, collab::SetClipFadeCurve{
        trackId, clip.id, collab::ClipEdge::In, clip.fadeInCurve});
    appendCommand(batch, collab::SetClipFadeCurve{
        trackId, clip.id, collab::ClipEdge::Out, clip.fadeOutCurve});
    appendCommand(batch, collab::SetClipFadeMode{
        trackId, clip.id, collab::ClipEdge::In, clip.fadeInMode});
    appendCommand(batch, collab::SetClipFadeMode{
        trackId, clip.id, collab::ClipEdge::Out, clip.fadeOutMode});
    if (!clip.patternClipId.empty()) {
        appendCommand(batch, collab::SetClipPatternOwner{
            trackId, clip.id, clip.patternClipId});
    }
    if (clip.kind == ClipKind::Audio) {
        appendCommand(batch, collab::SetClipMusicalAnalysis{
            trackId, clip.id, clip.musicalAnalysis});
        if (!clip.asset.empty()) {
            appendCommand(batch,
                          collab::SetClipAsset{trackId, clip.id, clip.asset});
        }
        appendCommand(batch, collab::SetClipSampleEdit{
            trackId, clip.id, clip.sampleEdit});
    }

    std::string pluginAnchor;
    for (const InsertModel& insert : clip.inserts) {
        InsertModel clean;
        if (!cleanSharedInsert(insert, clean, false)) return false;
        appendCommand(batch, collab::AddPluginInsert{
            {collab::PluginChain::Clip, trackId, clip.id}, clean,
            pluginAnchor});
        pluginAnchor = clean.id;
    }

    std::string noteAnchor;
    for (const NoteModel& note : clip.notes) {
        appendCommand(batch, collab::UpsertMidiNote{
            trackId, clip.id, note, noteAnchor});
        noteAnchor = note.id;
    }
    std::string laneAnchor;
    for (const ControllerLane& lane : clip.lanes) {
        appendCommand(batch, collab::AddControllerLane{
            trackId, clip.id, lane.id, lane.name,
            {lane.cc, lane.parameterId, lane.slotId}, lane.defaultValue,
            laneAnchor});
        laneAnchor = lane.id;
        std::string pointAnchor;
        for (const AutomationPoint& point : lane.points) {
            appendCommand(batch, collab::UpsertAutomationPoint{
                trackId, clip.id, lane.id, point, pointAnchor});
            pointAnchor = point.id;
        }
    }
    if (clip.kind == ClipKind::Automation) {
        appendCommand(batch, collab::SetAutomationTarget{
            trackId, clip.id, clip.automation.target});
        appendCommand(batch, collab::SetAutomationDefault{
            trackId, clip.id, clip.automation.defaultValue});
        appendCommand(batch, collab::SetAutomationActive{
            trackId, clip.id, clip.automation.active});
        std::string pointAnchor;
        for (const AutomationPoint& point : clip.automation.points) {
            appendCommand(batch, collab::UpsertAutomationPoint{
                trackId, clip.id, {}, point, pointAnchor});
            pointAnchor = point.id;
        }
    }

    std::string takeAnchor;
    for (TakeModel take : clip.takes) {
        if (!take.notes.empty()) return false;
        take.filePath.clear();
        appendCommand(batch,
                      collab::AddTake{trackId, clip.id, take, takeAnchor});
        takeAnchor = take.id;
    }
    std::string segmentAnchor;
    for (const CompSegment& segment : clip.comp) {
        appendCommand(batch, collab::UpsertCompSegment{
            trackId, clip.id, segment, segmentAnchor});
        segmentAnchor = segment.id;
    }
    return true;
}

bool appendMidiClipContentsDiff(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const std::string& trackId, const ClipModel& before,
    const ClipModel& after) {
    if (before.kind != ClipKind::Midi || after.kind != ClipKind::Midi ||
        before.id != after.id || before.lanes.size() != after.lanes.size()) {
        return false;
    }
    std::unordered_set<std::string> noteIds;
    for (const NoteModel& note : after.notes) noteIds.insert(note.id);
    for (const NoteModel& note : before.notes) {
        if (!noteIds.contains(note.id)) {
            appendCommand(batch, collab::DeleteMidiNote{
                trackId, before.id, note.id});
        }
    }
    std::string noteAnchor;
    for (const NoteModel& note : after.notes) {
        appendCommand(batch, collab::UpsertMidiNote{
            trackId, before.id, note, noteAnchor});
        noteAnchor = note.id;
    }
    for (std::size_t lane = 0; lane < before.lanes.size(); ++lane) {
        if (before.lanes[lane].id != after.lanes[lane].id) return false;
        std::unordered_set<std::string> pointIds;
        for (const AutomationPoint& point : after.lanes[lane].points)
            pointIds.insert(point.id);
        for (const AutomationPoint& point : before.lanes[lane].points) {
            if (!pointIds.contains(point.id)) {
                appendCommand(batch, collab::DeleteAutomationPoint{
                    trackId, before.id, before.lanes[lane].id, point.id});
            }
        }
        std::string pointAnchor;
        for (const AutomationPoint& point : after.lanes[lane].points) {
            appendCommand(batch, collab::UpsertAutomationPoint{
                trackId, before.id, before.lanes[lane].id, point,
                pointAnchor});
            pointAnchor = point.id;
        }
    }
    return true;
}

void mintClipIdentities(
    ClipModel& clip,
    const std::unordered_map<std::string, std::string>& trackSlotIds,
    bool withInserts) {
    clip.id = newUuid();
    std::unordered_map<std::string, std::string> slotIds = trackSlotIds;
    if (withInserts) {
        for (InsertModel& insert : clip.inserts) {
            const std::string before = insert.id;
            insert.id = newUuid();
            slotIds[before] = insert.id;
        }
    } else {
        clip.inserts.clear();
    }
    for (NoteModel& note : clip.notes) note.id = newUuid();
    for (ControllerLane& lane : clip.lanes) {
        lane.id = newUuid();
        if (const auto found = slotIds.find(lane.slotId);
            found != slotIds.end()) {
            lane.slotId = found->second;
        }
        for (AutomationPoint& point : lane.points) point.id = newUuid();
    }
    if (const auto found = slotIds.find(clip.automation.target.slotId);
        found != slotIds.end()) {
        clip.automation.target.slotId = found->second;
    }
    for (AutomationPoint& point : clip.automation.points)
        point.id = newUuid();

    std::unordered_map<std::string, std::string> takeIds;
    for (TakeModel& take : clip.takes) {
        const std::string before = take.id;
        take.id = newUuid();
        takeIds[before] = take.id;
        take.filePath.clear();
        for (NoteModel& note : take.notes) note.id = newUuid();
    }
    for (CompSegment& segment : clip.comp) {
        segment.id = newUuid();
        if (const auto found = takeIds.find(segment.takeId);
            found != takeIds.end()) {
            segment.takeId = found->second;
        }
    }
}

TrackModel mintTrackCopy(const TrackModel& source, bool withInserts) {
    TrackModel copy = source;
    copy.id = newUuid();
    copy.soloed = false;
    std::unordered_map<std::string, std::string> slotIds;
    for (SendModel& send : copy.sends) send.id = newUuid();

    if (copy.instrument.isLoaded()) {
        const std::string before = copy.instrument.id;
        copy.instrument.id = newUuid();
        slotIds[before] = copy.instrument.id;
    }
    if (withInserts) {
        const auto mintSlots = [&](std::vector<InsertModel>& inserts) {
            for (InsertModel& insert : inserts) {
                const std::string before = insert.id;
                insert.id = newUuid();
                slotIds[before] = insert.id;
            }
        };
        mintSlots(copy.inserts);
        mintSlots(copy.samplerFx.inserts);
    } else {
        copy.inserts.clear();
        copy.samplerFx.inserts.clear();
    }
    if (const auto found = slotIds.find(copy.samplerFx.ownerInstrumentId);
        found != slotIds.end()) {
        copy.samplerFx.ownerInstrumentId = found->second;
    }
    for (ClipModel& clip : copy.clips)
        mintClipIdentities(clip, slotIds, withInserts);
    return copy;
}

bool appendSharedTrackContents(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const TrackModel& track) {
    appendCommand(batch, collab::SetTrackProperty{
        track.id, collab::TrackProperty::Volume, double(track.volume)});
    appendCommand(batch, collab::SetTrackProperty{
        track.id, collab::TrackProperty::Pan, double(track.pan)});
    appendCommand(batch, collab::SetTrackProperty{
        track.id, collab::TrackProperty::Muted, track.muted});
    appendCommand(batch, collab::SetTrackProperty{
        track.id, collab::TrackProperty::Mono, track.mono});
    if (track.kind == TrackKind::Folder) {
        appendCommand(batch, collab::SetTrackProperty{
            track.id, collab::TrackProperty::Summing, track.summing});
    }
    if (!track.outputBusId.empty()) {
        appendCommand(batch,
                      collab::SetTrackOutput{track.id, track.outputBusId});
    }
    std::string sendAnchor;
    for (const SendModel& send : track.sends) {
        appendCommand(batch,
                      collab::AddSend{track.id, send, sendAnchor});
        sendAnchor = send.id;
    }
    if (track.instrument.isLoaded()) {
        InsertModel clean;
        if (!cleanSharedInsert(track.instrument, clean, false)) return false;
        appendCommand(batch, collab::AddPluginInsert{
            {collab::PluginChain::Instrument, track.id, {}}, clean, {}});
        appendCommand(batch, collab::SetSamplerFxLevels{
            track.id, clean.id, double(track.samplerFx.volume),
            double(track.samplerFx.pan)});
    }
    std::string pluginAnchor;
    for (const InsertModel& insert : track.samplerFx.inserts) {
        InsertModel clean;
        if (!cleanSharedInsert(insert, clean, false)) return false;
        appendCommand(batch, collab::AddPluginInsert{
            {collab::PluginChain::SamplerFx, track.id, {}}, clean,
            pluginAnchor});
        pluginAnchor = clean.id;
    }
    pluginAnchor.clear();
    for (const InsertModel& insert : track.inserts) {
        InsertModel clean;
        if (!cleanSharedInsert(insert, clean, false)) return false;
        appendCommand(batch, collab::AddPluginInsert{
            {collab::PluginChain::Track, track.id, {}}, clean,
            pluginAnchor});
        pluginAnchor = clean.id;
    }
    std::string clipAnchor;
    for (const ClipModel& clip : track.clips) {
        if (!appendSharedClip(batch, track.id, clip, clipAnchor)) return false;
        clipAnchor = clip.id;
    }
    return true;
}

bool appendSharedTrack(
    const std::shared_ptr<collab::BatchCommand>& batch,
    const TrackModel& track, const std::string& afterId) {
    appendCommand(batch, collab::AddTrack{
        track.id, track.kind, track.name, track.color, track.parentId,
        afterId});
    return appendSharedTrackContents(batch, track);
}

bool sharedBatchApplies(const ProjectModel& project,
                        const std::shared_ptr<collab::BatchCommand>& batch) {
    collab::SharedProjectDocument scratch;
    scratch.project = project;
    collab::ProjectCommand command;
    command.meta.operationId = newUuid();
    command.body = batch;
    return collab::ProjectReducer::apply(scratch, command).accepted();
}

void appendCompDiff(const std::shared_ptr<collab::BatchCommand>& batch,
                    const std::string& trackId, const std::string& clipId,
                    const std::vector<CompSegment>& before,
                    const std::vector<CompSegment>& after);

collab::PluginLocation channelPluginLocation(const std::string& channelId) {
    if (channelId == EngineController::kMasterChannelId)
        return {collab::PluginChain::Master, {}, {}};
    return {collab::PluginChain::Track, channelId, {}};
}

std::string previousIdAt(const std::vector<InsertModel>& values,
                         std::size_t index) {
    return index == 0 || values.empty() ? std::string()
                                        : values[std::min(index, values.size()) - 1].id;
}

bool sameInsertTopology(const InsertModel& left, const InsertModel& right) {
    return left.id == right.id && left.format == right.format &&
           left.uid == right.uid && left.channelMode == right.channelMode &&
           left.sidechainTrackId == right.sidechainTrackId;
}

bool sameInsertChainTopology(const std::vector<InsertModel>& left,
                             const std::vector<InsertModel>& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      sameInsertTopology);
}

bool hasLoadedInsert(const TrackModel& track) {
    const auto anyLoaded = [](const std::vector<InsertModel>& slots) {
        return std::any_of(slots.begin(), slots.end(),
                           [](const InsertModel& slot) {
                               return slot.isLoaded();
                           });
    };
    if (track.instrument.isLoaded() || anyLoaded(track.samplerFx.inserts) ||
        anyLoaded(track.inserts)) {
        return true;
    }
    return std::any_of(track.clips.begin(), track.clips.end(),
                       [&](const ClipModel& clip) {
                           return anyLoaded(clip.inserts);
                       });
}

bool sameTrackTopology(const TrackModel& left, const TrackModel& right) {
    if (left.id != right.id || left.kind != right.kind ||
        left.summing != right.summing || left.parentId != right.parentId ||
        left.outputBusId != right.outputBusId ||
        left.samplerFx.ownerInstrumentId !=
            right.samplerFx.ownerInstrumentId ||
        !sameInsertTopology(left.instrument, right.instrument) ||
        !sameInsertChainTopology(left.samplerFx.inserts,
                                 right.samplerFx.inserts) ||
        !sameInsertChainTopology(left.inserts, right.inserts) ||
        left.sends.size() != right.sends.size()) {
        return false;
    }
    if ((hasLoadedInsert(left) || hasLoadedInsert(right)) &&
        left.mono != right.mono) {
        return false;
    }
    for (std::size_t index = 0; index < left.sends.size(); ++index) {
        const SendModel& a = left.sends[index];
        const SendModel& b = right.sends[index];
        if (a.id != b.id || a.destinationTrackId != b.destinationTrackId ||
            a.preFader != b.preFader) {
            return false;
        }
    }

    const auto clipFx = [](const TrackModel& track) {
        std::vector<const ClipModel*> result;
        for (const ClipModel& clip : track.clips) {
            if (!clip.inserts.empty()) result.push_back(&clip);
        }
        return result;
    };
    const auto leftFx = clipFx(left);
    const auto rightFx = clipFx(right);
    if (leftFx.size() != rightFx.size()) return false;
    for (std::size_t index = 0; index < leftFx.size(); ++index) {
        if (leftFx[index]->id != rightFx[index]->id ||
            !sameInsertChainTopology(leftFx[index]->inserts,
                                     rightFx[index]->inserts)) {
            return false;
        }
    }
    return true;
}

bool sameCollaborationTopology(const ProjectModel& left,
                               const ProjectModel& right) {
    if (!sameInsertChainTopology(left.masterInserts, right.masterInserts) ||
        left.tracks.size() != right.tracks.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.tracks.size(); ++index) {
        if (!sameTrackTopology(left.tracks[index], right.tracks[index]))
            return false;
    }
    return true;
}

} // namespace

// ── Device bridge ──────────────────────────────────────────────────────────

/// Turns one PortAudio callback into one engine block. This is the only place
/// where the platform layer and the engine meet; it does no processing of its
/// own beyond handing the recorder the raw input.
class EngineController::DeviceCallback final : public audio::IAudioCallback {
public:
    DeviceCallback(engine::RealtimeEngine& engine,
                   engine::RealtimeSnapshot<RecorderList>& recorders)
        : m_engine(engine), m_recorders(recorders) {}

    bool writesCompleteOutput() const noexcept override { return true; }

    void onAudioCallback(audio::AudioCallbackContext& ctx) override {
        if (!ctx.outputBuffer) return;
        const engine::FrameCount frames =
            std::min<engine::FrameCount>(ctx.numFrames, ctx.outputBuffer->numFrames());
        if (frames == 0) return;

        // The device buffer is already planar, so the engine renders straight
        // into it — no copy, no interleave.
        const auto deviceChannels = ctx.outputBuffer->numChannels();
        const engine::ChannelCount channels = engine::ChannelCount(
            std::min<std::size_t>(deviceChannels, engine::kMaxChannels));
        for (engine::ChannelCount ch = 0; ch < channels; ++ch) {
            m_outputPointers[ch] = ctx.outputBuffer->getChannel(ch);
        }
        engine::AudioBlock output(m_outputPointers.data(), channels, frames);

        // The graph has a deliberate fixed channel ceiling. A wider device is
        // still safe: render the supported prefix and define every remaining
        // output channel as silence instead of overrunning the pointer array.
        for (std::size_t ch = channels; ch < deviceChannels; ++ch) {
            std::fill_n(ctx.outputBuffer->getChannel(ch), frames, 0.0f);
        }

        engine::ChannelCount inputChannels = 0;
        const float* const* inputChannelData = nullptr;
        if (ctx.inputBuffer) {
            inputChannels = engine::ChannelCount(std::min<std::size_t>(
                ctx.inputBuffer->numChannels(), engine::kMaxChannels));
            for (engine::ChannelCount ch = 0; ch < inputChannels; ++ch) {
                m_inputPointers[ch] = ctx.inputBuffer->getChannel(ch);
            }
            inputChannelData = m_inputPointers.data();
        }

        m_engine.renderBlock(output, inputChannelData, inputChannels, frames);

        // Recording taps the hardware input, not the mix: capturing the master
        // would print everything already on the timeline into the new take.
        // Each armed track has its own recorder, and each picks its own input
        // channels out of this same buffer.
        if (ctx.inputBuffer) {
            auto recorders = m_recorders.read();
            if (recorders) {
                for (const auto& recorder : *recorders) {
                    if (recorder && recorder->isRecording()) {
                        recorder->process(*ctx.inputBuffer, ctx.numFrames);
                    }
                }
            }
        }
    }

private:
    engine::RealtimeEngine& m_engine;
    /// Aliases the controller's published list, so a capture starting or ending
    /// is picked up on the next block without touching this object.
    engine::RealtimeSnapshot<RecorderList>& m_recorders;
    std::array<float*, engine::kMaxChannels> m_outputPointers{};
    std::array<const float*, engine::kMaxChannels> m_inputPointers{};
};

// ── Lifecycle ──────────────────────────────────────────────────────────────

EngineController::EngineController()
    : m_devices(std::make_unique<audio::AudioDeviceManager>()),
      m_recorder(std::make_unique<audio::AudioRecorder>()) {
    m_callback = std::make_unique<DeviceCallback>(m_engine, m_activeRecorders);

    // The built-in sampler reads audio files, and `daw_pluginhost` deliberately
    // links neither libsndfile nor a device layer — so the decoder is handed to
    // it from here, where both already exist. Installed in the constructor and
    // not in `initialize`, because loading a project builds sampler instances
    // before anything else runs.
    plugins::sampler::setSampleDecoder(
        [](const std::string& path) -> std::shared_ptr<const engine::SampleBuffer> {
            audio::platform::DecodedAudio decoded;
            if (!audio::platform::decodeAudioFile(path, decoded) || decoded.frames == 0) {
                return nullptr;
            }
            return engine::SampleBuffer::fromInterleaved(
                decoded.interleaved, engine::ChannelCount(decoded.channels),
                engine::FrameCount(decoded.frames), decoded.sampleRate);
        });
}

std::string EngineController::submitOptimisticSharedAudioClip(
    collab::SharedAssetMutationRequest request, const std::string& trackId,
    ClipModel clip, const std::string& afterId, std::string undoLabel) {
    if (!clip.asset.empty()) return {};
    retireOrphanedPendingAudioImports();
    const std::string clipId = clip.id;
    const std::string requestId = request.requestId;
    const std::string sourcePath = request.sourcePath;

    // Reserve the upload first. prepare() answers synchronously and only ever
    // completes through the event loop, so the clip below is always durable
    // before its asset operation is built. Reserving first also means a refusal
    // (offline, read-only) leaves no permanently silent clip behind.
    PendingSharedAssetMutation pending;
    pending.expected = expectedSharedAudioAsset(request);
    pending.complete = [trackId, clipId, undoLabel](
                           EngineController& controller,
                           const AssetRef& verifiedAsset) {
        // Only the audio is missing by now; the clip itself is already shared.
        // Whether it still exists is the reducer's call, not ours: it rejects a
        // clip.setAsset for a clip that has since been deleted, and duplicating
        // that check here would also depend on the projection having caught up.
        const collab::SharedMutationResult result =
            controller.submitSharedMutation(
                collab::CommandBody{collab::SetClipAsset{trackId, clipId,
                                                         verifiedAsset}},
                undoLabel);
        // The cache now holds the same bytes under the verified hash, so the
        // local override has done its job and the ordinary asset path takes
        // over. Keeping it would pin the original source file forever.
        if (result == collab::SharedMutationResult::Submitted)
            controller.m_pendingLocalAudio.erase(clipId);
        return result;
    };
    if (prepareSharedAssetMutation(std::move(request), std::move(pending)) !=
        collab::SharedMutationResult::Submitted) {
        return {};
    }

    auto batch = std::make_shared<collab::BatchCommand>();
    if (!appendSharedClip(batch, trackId, clip, afterId) ||
        !sharedBatchApplies(m_project, batch)) {
        cancelSharedAssetMutation(requestId);
        return {};
    }
    m_pendingLocalAudio[clipId] = {sourcePath, requestId};
    if (submitSharedMutation(collab::CommandBody{std::move(batch)},
                             std::move(undoLabel)) !=
        collab::SharedMutationResult::Submitted) {
        m_pendingLocalAudio.erase(clipId);
        cancelSharedAssetMutation(requestId);
        return {};
    }
    return clipId;
}

std::string EngineController::submitOptimisticSharedAudioTrack(
    collab::SharedAssetMutationRequest request, TrackModel track,
    const std::string& afterId) {
    if (track.clips.size() != 1 || !track.clips.front().asset.empty())
        return {};
    const std::string trackId = track.id;
    const std::string clipId = track.clips.front().id;
    const std::string requestId = request.requestId;
    const std::string sourcePath = request.sourcePath;
    static constexpr const char* kLabel = "Import Audio to New Track";

    PendingSharedAssetMutation pending;
    pending.expected = expectedSharedAudioAsset(request);
    pending.complete = [trackId, clipId](EngineController& controller,
                                         const AssetRef& verifiedAsset) {
        const collab::SharedMutationResult result =
            controller.submitSharedMutation(
                collab::CommandBody{collab::SetClipAsset{trackId, clipId,
                                                         verifiedAsset}},
                kLabel);
        if (result == collab::SharedMutationResult::Submitted)
            controller.m_pendingLocalAudio.erase(clipId);
        return result;
    };
    if (prepareSharedAssetMutation(std::move(request), std::move(pending)) !=
        collab::SharedMutationResult::Submitted) {
        return {};
    }

    auto batch = std::make_shared<collab::BatchCommand>();
    if (!appendSharedTrack(batch, track, afterId) ||
        !sharedBatchApplies(m_project, batch)) {
        cancelSharedAssetMutation(requestId);
        return {};
    }
    m_pendingLocalAudio[clipId] = {sourcePath, requestId};
    if (submitSharedMutation(collab::CommandBody{std::move(batch)}, kLabel) !=
        collab::SharedMutationResult::Submitted) {
        m_pendingLocalAudio.erase(clipId);
        cancelSharedAssetMutation(requestId);
        return {};
    }
    return trackId;
}

std::string EngineController::pendingLocalAudioPath(
    const std::string& clipId) const {
    const auto found = m_pendingLocalAudio.find(clipId);
    return found == m_pendingLocalAudio.end() ? std::string()
                                              : found->second.sourcePath;
}

void EngineController::retireOrphanedPendingAudioImports() {
    if (m_pendingLocalAudio.empty()) return;
    std::vector<std::string> orphaned;
    for (const auto& [clipId, pending] : m_pendingLocalAudio) {
        bool live = false;
        for (const TrackModel& track : m_project.tracks) {
            for (const ClipModel& clip : track.clips) {
                if (clip.id == clipId) { live = true; break; }
            }
            if (live) break;
        }
        if (!live) orphaned.push_back(clipId);
    }
    for (const std::string& clipId : orphaned) {
        const std::string requestId = m_pendingLocalAudio[clipId].requestId;
        m_pendingLocalAudio.erase(clipId);
        if (!requestId.empty()) cancelSharedAssetMutation(requestId);
    }
}

collab::SharedMutationResult EngineController::submitSharedMutation(
    collab::CommandBody body, std::string undoLabel,
    std::optional<std::string> transactionId) {
    if (!m_sharedMutationSink)
        return collab::SharedMutationResult::LocalFallback;
    return m_sharedMutationSink->submit(collab::SharedMutationRequest{
        std::move(body), std::move(undoLabel), std::move(transactionId)});
}

collab::SharedMutationResult EngineController::prepareSharedAssetMutation(
    collab::SharedAssetMutationRequest request,
    PendingSharedAssetMutation pending) {
    if (!cloudProjectBound() || !m_sharedAssetMutationSink ||
        request.requestId.empty() || request.assetId.empty() ||
        request.sourcePath.empty() || !pending.complete ||
        pending.expected.assetId != request.assetId) {
        return collab::SharedMutationResult::Blocked;
    }

    const std::string requestId = request.requestId;
    const auto [_, inserted] = m_pendingSharedAssetMutations.emplace(
        requestId, std::move(pending));
    if (!inserted) return collab::SharedMutationResult::Blocked;

    const auto result = m_sharedAssetMutationSink->prepare(std::move(request));
    if (result == collab::SharedMutationResult::Submitted) return result;

    // A sink is allowed to reject synchronously. It may also complete
    // synchronously in a deterministic test, hence the second lookup.
    const auto found = m_pendingSharedAssetMutations.find(requestId);
    if (found != m_pendingSharedAssetMutations.end()) {
        if (!found->second.cleanupPath.empty()) {
            std::error_code error;
            fs::remove(platform::pathFromUtf8(found->second.cleanupPath), error);
        }
        m_pendingSharedAssetMutations.erase(found);
    }
    return collab::SharedMutationResult::Blocked;
}

collab::SharedMutationResult EngineController::completeSharedAssetMutation(
    const std::string& requestId, const AssetRef& verifiedAsset) {
    const auto found = m_pendingSharedAssetMutations.find(requestId);
    if (found == m_pendingSharedAssetMutations.end())
        return collab::SharedMutationResult::Blocked;

    PendingSharedAssetMutation pending = std::move(found->second);
    m_pendingSharedAssetMutations.erase(found);
    const bool valid = completeSharedAudioAsset(verifiedAsset, pending.expected);
    const auto result = valid
        ? pending.complete(*this, verifiedAsset)
        : collab::SharedMutationResult::Blocked;
    if (!pending.cleanupPath.empty()) {
        std::error_code error;
        fs::remove(platform::pathFromUtf8(pending.cleanupPath), error);
    }
    return result;
}

void EngineController::cancelSharedAssetMutation(
    const std::string& requestId) {
    const auto found = m_pendingSharedAssetMutations.find(requestId);
    if (found == m_pendingSharedAssetMutations.end()) return;
    if (!found->second.cleanupPath.empty()) {
        std::error_code error;
        fs::remove(platform::pathFromUtf8(found->second.cleanupPath), error);
    }
    m_pendingSharedAssetMutations.erase(found);
}

bool EngineController::cloudProjectBound() {
    return m_sharedMutationSink && m_sharedMutationSink->handlesCloudBinding();
}

EngineController::~EngineController() {
    while (!m_pendingSharedAssetMutations.empty())
        cancelSharedAssetMutation(m_pendingSharedAssetMutations.begin()->first);
    shutdown();
}

audio::Result EngineController::initialize(double sampleRate,
                                           uint32_t bufferSize,
                                           bool openDevice) {
    audio::AudioDeviceConfig config;
    config.sampleRate = sampleRate;
    config.bufferSize = bufferSize;
    return initialize(config, openDevice);
}

audio::Result EngineController::initialize(
    const audio::AudioDeviceConfig& config, bool openDevice) {
    m_liveDeviceAllowed = openDevice;
    m_sampleRate = config.sampleRate;
    m_bufferSize = config.bufferSize;
    m_project.sampleRate = config.sampleRate;
    m_recordDir = platform::pathToUtf8(fs::temp_directory_path());

    m_engine.prepare(m_sampleRate, m_bufferSize, 2);
    m_engine.transport().setTempo(m_project.tempo);
    m_engine.transport().setTimeSignature(m_project.timeSigNumerator,
                                          m_project.timeSigDenominator);
    m_recorder->initialize(m_sampleRate, 2);
    // The scan results are read here and not in the constructor, so a caller
    // that only wants the document model never touches the user's cache file.
    // Without this the plugin browser is empty until the first scan of the
    // session, and a project's inserts fail to resolve their descriptors.
    m_pluginManager.load();
    rebuildGraph();
    m_prepared = true;

    if (!openDevice) return audio::Result::ok();

    auto r = m_devices->initialize(config);
    if (!r) return r;                       // offline path stays usable
    m_devices->setAudioCallback(m_callback.get());
    auto started = m_devices->start();
    if (!started) return started;

    // The device may have opened at a different rate than we asked for.
    m_sampleRate = m_devices->sampleRate();
    m_bufferSize = m_devices->bufferSize();
    m_project.sampleRate = m_sampleRate;
    m_engine.prepare(m_sampleRate, m_bufferSize, 2);
    m_recorder->initialize(m_sampleRate, 2);
    rebuildGraph();
    m_deviceOpen = true;
    return audio::Result::ok();
}

void EngineController::shutdown() {
    if (m_devices->isInitialized()) {
        m_devices->setAudioCallback(nullptr);
        m_devices->stop();
        m_devices->shutdown();
        m_deviceOpen = false;
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────

engine::SamplePos EngineController::toSamples(double seconds) const {
    return engine::SamplePos(std::llround(seconds * m_sampleRate));
}

double EngineController::toSeconds(engine::SamplePos samples) const {
    return m_sampleRate > 0.0 ? double(samples) / m_sampleRate : 0.0;
}

std::shared_ptr<const engine::SampleBuffer> EngineController::loadSamples(
    const std::string& path) {
    if (path.empty()) return nullptr;
    auto found = m_samples.find(path);
    if (found != m_samples.end()) return found->second;

    // A removed clip can leave the cache as the final owner of a large decoded
    // file. Reclaim such entries before admitting another one; buffers still
    // referenced by a live/retired graph snapshot have use_count > 1 and are
    // deliberately left alone until a later load.
    pruneDecodedSampleCache();

    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(path, decoded) || decoded.frames == 0) {
        m_samples[path] = nullptr;          // remember the failure
        return nullptr;
    }
    m_waveforms.storeDecoded(path, decoded);

    // Session-rate conversion is immutable project data, so doing it once at
    // import/load removes a scalar interpolation loop from every subsequent
    // audio block. The peak cache above deliberately describes the original
    // source before this conversion.
    //
    // A windowed-sinc polyphase conversion, not an interpolator. This path used
    // to be about the occasional oddly-rated import; rendering at a chosen
    // sample rate now sends every sample in the session through it, and a
    // four-point cubic measures about -24 dB of error at 12 kHz where this
    // measures -113 dB.
    if (decoded.sampleRate > 0.0 && m_sampleRate > 0.0 &&
        std::abs(decoded.sampleRate - m_sampleRate) > 0.01) {
        decoded.interleaved = engine::dsp::resampleInterleaved(
            decoded.interleaved, decoded.channels, decoded.frames,
            decoded.sampleRate, m_sampleRate);
        decoded.frames = audio::FrameCount(engine::dsp::resampledFrameCount(
            decoded.frames, decoded.sampleRate, m_sampleRate));
        decoded.sampleRate = m_sampleRate;
    }
    auto buffer = engine::SampleBuffer::fromInterleaved(
        decoded.interleaved, engine::ChannelCount(decoded.channels),
        engine::FrameCount(decoded.frames), decoded.sampleRate);
    m_samples[path] = buffer;
    return buffer;
}

void EngineController::pruneDecodedSampleCache() {
    std::erase_if(m_samples, [](const auto& entry) {
        const auto& sample = entry.second;
        return sample && sample.use_count() == 1;
    });
}

std::shared_ptr<const plugins::sampler::SampleData>
EngineController::processedClipSample(
    const ClipModel& clip, const std::string& path,
    std::shared_ptr<const engine::SampleBuffer> raw) {
    if (!raw || raw->frames() == 0) return {};
    const plugins::sampler::PrecomputeSettings settings =
        clipPrecomputeSettings(clip.sampleEdit);
    auto found = m_clipSampleCache.find(clip.id);
    if (found != m_clipSampleCache.end() && found->second.path == path &&
        found->second.settings == settings) {
        return found->second.data;
    }

    for (auto it = m_sharedClipSampleCache.begin();
         it != m_sharedClipSampleCache.end();) {
        std::shared_ptr<const plugins::sampler::SampleData> shared =
            it->data.lock();
        if (!shared) {
            it = m_sharedClipSampleCache.erase(it);
            continue;
        }
        if (it->path == path && it->settings == settings) {
            m_clipSampleCache[clip.id] = {path, settings, shared};
            return shared;
        }
        ++it;
    }

    auto data = std::make_shared<plugins::sampler::SampleData>();
    data->path = path;
    data->name = platform::pathToUtf8(platform::pathFromUtf8(path).filename());
    if (settings.isNeutral()) {
        data->audio = std::move(raw);
        data->baseFrames = data->audio->frames();
    } else {
        engine::FrameCount baseFrames = raw->frames();
        data->audio = plugins::sampler::precompute(*raw, settings, baseFrames);
        data->baseFrames = baseFrames;
    }
    if (!data->audio) return {};
    m_clipSampleCache[clip.id] = {path, settings, data};
    m_sharedClipSampleCache.push_back({path, settings, data});
    return data;
}

const ClipModel* EngineController::audioClip(const std::string& trackId,
                                              const std::string& clipId) const {
    const TrackModel* track = m_project.findTrack(trackId);
    if (!track) return nullptr;
    for (const ClipModel& clip : track->clips) {
        if (clip.id == clipId && clip.kind == ClipKind::Audio) return &clip;
    }
    return nullptr;
}

bool EngineController::setClipAudioFile(const std::string& trackId,
                                         const std::string& clipId,
                                         const std::string& filePath) {
    TrackModel* track = m_project.findTrack(trackId);
    ClipModel* clip = findClip(trackId, clipId);
    if (!track || !clip || clip->kind != ClipKind::Audio) return false;

    std::shared_ptr<const engine::SampleBuffer> replacement;
    if (!filePath.empty()) {
        replacement = loadSamples(filePath);
        if (!replacement || replacement->sampleRate() <= 0.0) return false;
    }
    const ClipModel before = *clip;
    ClipModel after = before;
    after.filePath = filePath;
    after.musicalAnalysis = {};
    if (!filePath.empty()) {
        const std::string previousFile = platform::pathToUtf8(
            platform::pathFromUtf8(before.filePath).filename());
        if (after.name.empty() || after.name == previousFile) {
            after.name = platform::pathToUtf8(
                platform::pathFromUtf8(filePath).filename());
        }
        after.channels = int(replacement->channels());
        after.offsetSeconds = 0.0;
        after.durationSeconds =
            double(replacement->frames()) / replacement->sampleRate() *
            std::max(after.sampleEdit.stretchTime, 0.01);
        after.fadeInSeconds = std::min(after.fadeInSeconds, after.durationSeconds);
        after.fadeOutSeconds = std::min(after.fadeOutSeconds, after.durationSeconds);
    } else {
        after.channels = 0;
    }

    if (cloudProjectBound()) {
        // This used to write straight into m_project and push onto the legacy
        // undo stack, which is disabled under a cloud binding — so replacing a
        // clip's audio was invisible to everyone else and could not be undone.
        // It follows the optimistic import shape: the new geometry and the
        // cleared asset go out now, the verified asset follows.
        const std::string label =
            filePath.empty() ? "Clear Clip Sample" : "Replace Clip Sample";
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::Name, after.name});
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::OffsetSeconds,
            after.offsetSeconds});
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::DurationSeconds,
            after.durationSeconds});
        appendCommand(batch, collab::SetClipFade{
            trackId, clipId, after.fadeInSeconds, after.fadeOutSeconds});
        appendCommand(batch, collab::SetClipMusicalAnalysis{
            trackId, clipId, after.musicalAnalysis});
        // Dropping the old asset is the honest intermediate state: the clip is
        // being given different audio, so it is silent for everyone until the
        // replacement is durable. The importer keeps hearing it locally.
        appendCommand(batch,
                      collab::SetClipAsset{trackId, clipId, AssetRef{}});
        if (!sharedBatchApplies(m_project, batch)) return false;

        if (filePath.empty()) {
            return submitSharedMutation(collab::CommandBody{std::move(batch)},
                                        label) ==
                   collab::SharedMutationResult::Submitted;
        }

        auto request = sharedAudioRequest(filePath);
        if (!request) return false;
        const std::string requestId = request->requestId;
        PendingSharedAssetMutation pending;
        pending.expected = expectedSharedAudioAsset(*request);
        pending.complete = [trackId, clipId, label](
                               EngineController& controller,
                               const AssetRef& verifiedAsset) {
            const collab::SharedMutationResult result =
                controller.submitSharedMutation(
                    collab::CommandBody{collab::SetClipAsset{
                        trackId, clipId, verifiedAsset}},
                    label);
            if (result == collab::SharedMutationResult::Submitted)
                controller.m_pendingLocalAudio.erase(clipId);
            return result;
        };
        if (prepareSharedAssetMutation(std::move(*request),
                                       std::move(pending)) !=
            collab::SharedMutationResult::Submitted) {
            return false;
        }
        m_pendingLocalAudio[clipId] = {filePath, requestId};
        if (submitSharedMutation(collab::CommandBody{std::move(batch)},
                                 label) !=
            collab::SharedMutationResult::Submitted) {
            m_pendingLocalAudio.erase(clipId);
            cancelSharedAssetMutation(requestId);
            return false;
        }
        return true;
    }

    auto apply = [this, trackId, clipId](const ClipModel& value) {
        TrackModel* owner = m_project.findTrack(trackId);
        ClipModel* target = findClip(trackId, clipId);
        if (!owner || !target) return;
        *target = value;
        m_clipSampleCache.erase(clipId);
        syncTrackClips(*owner);
        updateTimelineDuration();
    };
    apply(after);
    m_undo.push(filePath.empty() ? "Clear Clip Sample" : "Replace Clip Sample",
                [apply, before] { apply(before); },
                [apply, after] { apply(after); });
    return true;
}

bool EngineController::setClipMusicalAnalysis(
    const std::string& trackId, const std::string& clipId,
    const ClipMusicalAnalysisModel& analysis, const std::string& label) {
    ClipModel* clip = findClip(trackId, clipId);
    if (!clip || clip->kind != ClipKind::Audio) return false;
    const auto shared = submitSharedMutation(
        collab::SetClipMusicalAnalysis{trackId, clipId, analysis}, label);
    if (shared == collab::SharedMutationResult::Submitted) return true;
    if (shared == collab::SharedMutationResult::Blocked) return false;
    const ClipMusicalAnalysisModel before = clip->musicalAnalysis;
    clip->musicalAnalysis = analysis;
    auto apply = [this, trackId, clipId](const ClipMusicalAnalysisModel& value) {
        if (ClipModel* target = findClip(trackId, clipId))
            target->musicalAnalysis = value;
    };
    m_undo.push(label,
                [apply, before] { apply(before); },
                [apply, analysis] { apply(analysis); });
    return true;
}

std::shared_ptr<const plugins::sampler::SampleData>
EngineController::clipSampleData(const std::string& trackId,
                                 const std::string& clipId) {
    const ClipModel* clip = audioClip(trackId, clipId);
    if (!clip || isLayered(*clip) || clip->filePath.empty()) return {};
    return processedClipSample(*clip, clip->filePath, loadSamples(clip->filePath));
}

void EngineController::updateTimelineDuration() {
    double maxEnd = 0.0;
    for (const auto& t : m_project.tracks) {
        for (const auto& c : t.clips) {
            maxEnd = std::max(maxEnd, c.startSeconds + c.durationSeconds);
        }
    }
    m_engine.transport().setDuration(toSamples(maxEnd));
}

double EngineController::effectiveClipLength(const ClipModel& clip) {
    if (clip.durationSeconds > 0.0) return clip.durationSeconds;
    if (auto samples = loadSamples(clip.filePath);
        samples && samples->sampleRate() > 0.0) {
        return double(samples->frames()) / samples->sampleRate() -
               clip.offsetSeconds;
    }
    return 0.0;
}

EngineController::PlacementSpan EngineController::emitClipPlacements(
    const ClipModel& clip, engine::ClipPlayerNode::ClipList& list) {
    PlacementSpan span;
    span.first = list.size();

    if (!isLayered(clip)) {
        auto raw = loadSamples(clip.filePath);
        auto edited = processedClipSample(clip, clip.filePath, std::move(raw));
        if (!edited || !edited->audio) return span;
        engine::ClipPlacement placement;
        placement.audio = edited->audio;
        placement.startSample = toSamples(clip.startSeconds);
        placement.offsetSamples = toSamples(clip.offsetSeconds);
        const double sourceRate = placement.audio->sampleRate() > 0.0
                                      ? placement.audio->sampleRate()
                                      : m_sampleRate;
        placement.sourceStartFrame = clip.offsetSeconds * sourceRate;
        double outputSeconds = clip.durationSeconds;
        if (outputSeconds <= 0.0) {
            const double sourceSeconds =
                std::max(0.0, double(edited->baseFrames) / sourceRate - clip.offsetSeconds);
            outputSeconds = sourceSeconds * clip.sampleEdit.stretchTime;
        }
        const double sourceSeconds = outputSeconds /
            std::max(clip.sampleEdit.stretchTime, 0.01);
        placement.sourceEndFrame = std::min<double>(
            edited->baseFrames, placement.sourceStartFrame + sourceSeconds * sourceRate);
        placement.lengthSamples = toSamples(outputSeconds);
        placement.fadeInSamples = toSamples(std::max(0.0, clip.fadeInSeconds));
        placement.fadeOutSamples = toSamples(std::max(0.0, clip.fadeOutSeconds));
        placement.fadeInCurve = float(clip.fadeInCurve);
        placement.fadeOutCurve = float(clip.fadeOutCurve);
        placement.tapeStartSamples = clip.fadeInMode == ClipFadeMode::Tape
                                         ? placement.fadeInSamples : 0;
        placement.tapeStopSamples = clip.fadeOutMode == ClipFadeMode::Tape
                                        ? placement.fadeOutSamples : 0;
        placement.stretchMode = int(clip.sampleEdit.stretchMode);
        placement.stretchTime = clip.sampleEdit.stretchTime;
        placement.stretchPitch = clip.sampleEdit.stretchPitch;
        placement.formant = clip.sampleEdit.formant;
        placement.loopMode = clip.sampleEdit.loopMode;
        placement.loopStart = clip.sampleEdit.loopStart;
        placement.loopEnd = clip.sampleEdit.loopEnd;
        placement.gain = clip.gain;
        placement.pan = clip.pan;
        placement.muted = clip.muted;
        span.startSample = placement.startSample;
        span.endSample = placement.startSample + toSamples(effectiveClipLength(clip));
        span.count = 1;
        list.push_back(std::move(placement));
        return span;
    }

    // A layered clip plays its comp, not its own file: one placement per
    // segment, each reading the take that segment names.
    //
    // Comp segments butt up against each other, so a crossfade at a seam has to
    // *extend* both sides past it — half the fade each way — and there has to be
    // material there to extend into. That is what bounds the fade below, along
    // with the segment lengths themselves (a 5 ms fade across a 3 ms sliver
    // would run past both ends).
    const double crossfade =
        std::clamp(clip.compCrossfadeMs, 0.0, 20.0) / 1000.0;

    // Auditioning one take replaces the comp for as long as it lasts, so the
    // headphone button works while the transport is already rolling.
    std::vector<CompSegment> audition;
    if (!m_soloTakeId.empty() && m_soloClipId == clip.id) {
        if (const TakeModel* solo = findTake(clip, m_soloTakeId)) {
            const double end = solo->lengthSeconds > 0.0
                ? solo->clipOffsetSeconds + solo->lengthSeconds
                : effectiveClipLength(clip);
            CompSegment segment;
            segment.takeId = m_soloTakeId;
            segment.startSeconds = solo->clipOffsetSeconds;
            segment.endSeconds = end;
            // This segment exists only while auditioning and is never part of
            // the shared document, so it deliberately has no durable id.
            audition.push_back(std::move(segment));
        }
    }
    const std::vector<CompSegment>& segments = audition.empty() ? clip.comp : audition;

    struct Piece {
        size_t index;          // into `list`
        double start;          // clip-relative seconds
        double end;
        double takeStart;      // where the take's material begins, clip-relative
        double takeEnd;        // ... and ends
    };
    std::vector<Piece> pieces;
    pieces.reserve(segments.size());

    for (const CompSegment& segment : segments) {
        const TakeModel* take = findTake(clip, segment.takeId);
        // A soloed take is heard even when muted — that is what auditioning is
        // for; otherwise a muted take is silent.
        if (!take || (take->muted && audition.empty())) continue;
        auto samples = loadSamples(take->filePath);
        if (!samples || samples->sampleRate() <= 0.0) continue;

        // How much of the take there actually is, in clip-relative time.
        const double sourceLength =
            double(samples->frames()) / samples->sampleRate() - take->offsetSeconds;
        const double available =
            take->lengthSeconds > 0.0 ? std::min(take->lengthSeconds, sourceLength)
                                      : sourceLength;
        const double takeStart = take->clipOffsetSeconds;
        const double takeEnd = takeStart + std::max(0.0, available);

        // A segment can name a stretch the take does not cover — a punch-in take
        // trimmed to its recorded region, say — so it is clipped to the material.
        const double start = std::max(segment.startSeconds, takeStart);
        const double end = std::min(segment.endSeconds, takeEnd);
        if (end - start <= 0.0) continue;

        engine::ClipPlacement placement;
        placement.audio = std::move(samples);
        placement.startSample = toSamples(clip.startSeconds + start);
        placement.offsetSamples =
            toSamples(take->offsetSeconds + (start - takeStart));
        placement.lengthSamples = toSamples(end - start);
        placement.gain = clip.gain * take->gain;
        placement.pan = clip.pan;
        placement.muted = clip.muted;
        pieces.push_back({list.size(), start, end, takeStart, takeEnd});
        list.push_back(std::move(placement));
    }

    if (pieces.empty()) return span;

    for (size_t k = 0; k + 1 < pieces.size(); ++k) {
        Piece& earlier = pieces[k];
        Piece& later = pieces[k + 1];
        if (std::abs(later.start - earlier.end) > 1e-9) continue;  // not a seam
        const double half = std::min({
            crossfade * 0.5,
            (earlier.end - earlier.start) * 0.5,
            (later.end - later.start) * 0.5,
            earlier.takeEnd - earlier.end,     // material after the earlier piece
            later.start - later.takeStart,     // material before the later piece
        });
        if (half <= 0.0) continue;

        engine::ClipPlacement& a = list[earlier.index];
        engine::ClipPlacement& b = list[later.index];
        a.lengthSamples += toSamples(half);
        a.fadeOutSamples = toSamples(half * 2.0);
        a.fadeEqualPower = true;
        b.startSample -= toSamples(half);
        b.offsetSamples -= toSamples(half);
        b.lengthSamples += toSamples(half);
        b.fadeInSamples = toSamples(half * 2.0);
        b.fadeEqualPower = true;
    }

    // The user's own fades belong to the clip as a whole, so they land on the
    // outermost pieces rather than on every segment.
    engine::ClipPlacement& head = list[pieces.front().index];
    engine::ClipPlacement& tail = list[pieces.back().index];
    if (clip.fadeInSeconds > 0.0) {
        head.fadeInSamples =
            std::max(head.fadeInSamples, toSamples(clip.fadeInSeconds));
        head.fadeInCurve = float(clip.fadeInCurve);
        head.tapeStartSamples = clip.fadeInMode == ClipFadeMode::Tape
                                    ? toSamples(clip.fadeInSeconds) : 0;
    }
    if (clip.fadeOutSeconds > 0.0) {
        tail.fadeOutSamples =
            std::max(tail.fadeOutSamples, toSamples(clip.fadeOutSeconds));
        tail.fadeOutCurve = float(clip.fadeOutCurve);
        tail.tapeStopSamples = clip.fadeOutMode == ClipFadeMode::Tape
                                   ? toSamples(clip.fadeOutSeconds) : 0;
    }

    span.count = list.size() - span.first;
    span.startSample = head.startSample;
    span.endSample = toSamples(clip.startSeconds + pieces.back().end);
    return span;
}

void EngineController::syncTrackNotes(const TrackModel& track,
                                      bool geometryChanged) {
    if (geometryChanged) bumpMidiNotesRevision(track.id);
    auto found = m_channels.find(track.id);
    if (found == m_channels.end() || !found->second.midiClips) return;

    auto notes = std::make_shared<engine::MidiClipPlayerNode::NoteList>();
    const double beatsPerSecond = m_project.tempo / 60.0;

    // Reserve once and collect only the Pattern owners this track actually
    // references. Previously every child clip rescanned every track and every
    // Pattern clip in the project, which made a single piano-roll publication
    // quadratic in project size.
    std::size_t noteCapacity = 0;
    std::unordered_set<std::string> referencedPatternIds;
    for (const ClipModel& clip : track.clips) {
        if (clip.kind != ClipKind::Midi || clip.muted) continue;
        noteCapacity += clip.notes.size();
        if (!clip.patternClipId.empty()) {
            if (referencedPatternIds.empty())
                referencedPatternIds.reserve(track.clips.size());
            referencedPatternIds.insert(clip.patternClipId);
        }
    }
    notes->reserve(noteCapacity);

    struct PatternGate {
        double startBeats = 0.0;
        double endBeats = 0.0;
        bool muted = false;
    };
    std::unordered_map<std::string, PatternGate> patternGates;
    patternGates.reserve(referencedPatternIds.size());
    if (!referencedPatternIds.empty()) {
        for (const TrackModel& ownerTrack : m_project.tracks) {
            if (ownerTrack.kind != TrackKind::Pattern) continue;
            for (const ClipModel& owner : ownerTrack.clips) {
                if (owner.kind != ClipKind::Pattern ||
                    !referencedPatternIds.contains(owner.id)) {
                    continue;
                }
                // Keep the first matching owner, matching the old scan's
                // behaviour even if a malformed document duplicates an id.
                patternGates.try_emplace(
                    owner.id,
                    PatternGate{owner.startSeconds * beatsPerSecond,
                                (owner.startSeconds + owner.durationSeconds) *
                                    beatsPerSecond,
                                owner.muted});
            }
        }
    }

    for (const ClipModel& clip : track.clips) {
        if (clip.kind != ClipKind::Midi || clip.muted) continue;
        // A clip's position is in seconds and a note's is in beats from the
        // clip's start; the node wants beats from the start of the timeline.
        const double clipStartBeats = clip.startSeconds * beatsPerSecond;
        const double clipLengthBeats =
            clip.durationSeconds > 0.0 ? clip.durationSeconds * beatsPerSecond : 0.0;

        double gateStartBeats = clipStartBeats;
        double gateEndBeats = clipLengthBeats > 0.0
                                  ? clipStartBeats + clipLengthBeats
                                  : std::numeric_limits<double>::max();
        if (!clip.patternClipId.empty()) {
            const auto owner = patternGates.find(clip.patternClipId);
            if (owner != patternGates.end()) {
                if (owner->second.muted) {
                    gateEndBeats = gateStartBeats;
                } else {
                    gateStartBeats =
                        std::max(gateStartBeats, owner->second.startBeats);
                    gateEndBeats =
                        std::min(gateEndBeats, owner->second.endBeats);
                }
            }
        }

        for (const NoteModel& note : clip.notes) {
            if (note.muted) continue;
            // Notes past the clip's end are not played, and one that runs over
            // it is cut there — the clip's boundary is what the user drew.
            if (clipLengthBeats > 0.0 && note.startBeats >= clipLengthBeats) continue;
            const double sourceLength =
                clipLengthBeats > 0.0
                    ? std::min(note.lengthBeats, clipLengthBeats - note.startBeats)
                    : note.lengthBeats;
            if (!(sourceLength > 0.0)) continue;

            const double noteStart = clipStartBeats + note.startBeats;
            const double noteEnd = noteStart + sourceLength;
            const double audibleStart = std::max(noteStart, gateStartBeats);
            const double audibleEnd = std::min(noteEnd, gateEndBeats);
            if (!(audibleEnd > audibleStart)) continue;

            engine::MidiNote out;
            out.startBeats = audibleStart;
            out.lengthBeats = audibleEnd - audibleStart;
            out.key = std::uint8_t(std::clamp(note.pitch, 0, 127));
            out.velocity = std::uint8_t(std::clamp(note.velocity, 1, 127));
            out.pan = std::clamp(note.pan, -1.0f, 1.0f);
            notes->push_back(out);
        }
    }

    const auto startsBefore = [](const engine::MidiNote& a,
                                 const engine::MidiNote& b) {
        return a.startBeats < b.startBeats;
    };
    // Imported files, recorded takes and ordinary one-clip lanes are already
    // chronological. Checking that linear fast path avoids an N-log-N sort on
    // every velocity/mute publication of a 100k-note clip; genuinely reordered
    // edits and overlapping unsorted clips still take the full sort once.
    if (!std::is_sorted(notes->begin(), notes->end(), startsBefore))
        std::sort(notes->begin(), notes->end(), startsBefore);
    found->second.midiClips->setNotes(std::move(notes));
}

void EngineController::writeAutomationPoint(const std::string& channelId,
                                            const std::string& slotId,
                                            const std::string& parameterId,
                                            double value) {
    if (!m_automationWrite || !isPlaying()) return;
    TrackModel* track = m_project.findTrack(channelId);
    if (!track) return;

    const double beatsPerSecond = m_project.tempo / 60.0;
    const double playheadBeats = positionSeconds() * beatsPerSecond;
    // The instrument's own slot id is what a lane with an empty `slotId`
    // means, so the two have to be compared as the same thing.
    const bool isInstrument = track->instrument.id == slotId;

    bool touched = false;
    for (ClipModel& clip : track->clips) {
        if (clip.kind != ClipKind::Midi) continue;
        const double clipStartBeats = clip.startSeconds * beatsPerSecond;
        const double clipEndBeats =
            clip.durationSeconds > 0.0
                ? clipStartBeats + clip.durationSeconds * beatsPerSecond
                : std::numeric_limits<double>::max();
        // Only the clip the playhead is inside: a breakpoint written outside
        // the clip that owns the lane would never be played back.
        if (playheadBeats < clipStartBeats || playheadBeats >= clipEndBeats) continue;

        for (ControllerLane& lane : clip.lanes) {
            if (lane.cc >= 0 || lane.parameterId != parameterId) continue;
            const bool matches = lane.slotId.empty() ? isInstrument : lane.slotId == slotId;
            if (!matches) continue;

            // Back to 0…1, the other half of the mapping in
            // `syncTrackAutomation`: what the plugin reported is in plain units
            // and the lane stores normalised.
            double normalized = value;
            if (plugins::PluginNode* node = insertNode(channelId, slotId)) {
                if (plugins::PluginInstance* live = node->instance()) {
                    const std::int32_t index = live->parameterIndexForId(parameterId);
                    if (index >= 0) {
                        const plugins::ParameterInfo& info =
                            live->parameters()[std::size_t(index)];
                        const double span = info.maxValue - info.minValue;
                        normalized = span > 0.0 ? (value - info.minValue) / span : 0.0;
                    }
                }
            }
            normalized = std::clamp(normalized, 0.0, 1.0);

            AutomationPoint point;
            point.beats = playheadBeats - clipStartBeats;
            point.value = normalized;
            // Replace a point at effectively the same instant rather than
            // stacking a pile of them: a knob drag fires far faster than the
            // curve needs resolving.
            constexpr double kMergeBeats = 1e-4;
            auto insertAt = std::lower_bound(
                lane.points.begin(), lane.points.end(), point.beats,
                [](const AutomationPoint& existing, double beat) {
                    return existing.beats < beat;
                });
            auto merge = lane.points.end();
            if (insertAt != lane.points.end() &&
                std::abs(insertAt->beats - point.beats) <= kMergeBeats) {
                merge = insertAt;
            } else if (insertAt != lane.points.begin()) {
                auto previous = insertAt - 1;
                if (std::abs(previous->beats - point.beats) <= kMergeBeats)
                    merge = previous;
            }
            if (merge != lane.points.end()) merge->value = normalized;
            else lane.points.insert(insertAt, point);
            touched = true;
        }
    }
    if (touched) syncTrackAutomation(*track);
}

void EngineController::syncTrackAutomation(const TrackModel& track) {
    invalidateAutomationReadoutCache();
    auto found = m_channels.find(track.id);
    if (found == m_channels.end()) return;
    TrackChannel& channel = found->second;
    const double beatsPerSecond = m_project.tempo / 60.0;

    // One curve set per loaded plugin on this channel, keyed by slot id. A lane
    // with an empty slot id means the instrument, which is what a project
    // written before insert automation existed will say.
    auto curvesFor = [&](const std::string& slotId) -> plugins::PluginNode* {
        if (slotId.empty()) {
            return channel.instrument.empty() ? nullptr
                                              : channel.instrument.front().node.get();
        }
        for (InsertSlot& slot : channel.inserts) {
            if (slot.slotId == slotId) return slot.node.get();
        }
        for (InsertSlot& slot : channel.samplerInserts) {
            if (slot.slotId == slotId) return slot.node.get();
        }
        for (auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            for (InsertSlot& slot : clipFx.inserts) {
                if (slot.slotId == slotId) return slot.node.get();
            }
        }
        return nullptr;
    };

    std::unordered_map<plugins::PluginNode*,
                       std::shared_ptr<plugins::PluginNode::AutomationCurves>>
        built;
    auto ensure = [&](plugins::PluginNode* node) {
        auto& entry = built[node];
        if (!entry) {
            entry = std::make_shared<plugins::PluginNode::AutomationCurves>();
        }
        return entry;
    };
    // Every loaded plugin gets an entry even when nothing automates it, so a
    // lane the user deleted stops driving the parameter instead of leaving the
    // last curve in place forever.
    for (InsertSlot& slot : channel.instrument) {
        if (slot.node) ensure(slot.node.get());
    }
    for (InsertSlot& slot : channel.samplerInserts) {
        if (slot.node) ensure(slot.node.get());
    }
    for (auto& [clipId, clipFx] : channel.clipFx) {
        (void)clipId;
        for (InsertSlot& slot : clipFx.inserts) {
            if (slot.node) ensure(slot.node.get());
        }
    }
    for (InsertSlot& slot : channel.inserts) {
        if (slot.node) ensure(slot.node.get());
    }

    for (const ClipModel& clip : track.clips) {
        if (clip.kind != ClipKind::Midi || clip.muted) continue;
        const double clipStartBeats = clip.startSeconds * beatsPerSecond;

        for (const ControllerLane& lane : clip.lanes) {
            // cc >= 0 is a MIDI controller lane; those are not routed yet.
            if (lane.cc >= 0 || lane.parameterId.empty()) continue;
            plugins::PluginNode* node = curvesFor(lane.slotId);
            if (!node || !node->instance()) continue;
            const std::int32_t index =
                node->instance()->parameterIndexForId(lane.parameterId);
            if (index < 0) continue;   // the plugin no longer has that parameter

            // A lane's values are 0…1 — that is what the piano roll draws and
            // what `normalizeLane` clamps them to — while a plugin parameter is
            // in its own plain units. The mapping happens here, once, against
            // the range the live plugin reports: doing it in the node would put
            // the plugin's range on the audio thread, and storing plain values
            // in the lane would make the drawn curve meaningless the moment the
            // lane was pointed at a different parameter.
            const std::span<const plugins::ParameterInfo> parameters =
                node->instance()->parameters();
            const plugins::ParameterInfo& info = parameters[std::size_t(index)];
            const double span = info.maxValue - info.minValue;
            auto toPlain = [&](double normalized) {
                return info.minValue + span * std::clamp(normalized, 0.0, 1.0);
            };

            plugins::PluginNode::AutomationCurve curve;
            curve.parameterIndex = std::uint32_t(index);
            curve.defaultValue = toPlain(lane.defaultValue);
            curve.points.reserve(lane.points.size());
            for (const AutomationPoint& point : lane.points) {
                curve.points.emplace_back(clipStartBeats + point.beats,
                                          toPlain(point.value));
            }
            std::sort(curve.points.begin(), curve.points.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            ensure(node)->push_back(std::move(curve));
        }
    }

    // ── Automation clips ──
    //
    // A curve on an automation lane names the channel it drives, so the clips
    // that belong to *this* channel can be anywhere in the project — under this
    // track, under another one, or on a free-standing automation track. Cheap
    // to gather: automation lanes are few, and this only runs when the graph or
    // a curve changes.
    for (const TrackModel& lane : m_project.tracks) {
        if (!isAutomationLane(lane)) continue;
        for (const ClipModel& clip : lane.clips) {
            if (clip.kind != ClipKind::Automation || clip.muted ||
                !clip.automation.active) continue;
            const AutomationTarget& target = clip.automation.target;
            if (target.kind != AutomationTargetKind::PluginParameter) continue;
            if (target.channelId != track.id) continue;

            plugins::PluginNode* node = curvesFor(target.slotId);
            if (!node || !node->instance()) continue;
            const std::int32_t index =
                node->instance()->parameterIndexForId(target.parameterId);
            if (index < 0) continue;

            const std::span<const plugins::ParameterInfo> parameters =
                node->instance()->parameters();
            const plugins::ParameterInfo& info = parameters[std::size_t(index)];
            const double span = info.maxValue - info.minValue;
            auto toPlain = [&](double normalized) {
                return info.minValue + span * std::clamp(normalized, 0.0, 1.0);
            };

            plugins::PluginNode::AutomationCurve curve;
            curve.parameterIndex = std::uint32_t(index);
            curve.defaultValue = toPlain(clip.automation.defaultValue);
            appendCurvePoints(curve.points, clip, beatsPerSecond, toPlain);
            std::sort(curve.points.begin(), curve.points.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            ensure(node)->push_back(std::move(curve));
        }
    }

    for (auto& [node, curves] : built) {
        // A curve writes its parameter on *every* block, so a lane pointing at
        // the wrong parameter — or holding a default the user never chose —
        // looks exactly like a knob that will not stay where it is put.
        if (diagnosePluginParameters() && !curves->empty()) {
            for (const plugins::PluginNode::AutomationCurve& curve : *curves) {
                logParameterWrite("automation", node->instance(),
                                  std::int32_t(curve.parameterIndex),
                                  curve.points.empty() ? curve.defaultValue
                                                       : curve.points.front().second);
            }
        }
        node->setAutomation(curves);
    }
}

void EngineController::syncTrackLevelAutomation(const TrackModel& track) {
    invalidateAutomationReadoutCache();
    auto found = m_channels.find(track.id);
    if (found == m_channels.end() || !found->second.fader) return;
    TrackChannel& channel = found->second;
    const double beatsPerSecond = m_project.tempo / 60.0;

    auto levels = std::make_shared<engine::LevelAutomation>();
    std::vector<std::shared_ptr<engine::LevelCurve>> sendCurves(track.sends.size());

    for (const TrackModel& lane : m_project.tracks) {
        if (!isAutomationLane(lane)) continue;
        for (const ClipModel& clip : lane.clips) {
            if (clip.kind != ClipKind::Automation || clip.muted ||
                !clip.automation.active) continue;
            const AutomationTarget& target = clip.automation.target;
            if (target.channelId != track.id) continue;

            engine::LevelCurve* curve = nullptr;
            switch (target.kind) {
                case AutomationTargetKind::TrackVolume: curve = &levels->gain; break;
                case AutomationTargetKind::TrackPan: curve = &levels->pan; break;
                case AutomationTargetKind::TrackMute: curve = &levels->mute; break;
                case AutomationTargetKind::SendLevel: {
                    for (std::size_t i = 0; i < track.sends.size(); ++i) {
                        if (track.sends[i].id != target.sendId) continue;
                        if (!sendCurves[i])
                            sendCurves[i] = std::make_shared<engine::LevelCurve>();
                        curve = sendCurves[i].get();
                    }
                    break;
                }
                case AutomationTargetKind::PluginParameter: break;
            }
            if (!curve) continue;

            // Plain units, densified — the node interpolates straight lines and
            // nothing else, exactly like the plugin path.
            auto toPlain = [&](double normalized) {
                return automationToPlain(target, normalized);
            };
            if (!curve->active) {
                curve->active = true;
                curve->defaultValue = toPlain(clip.automation.defaultValue);
            }
            appendCurvePoints(curve->points, clip, beatsPerSecond, toPlain);
        }
    }

    const auto tidy = [](engine::LevelCurve& curve) {
        if (!curve.active) return;
        std::sort(curve.points.begin(), curve.points.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    };
    tidy(levels->gain);
    tidy(levels->pan);
    tidy(levels->mute);
    channel.fader->setAutomation(levels);

    // Every send gets a snapshot, including the empty one — a curve the user
    // deleted has to stop driving the send rather than leaving its last shape
    // in place for good.
    for (std::size_t i = 0; i < channel.sends.size(); ++i) {
        if (!channel.sends[i]) continue;
        if (i < sendCurves.size() && sendCurves[i]) {
            tidy(*sendCurves[i]);
            channel.sends[i]->setAutomation(sendCurves[i]);
        } else {
            channel.sends[i]->setAutomation(
                std::make_shared<engine::LevelCurve>());
        }
    }
}

void EngineController::syncAllLevelAutomation() {
    for (const TrackModel& track : m_project.tracks) {
        if (carriesAudio(track)) syncTrackLevelAutomation(track);
    }
}

void EngineController::followPassiveAutomation(const AutomationTarget& target,
                                                double normalized) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    for (TrackModel& lane : m_project.tracks) {
        if (!isAutomationLane(lane)) continue;
        for (ClipModel& clip : lane.clips) {
            if (clip.kind != ClipKind::Automation || clip.automation.active ||
                clip.automation.target != target) continue;
            clip.automation.defaultValue = normalized;
            for (AutomationPoint& point : clip.automation.points)
                point.value = normalized;
        }
    }
    invalidateAutomationReadoutCache();
}

void EngineController::syncAutomationTarget(const AutomationTarget& target) {
    TrackModel* track = m_project.findTrack(target.channelId);
    if (!track) {
        invalidateAutomationReadoutCache();
        return;
    }
    if (target.kind == AutomationTargetKind::PluginParameter) {
        syncTrackAutomation(*track);
    } else {
        syncTrackLevelAutomation(*track);
    }
}

void EngineController::syncAllAutomation() {
    for (const TrackModel& track : m_project.tracks) {
        syncTrackAutomation(track);
    }
    syncAllLevelAutomation();
}

void EngineController::syncAllNotes() {
    for (const TrackModel& track : m_project.tracks) {
        if (trackAccepts(track.kind, ClipKind::Midi)) syncTrackNotes(track);
    }
}

void EngineController::syncTrackClips(const TrackModel& track) {
    auto found = m_channels.find(track.id);
    if (found == m_channels.end() || !found->second.clips) return;
    TrackChannel& channel = found->second;
    engine::ClipPlayerNode* player = channel.clips.get();

    auto list = std::make_shared<engine::ClipPlayerNode::ClipList>();
    list->reserve(track.clips.size());

    // A track being recorded onto plays nothing. Recording a second layer over
    // an existing clip means playing *against* it, not along with it: the old
    // attempt in the performer's ears is the one thing guaranteed to ruin the
    // new one. What is heard during the take is the input, if it is monitored,
    // and otherwise silence. The clips come straight back when the graph is
    // rebuilt on stop.
    const bool capturing =
        std::find(m_recordingTracks.begin(), m_recordingTracks.end(), track.id) !=
        m_recordingTracks.end();
    if (capturing) {
        player->setClips(std::move(list));
        for (auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            if (clipFx.player) {
                clipFx.player->setClips(
                    std::make_shared<const engine::ClipPlayerNode::ClipList>());
            }
        }
        return;
    }

    // One span per clip that rendered anything. A clip may produce several
    // placements (a comp) or none (MIDI, or a file that failed to decode), so
    // the crossfade pass below works on spans rather than indexing track.clips.
    std::vector<PlacementSpan> spans;
    spans.reserve(track.clips.size());
    for (const auto& clip : track.clips) {
        // MIDI clips carry notes, not samples, and nothing renders them yet.
        if (clip.kind != ClipKind::Audio) continue;
        if (!clip.inserts.empty()) {
            auto privateChannel = channel.clipFx.find(clip.id);
            if (privateChannel == channel.clipFx.end() ||
                !privateChannel->second.player) {
                continue;
            }
            auto privateList =
                std::make_shared<engine::ClipPlayerNode::ClipList>();
            PlacementSpan privateSpan = emitClipPlacements(clip, *privateList);
            // Clip gain and pan sit after its private plugins, so the dedicated
            // fader owns them rather than the source placement.
            for (engine::ClipPlacement& placement : *privateList) {
                placement.gain = 1.0f;
                placement.pan = 0.0f;
            }
            std::stable_sort(privateList->begin(), privateList->end(),
                             [](const engine::ClipPlacement& a,
                                const engine::ClipPlacement& b) {
                                 return a.startSample < b.startSample;
                             });
            privateChannel->second.player->setClips(std::move(privateList));
            (void)privateSpan;
            continue;
        }
        PlacementSpan span = emitClipPlacements(clip, *list);
        if (span.count > 0) spans.push_back(span);
    }

    // Auto-crossfades: where two clips overlap, the earlier one fades out and
    // the later one fades in over the overlap, equal-power so the loudness in
    // the seam stays constant. Derived here from clip geometry — the document
    // only stores the user's own fades, so this recomputes on every edit. A
    // crossfade never shortens a longer user fade (it takes the max).
    //
    // A comp's internal seams are already faded, so this only touches a span's
    // outermost placements: the fade belongs between clips, not inside one.
    std::sort(spans.begin(), spans.end(),
              [](const PlacementSpan& a, const PlacementSpan& b) {
                  return a.startSample < b.startSample;
              });
    for (size_t k = 0; k + 1 < spans.size(); ++k) {
        const engine::SamplePos overlap =
            spans[k].endSample - spans[k + 1].startSample;
        if (overlap <= 0) continue;
        engine::ClipPlacement& earlier = (*list)[spans[k].first + spans[k].count - 1];
        engine::ClipPlacement& later = (*list)[spans[k + 1].first];
        earlier.fadeOutSamples = std::max(earlier.fadeOutSamples, overlap);
        earlier.fadeEqualPower = true;
        later.fadeInSamples = std::max(later.fadeInSamples, overlap);
        later.fadeEqualPower = true;
    }

    std::stable_sort(list->begin(), list->end(),
                     [](const engine::ClipPlacement& a,
                        const engine::ClipPlacement& b) {
                         return a.startSample < b.startSample;
                     });

    player->setClips(std::move(list));
}

void EngineController::deferClipSync(const std::string& trackId) {
    if (std::find(m_deferredClipSync.begin(), m_deferredClipSync.end(), trackId) ==
        m_deferredClipSync.end()) {
        m_deferredClipSync.push_back(trackId);
    }
}

void EngineController::flushDeferredClipSync() {
    if (m_deferredClipSync.empty()) return;
    std::vector<std::string> pending;
    pending.swap(m_deferredClipSync);
    for (const std::string& trackId : pending) {
        if (const TrackModel* track = m_project.findTrack(trackId)) {
            syncTrackClips(*track);
        }
    }
}

void EngineController::flushSamplerPrecompute() {
    auto flushNode = [](const std::shared_ptr<plugins::PluginNode>& node) {
        if (!node) return;
        auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(
            node->instance());
        if (sampler) sampler->flushPendingPrecompute();
    };
    auto flushSlot = [&](InsertSlot& slot) {
        flushNode(slot.node);
        flushNode(slot.rightNode);
    };

    for (auto& [channelId, channel] : m_channels) {
        (void)channelId;
        for (InsertSlot& slot : channel.instrument) flushSlot(slot);
        for (InsertSlot& slot : channel.samplerInserts) flushSlot(slot);
        for (auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            for (InsertSlot& slot : clipFx.inserts) flushSlot(slot);
        }
        for (InsertSlot& slot : channel.inserts) flushSlot(slot);
    }
}

EngineController::SoloState EngineController::soloState() const {
    SoloState state;
    for (const TrackModel& t : m_project.tracks) {
        if (!t.soloed) continue;
        state.any = true;
        state.open.insert(t.id);
        // Soloing a folder means soloing what is in it. A plain folder carries
        // no signal at all, so without this the solo would open a channel that
        // does not exist and mute the entire project; a summing folder would
        // survive on its own, but "solo the group" has to mean the same thing
        // whichever kind of folder the group is.
        if (isFolder(t)) {
            for (const std::string& child : subtreeOf(m_project, t.id))
                state.open.insert(child);
        }
    }
    if (!state.any) return state;

    // Walk downstream from every soloed track: its output bus, the buses its
    // sends feed, and so on. Those channels carry the soloed signal, so muting
    // them would silence the very thing being soloed. Bounded by the track
    // count — a routing loop cannot be created, and a visited id is skipped.
    std::vector<std::string> pending(state.open.begin(), state.open.end());
    while (!pending.empty()) {
        const std::string id = pending.back();
        pending.pop_back();
        const TrackModel* track = m_project.findTrack(id);
        if (!track) continue;
        auto follow = [&](const std::string& destination) {
            if (destination.empty()) return;
            if (!state.open.insert(destination).second) return;
            pending.push_back(destination);
        };
        follow(track->outputBusId);
        for (const SendModel& send : track->sends) follow(send.destinationTrackId);
    }
    return state;
}

void EngineController::syncTrackGain(const TrackModel& track) {
    syncTrackGain(track, soloState());
}

void EngineController::syncTrackGain(const TrackModel& track,
                                     const SoloState& solo) {
    auto found = m_channels.find(track.id);
    if (found == m_channels.end() || !found->second.fader) return;
    engine::GainNode* fader = found->second.fader.get();

    // Mute and solo are a gate over the fader. They cannot be represented by
    // setting its static gain to zero: an active automation curve replaces the
    // static fader and must still be able to rise from a stored value of zero.
    //
    // Solo silences every channel that is neither soloed nor carrying a soloed
    // signal. It used to silence only `TrackKind::Audio`, which meant soloing
    // anything left every instrument and MIDI track playing — solo looked
    // broken because, for most of a project, it was.
    const bool silent =
        track.muted || (solo.any && !solo.open.contains(track.id));
    fader->setGain(track.volume);
    fader->setSilent(silent);
    fader->setPan(track.pan);
    fader->setMono(track.mono);
}

void EngineController::syncAllTrackGains() {
    const SoloState solo = soloState();
    for (const auto& t : m_project.tracks) syncTrackGain(t, solo);
}

bool EngineController::anySoloed() const {
    return std::any_of(m_project.tracks.begin(), m_project.tracks.end(),
                       [](const TrackModel& t) { return t.soloed; });
}

bool EngineController::anyMuted() const {
    return std::any_of(m_project.tracks.begin(), m_project.tracks.end(),
                       [](const TrackModel& t) { return t.muted; });
}

void EngineController::clearAllSolos() {
    bool changed = false;
    for (TrackModel& t : m_project.tracks) {
        if (!t.soloed) continue;
        t.soloed = false;
        changed = true;
    }
    if (changed) syncAllTrackGains();
}

collab::SharedMutationResult EngineController::clearAllMutes() {
    if (m_sharedMutationSink) {
        std::vector<std::string> mutedTrackIds;
        mutedTrackIds.reserve(m_project.tracks.size());
        for (const TrackModel& track : m_project.tracks) {
            if (track.muted) mutedTrackIds.push_back(track.id);
        }
        if (mutedTrackIds.empty())
            return collab::SharedMutationResult::LocalFallback;
        const auto result =
            m_sharedMutationSink->clearAllMutes(mutedTrackIds);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }

    bool changed = false;
    for (TrackModel& t : m_project.tracks) {
        if (!t.muted) continue;
        t.muted = false;
        changed = true;
    }
    if (changed) syncAllTrackGains();
    return collab::SharedMutationResult::LocalFallback;
}

// ── Plugin inserts ─────────────────────────────────────────────────────────

void EngineController::syncChannelInserts(const std::string& channelId,
                                          TrackChannel& channel,
                                          const std::vector<InsertModel>& slots) {
    syncSlots(channelId, channel.inserts, slots);
}

void EngineController::announceRetiring(const std::string& channelId,
                                        const std::vector<InsertSlot>& going) {
    if (!m_pluginRetiring) return;
    for (const InsertSlot& slot : going) {
        if (slot.node) m_pluginRetiring(channelId, slot.slotId);
    }
}

void EngineController::announceAllRetiring() {
    if (!m_pluginRetiring) return;
    for (const auto& [channelId, channel] : m_channels) {
        announceRetiring(channelId, channel.instrument);
        announceRetiring(channelId, channel.samplerInserts);
        for (const auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            announceRetiring(channelId, clipFx.inserts);
        }
        announceRetiring(channelId, channel.inserts);
    }
}

void EngineController::syncSlots(const std::string& channelId,
                                 std::vector<InsertSlot>& live,
                                 const std::vector<InsertModel>& slots) {
    // A plugin slot follows its channel format automatically. Master and any
    // non-track owner stay stereo; every slot owned by a mono track (including
    // its instrument, clip FX and sampler FX) asks the format for mono.
    const TrackModel* owner = m_project.findTrack(channelId);

    std::vector<InsertSlot> rebuilt;
    rebuilt.reserve(slots.size());

    auto descriptorFor = [&](const InsertModel& slot) {
        plugins::PluginDescriptor descriptor;
        descriptor.format = toHostFormat(slot.format);
        descriptor.uid = slot.uid;
        descriptor.path = slot.path;
        descriptor.name = slot.name;
        if (const auto known =
                m_pluginManager.find(descriptor.format, descriptor.uid)) {
            descriptor = *known;
        }
        // Loading directly from a descriptor is valid before this controller's
        // scan cache has been populated. The slot itself still says what the
        // plugin is: anything placed as the track instrument must receive MIDI
        // and behave as a graph source even in that first rebuild.
        if (owner && owner->instrument.id == slot.id) {
            descriptor.isInstrument = true;
            descriptor.wantsMidi = true;
        }
        return descriptor;
    };
    auto restoreParameters = [this](plugins::PluginNode& node,
                                    const std::vector<InsertParameter>& values) {
        applyStoredParameters(node, values);
    };

    for (const InsertModel& slot : slots) {
        const std::uint16_t preferredChannels =
            slot.channelMode == PluginChannelMode::Mono ||
                    slot.channelMode == PluginChannelMode::DualMono
                ? 1
                : slot.channelMode == PluginChannelMode::Stereo
                      ? 2
                      : (owner && owner->mono ? 1 : 2);
        // Reuse the loaded plugin whenever the slot still refers to the same
        // one. Instantiating is slow and throws away the plugin's state, so it
        // must happen when the user swapped the plugin — not on every routing
        // edit that happens to rebuild the graph.
        auto existing = std::find_if(
            live.begin(), live.end(), [&](const InsertSlot& candidate) {
                return candidate.slotId == slot.id && candidate.uid == slot.uid;
            });
        InsertSlot loaded;
        if (existing != live.end() && existing->node) {
            loaded = std::move(*existing);
            live.erase(existing);
        } else {
            std::vector<std::uint8_t> reloadState;
            if (existing != live.end()) {
                reloadState = std::move(existing->reloadState);
                live.erase(existing);
            }
            if (!slot.isLoaded()) continue;

            auto instance = m_pluginManager.instantiate(descriptorFor(slot));
            if (!instance) continue;
            if (!reloadState.empty()) (void)instance->loadState(reloadState);

            loaded.slotId = slot.id;
            loaded.uid = slot.uid;
            loaded.node = std::make_shared<plugins::PluginNode>(slot.name,
                                                                std::move(instance));
            if (reloadState.empty()) restoreParameters(*loaded.node, slot.parameters);
        }

        loaded.channelMode = slot.channelMode;
        loaded.node->setPreferredChannelCount(preferredChannels);
        loaded.node->setBypassed(slot.bypassed);
        loaded.node->setMix(slot.mix);

        if (slot.channelMode == PluginChannelMode::DualMono) {
            if (!loaded.rightNode) {
                auto right = m_pluginManager.instantiate(descriptorFor(slot));
                if (right) {
                    if (!loaded.rightReloadState.empty()) {
                        (void)right->loadState(loaded.rightReloadState);
                    }
                    loaded.rightNode = std::make_shared<plugins::PluginNode>(
                        slot.name + " Right", std::move(right));
                    if (loaded.rightReloadState.empty()) {
                        restoreParameters(
                            *loaded.rightNode,
                            slot.rightParameters.empty() ? slot.parameters
                                                         : slot.rightParameters);
                    }
                    loaded.rightReloadState.clear();
                }
            }
            if (!loaded.leftSelector) {
                loaded.leftSelector = std::make_shared<engine::ChannelSelectNode>(
                    0, slot.name + " Left Input");
                loaded.rightSelector = std::make_shared<engine::ChannelSelectNode>(
                    1, slot.name + " Right Input");
                loaded.stereoMerge = std::make_shared<engine::StereoMergeNode>(
                    slot.name + " Dual Mono Merge");
            }
        }
        if (loaded.rightNode) {
            loaded.rightNode->setPreferredChannelCount(1);
            loaded.rightNode->setBypassed(slot.bypassed);
            loaded.rightNode->setMix(slot.mix);
        }
        rebuilt.push_back(std::move(loaded));
    }

    // Anything left in the old list is a plugin the user removed or replaced;
    // dropping the last reference here destroys it. Safe because the published
    // graph co-owns its nodes and keeps rendering the old snapshot until the
    // new one is committed.
    //
    // The one thing that is *not* safe is an editor window still holding the
    // plugin's view, so whoever owns windows is told first — while the plugin
    // is still there to be told to let go.
    announceRetiring(channelId, live);
    live = std::move(rebuilt);
}

engine::NodeId EngineController::connectInsertChain(engine::AudioGraph& graph,
                                                    TrackChannel& channel,
                                                    engine::NodeId head) {
    return connectSlots(graph, channel.inserts, channel.ids.inserts, head);
}

engine::NodeId EngineController::connectSlots(engine::AudioGraph& graph,
                                              std::vector<InsertSlot>& live,
                                              std::vector<engine::NodeId>& ids,
                                              engine::NodeId head) {
    ids.clear();
    engine::NodeId previous = head;
    for (InsertSlot& slot : live) {
        slot.nodeId = engine::kInvalidNode;
        slot.rightNodeId = engine::kInvalidNode;
        slot.leftSelectorId = engine::kInvalidNode;
        slot.rightSelectorId = engine::kInvalidNode;
        if (!slot.node) continue;
        if (slot.channelMode == PluginChannelMode::DualMono && slot.rightNode &&
            slot.leftSelector && slot.rightSelector && slot.stereoMerge) {
            slot.leftSelectorId = graph.adoptNode(slot.leftSelector);
            slot.rightSelectorId = graph.adoptNode(slot.rightSelector);
            slot.nodeId = graph.adoptNode(slot.node);
            slot.rightNodeId = graph.adoptNode(slot.rightNode);
            const engine::NodeId mergeId = graph.adoptNode(slot.stereoMerge);
            ids.push_back(slot.leftSelectorId);
            graph.connect(previous, slot.leftSelectorId);
            graph.connect(previous, slot.rightSelectorId);
            graph.connect(slot.leftSelectorId, slot.nodeId);
            graph.connect(slot.rightSelectorId, slot.rightNodeId);
            graph.connect(slot.nodeId, mergeId);
            graph.connect(slot.rightNodeId, mergeId);
            previous = mergeId;
            continue;
        }
        const engine::NodeId id = graph.adoptNode(slot.node);
        slot.nodeId = id;
        ids.push_back(id);
        graph.connect(previous, id);
        previous = id;
    }
    return previous;
}

EngineController::TrackChannel* EngineController::findChannel(
    const std::string& channelId) {
    auto found = m_channels.find(channelId);
    return found == m_channels.end() ? nullptr : &found->second;
}

std::vector<InsertModel>* EngineController::mutableChannelInserts(
    const std::string& channelId) {
    if (channelId == kMasterChannelId) return &m_project.masterInserts;
    TrackModel* track = m_project.findTrack(channelId);
    return track ? &track->inserts : nullptr;
}

std::vector<InsertModel>* EngineController::mutableSamplerFxInserts(
    const std::string& trackId, const std::string& samplerSlotId) {
    TrackModel* track = m_project.findTrack(trackId);
    if (!track || track->instrument.uid != "daw.sampler" ||
        track->instrument.id != samplerSlotId ||
        !track->samplerFx.isOwnedBy(track->instrument)) {
        return nullptr;
    }
    return &track->samplerFx.inserts;
}

std::vector<InsertModel>* EngineController::mutableClipFxInserts(
    const std::string& trackId, const std::string& clipId) {
    ClipModel* clip = findClip(trackId, clipId);
    if (!clip || clip->kind != ClipKind::Audio) return nullptr;
    return &clip->inserts;
}

const std::vector<InsertModel>* EngineController::channelInserts(
    const std::string& channelId) const {
    if (channelId == kMasterChannelId) return &m_project.masterInserts;
    const TrackModel* track = m_project.findTrack(channelId);
    return track ? &track->inserts : nullptr;
}

InsertModel* EngineController::mutableInsertSlot(const std::string& channelId,
                                                 const std::string& insertId) {
    // The instrument first, matching `insertNode`: a slot id is a slot id
    // whether the plugin makes sound or shapes it.
    if (TrackModel* track = m_project.findTrack(channelId)) {
        if (track->instrument.id == insertId) return &track->instrument;
        for (InsertModel& slot : track->samplerFx.inserts) {
            if (slot.id == insertId) return &slot;
        }
        for (ClipModel& clip : track->clips) {
            for (InsertModel& slot : clip.inserts) {
                if (slot.id == insertId) return &slot;
            }
        }
    }
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return nullptr;
    for (InsertModel& slot : *slots) {
        if (slot.id == insertId) return &slot;
    }
    return nullptr;
}

const InsertModel* EngineController::insertModel(
    const std::string& channelId, const std::string& insertId) const {
    return const_cast<EngineController*>(this)->mutableInsertSlot(channelId,
                                                                  insertId);
}

bool EngineController::sidechainWouldFeedback(
    const std::string& destinationChannelId,
    const std::string& sourceTrackId) const {
    if (destinationChannelId == sourceTrackId) return true;

    // A sidechain edge is source -> destination. It closes a loop exactly when
    // the destination already reaches the source through an output or send.
    std::vector<std::string> pending{destinationChannelId};
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        const std::string current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        const TrackModel* track = m_project.findTrack(current);
        if (!track) continue; // master has no outgoing project route
        auto follow = [&](const std::string& next) {
            if (next.empty()) return false;
            if (next == sourceTrackId) return true;
            pending.push_back(next);
            return false;
        };
        if (follow(track->outputBusId)) return true;
        for (const SendModel& send : track->sends) {
            // Disabled sends remain graph edges at zero gain, so they still
            // count for cycle safety.
            if (follow(send.destinationTrackId)) return true;
        }
    }
    return false;
}

EngineController::InsertSlot* EngineController::liveInsertSlot(
    const std::string& channelId, const std::string& insertId) {
    // The instrument is addressed by slot id like any insert — it is simply the
    // one that sits ahead of them and gets fed notes.
    if (TrackChannel* channel = findChannel(channelId)) {
        for (InsertSlot& slot : channel->instrument) {
            if (slot.slotId == insertId) return &slot;
        }
        for (InsertSlot& slot : channel->samplerInserts) {
            if (slot.slotId == insertId) return &slot;
        }
        for (auto& [clipId, clipFx] : channel->clipFx) {
            (void)clipId;
            for (InsertSlot& slot : clipFx.inserts) {
                if (slot.slotId == insertId) return &slot;
            }
        }
    }
    TrackChannel* channel = findChannel(channelId);
    if (!channel) return nullptr;
    for (InsertSlot& slot : channel->inserts) {
        if (slot.slotId == insertId) return &slot;
    }
    return nullptr;
}

plugins::PluginNode* EngineController::insertNode(const std::string& channelId,
                                                  const std::string& insertId) {
    InsertSlot* slot = liveInsertSlot(channelId, insertId);
    return slot ? slot->node.get() : nullptr;
}

plugins::PluginNode* EngineController::editorInsertNode(
    const std::string& channelId, const std::string& insertId) {
    InsertSlot* live = liveInsertSlot(channelId, insertId);
    const InsertModel* model = insertModel(channelId, insertId);
    if (!live) return nullptr;
    if (model && model->channelMode == PluginChannelMode::DualMono &&
        model->editorChannel == PluginEditorChannel::Right && live->rightNode) {
        return live->rightNode.get();
    }
    return live->node.get();
}

// ── Graph construction ─────────────────────────────────────────────────────

audio::Result EngineController::rebuildGraph(bool reconfigurePlugins) {
    ++m_graphRebuildCount;
    engine::AudioGraph& graph = m_engine.graph();

    // A fresh topology every time, but the *same node objects* wherever the
    // track still exists: routing changes must not reset filters, meters or
    // loaded clips. The previously published graph co-owns its nodes, so the
    // audio thread keeps rendering safely until the new one is committed.
    graph = engine::AudioGraph{};

    // Drop channels whose track is gone — or whose track no longer has a
    // channel at all, which is what a summing folder becomes when summing is
    // switched off. Either way nothing below rebuilds this entry, so its node
    // ids would be left pointing into the graph that is being thrown away.
    // The master's channel has no track and must survive, so it is exempt.
    std::erase_if(m_channels, [this](const auto& entry) {
        const TrackModel* track = m_project.findTrack(entry.first);
        const bool removed = entry.first != kMasterChannelId &&
                             (track == nullptr || !carriesAudio(*track));
        if (removed) {
            announceRetiring(entry.first, entry.second.instrument);
            announceRetiring(entry.first, entry.second.samplerInserts);
            for (const auto& [clipId, clipFx] : entry.second.clipFx) {
                (void)clipId;
                announceRetiring(entry.first, clipFx.inserts);
            }
            announceRetiring(entry.first, entry.second.inserts);
        }
        return removed;
    });

    if (!m_masterSum) m_masterSum = std::make_shared<engine::SumNode>("Master Sum");
    if (!m_masterFader) m_masterFader = std::make_shared<engine::GainNode>("Master");
    m_masterSumId = graph.adoptNode(m_masterSum);
    m_masterFaderId = graph.adoptNode(m_masterFader);
    graph.setSink(m_masterFaderId);
    m_masterFader->setGain(m_project.masterVolume);
    m_masterFader->setPan(m_project.masterPan);

    // The master's inserts sit between its sum and its fader. Giving the master
    // a real TrackChannel — rather than special-casing it — is what lets every
    // insert command below work on it with no second code path.
    TrackChannel& masterChannel = m_channels[std::string(kMasterChannelId)];
    masterChannel.fader = m_masterFader;
    masterChannel.ids = TrackNodes{};
    masterChannel.ids.clips = m_masterSumId;
    masterChannel.ids.fader = m_masterFaderId;
    masterChannel.ids.meter = m_masterFaderId;
    syncChannelInserts(std::string(kMasterChannelId), masterChannel,
                       m_project.masterInserts);
    const engine::NodeId masterChainEnd =
        connectInsertChain(graph, masterChannel, m_masterSumId);
    masterChannel.ids.preFaderTap = masterChainEnd;
    graph.connect(masterChainEnd, m_masterFaderId);

    // The metronome is a permanent source summed into the master; it only makes
    // sound while the transport rolls and the click is enabled.
    if (!m_metronome) m_metronome = std::make_shared<engine::MetronomeNode>();
    m_metronome->setEnabled(m_metronomeEnabled);
    // Left out of the graph entirely during a render rather than merely
    // disabled: `MetronomeNode` gates its click on `context.playing`, which an
    // offline pass asserts, and it renders its count-in ahead of that gate. Not
    // connecting it is the only way to be sure none of it reaches the file.
    if (!m_renderingPass) {
        const engine::NodeId metronomeId = graph.adoptNode(m_metronome);
        graph.connect(metronomeId, m_masterSumId);
    }

    // Auditioning a file from the browser, on the same terms: a permanent
    // source into the master, silent unless a preview was asked for. Adopting
    // the same object every rebuild is what lets a preview keep playing while
    // the user adds a track.
    if (!m_preview) m_preview = std::make_shared<engine::PreviewPlayerNode>();
    const engine::NodeId previewId = graph.adoptNode(m_preview);
    graph.connect(previewId, m_masterSumId);

    // Which channels are fed from elsewhere — the destination of a track's
    // output, or of a send. Known *before* the strips are built, because a
    // channel that receives audio needs a merge point ahead of its inserts and
    // one that does not should not pay for a node it never uses.
    std::unordered_set<std::string> receivers;
    for (const auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        if (!track.outputBusId.empty()) receivers.insert(track.outputBusId);
        for (const SendModel& send : track.sends) {
            receivers.insert(send.destinationTrackId);
        }
    }

    // ── One channel strip per track ──
    for (const auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;

        TrackChannel& channel = m_channels[track.id];
        if (!channel.clips) {
            channel.clips = std::make_shared<engine::ClipPlayerNode>(track.name + " Clips");
        }
        if (!channel.fader) {
            channel.fader = std::make_shared<engine::GainNode>(track.name + " Fader");
        }
        if (!channel.meter) {
            channel.meter = std::make_shared<engine::MeterNode>(track.name + " Meter");
        }

        channel.ids = TrackNodes{};
        channel.ids.clips = graph.adoptNode(channel.clips);
        channel.ids.fader = graph.adoptNode(channel.fader);
        channel.ids.meter = graph.adoptNode(channel.meter);
        graph.connect(channel.ids.fader, channel.ids.meter);

        // A clip with private inserts leaves the shared track clip player,
        // runs through its own chain, post-FX level and meter, then rejoins the
        // track here.  This is the clip equivalent of Sampler FX: other clips,
        // monitored input and routed audio never touch the private plugins.
        std::unordered_set<std::string> wantedClipFx;
        for (const ClipModel& clip : track.clips) {
            if (clip.kind == ClipKind::Audio && !clip.inserts.empty()) {
                wantedClipFx.insert(clip.id);
            }
        }
        std::erase_if(channel.clipFx, [&](auto& entry) {
            if (wantedClipFx.contains(entry.first)) return false;
            announceRetiring(track.id, entry.second.inserts);
            return true;
        });

        engine::NodeId sourceHead = channel.ids.clips;
        if (!wantedClipFx.empty()) {
            if (!channel.clipFxSum) {
                channel.clipFxSum =
                    std::make_shared<engine::SumNode>(track.name + " Clip FX Sum");
            }
            const engine::NodeId sumId = graph.adoptNode(channel.clipFxSum);
            graph.connect(channel.ids.clips, sumId);
            for (const ClipModel& clip : track.clips) {
                if (!wantedClipFx.contains(clip.id)) continue;
                ClipFxChannel& clipChannel = channel.clipFx[clip.id];
                if (!clipChannel.player) {
                    clipChannel.player = std::make_shared<engine::ClipPlayerNode>(
                        track.name + " / " + clip.name + " Player");
                }
                if (!clipChannel.fader) {
                    clipChannel.fader = std::make_shared<engine::GainNode>(
                        track.name + " / " + clip.name + " Level");
                }
                if (!clipChannel.meter) {
                    clipChannel.meter = std::make_shared<engine::MeterNode>(
                        track.name + " / " + clip.name + " Meter");
                }
                syncSlots(track.id, clipChannel.inserts, clip.inserts);
                clipChannel.playerId = graph.adoptNode(clipChannel.player);
                clipChannel.faderId = graph.adoptNode(clipChannel.fader);
                clipChannel.meterId = graph.adoptNode(clipChannel.meter);
                const engine::NodeId chainEnd = connectSlots(
                    graph, clipChannel.inserts, clipChannel.insertIds,
                    clipChannel.playerId);
                clipChannel.fader->setGain(clip.gain);
                clipChannel.fader->setPan(clip.pan);
                graph.connect(chainEnd, clipChannel.faderId);
                graph.connect(clipChannel.faderId, clipChannel.meterId);
                graph.connect(clipChannel.meterId, sumId);
            }
            sourceHead = sumId;
        } else {
            channel.clipFxSum.reset();
        }

        // A live input node only exists while the channel is listening or armed,
        // so an idle project carries no input plumbing at all.
        if (track.monitor || track.armed) {
            if (!channel.input || channel.inputChannel != track.inputChannel ||
                channel.inputChannelCount != track.inputChannelCount) {
                channel.input = std::make_shared<engine::InputNode>(
                    track.name + " Input", m_engine.inputBus(),
                    engine::ChannelCount(track.inputChannel),
                    engine::ChannelCount(
                        std::clamp(track.inputChannelCount, 1u, 2u)));
                channel.inputChannel = track.inputChannel;
                channel.inputChannelCount = track.inputChannelCount;
            }
            channel.input->setEnabled(track.monitor);
            channel.ids.input = graph.adoptNode(channel.input);
        } else {
            channel.input.reset();
        }

        // Sources → instrument → inserts → fader. A PluginNode sums its own
        // inputs, so the clips and the live input feed the first insert
        // directly and no extra SumNode is needed at the head of the chain.
        syncChannelInserts(track.id, channel, track.inserts);

        // The instrument, and the notes that drive it. Only tracks that carry
        // notes get either — an audio track has nothing to sound.
        engine::NodeId head = sourceHead;
        if (trackAccepts(track.kind, ClipKind::Midi)) {
            if (!channel.midiClips) {
                channel.midiClips =
                    std::make_shared<engine::MidiClipPlayerNode>(track.name + " Notes");
            }
            channel.ids.midiClips = graph.adoptNode(channel.midiClips);

            const std::vector<InsertModel> one =
                track.instrument.isLoaded() ? std::vector<InsertModel>{track.instrument}
                                            : std::vector<InsertModel>{};
            syncSlots(track.id, channel.instrument, one);
            if (!channel.instrument.empty() && channel.instrument.front().node) {
                channel.ids.instrument = graph.adoptNode(channel.instrument.front().node);
                // Notes in, audio out. The instrument also takes the clip
                // player's (silent) output so the graph has one head to hang
                // the rest of the chain off.
                graph.connect(channel.ids.midiClips, channel.ids.instrument);
                graph.connect(sourceHead, channel.ids.instrument);
                head = channel.ids.instrument;
            } else {
                channel.ids.instrument = engine::kInvalidNode;
            }
        } else {
            channel.midiClips.reset();
            channel.instrument.clear();
        }

        // The sampler owns a private post-instrument strip. It is deliberately
        // completed before the channel merge point: monitored input, track
        // routing and sends entering this channel must never pass through FX
        // that belong to one sample instrument.
        const bool samplerOwned = track.samplerFx.isOwnedBy(track.instrument);
        if (samplerOwned) {
            syncSlots(track.id, channel.samplerInserts, track.samplerFx.inserts);
            if (!channel.samplerFader) {
                channel.samplerFader =
                    std::make_shared<engine::GainNode>(track.name + " Sampler Level");
            }
            if (!channel.samplerMeter) {
                channel.samplerMeter =
                    std::make_shared<engine::MeterNode>(track.name + " Sampler Meter");
            }
            channel.samplerFader->setGain(track.samplerFx.volume);
            channel.samplerFader->setPan(track.samplerFx.pan);
            channel.ids.samplerFader = graph.adoptNode(channel.samplerFader);
            channel.ids.samplerMeter = graph.adoptNode(channel.samplerMeter);

            if (channel.ids.instrument != engine::kInvalidNode) {
                const engine::NodeId samplerChainEnd = connectSlots(
                    graph, channel.samplerInserts, channel.ids.samplerInserts,
                    channel.ids.instrument);
                graph.connect(samplerChainEnd, channel.ids.samplerFader);
                graph.connect(channel.ids.samplerFader, channel.ids.samplerMeter);
                head = channel.ids.samplerMeter;
            }
        } else {
            syncSlots(track.id, channel.samplerInserts, {});
            channel.samplerFader.reset();
            channel.samplerMeter.reset();
        }

        // Anything routed into this track joins here, ahead of the inserts.
        // Without it the arriving audio would have to be connected straight to
        // the fader — which is exactly how a bus used to end up passing signal
        // through while its plugins did nothing.
        if (receivers.contains(track.id)) {
            if (!channel.sum) {
                channel.sum = std::make_shared<engine::SumNode>(track.name + " In");
            }
            channel.ids.sum = graph.adoptNode(channel.sum);
            graph.connect(head, channel.ids.sum);
            head = channel.ids.sum;
        } else {
            channel.sum.reset();
        }

        engine::NodeId chainEnd = connectInsertChain(graph, channel, head);

        // With no instrument, the notes still have to reach the first insert —
        // that is where a user who loaded a synth into slot 1 expects them.
        // Routing them through the audio clip player instead would drop them:
        // that node is a source and writes only silence to its MIDI output.
        if (channel.ids.midiClips != engine::kInvalidNode &&
            channel.ids.instrument == engine::kInvalidNode &&
            !channel.ids.inserts.empty()) {
            graph.connect(channel.ids.midiClips, channel.ids.inserts.front());
            if (!channel.inserts.empty() &&
                channel.inserts.front().channelMode ==
                    PluginChannelMode::DualMono &&
                channel.inserts.front().rightSelectorId !=
                    engine::kInvalidNode) {
                graph.connect(channel.ids.midiClips,
                              channel.inserts.front().rightSelectorId);
            }
        }

        if (channel.ids.input != engine::kInvalidNode) {
            // The input joins at the head, so it goes through the inserts too —
            // monitoring a guitar through an amp sim is the whole point.
            const engine::NodeId entry =
                !channel.ids.inserts.empty() ? channel.ids.inserts.front()
                : channel.ids.sum != engine::kInvalidNode ? channel.ids.sum
                                                          : channel.ids.fader;
            graph.connect(channel.ids.input, entry);
            if (!channel.inserts.empty() &&
                channel.inserts.front().channelMode ==
                    PluginChannelMode::DualMono &&
                channel.inserts.front().rightSelectorId !=
                    engine::kInvalidNode) {
                graph.connect(channel.ids.input,
                              channel.inserts.front().rightSelectorId);
            }
        }
        channel.ids.preFaderTap = chainEnd;
        graph.connect(chainEnd, channel.ids.fader);
    }

    // ── Main outputs and sends, once every channel exists ──

    // Where audio arriving from elsewhere has to land. The merge point when the
    // channel has one; otherwise the head of its insert chain, so that even a
    // channel built before the receiver set was known still gets its plugins
    // fed. Never the fader: that is what skipped the inserts.
    auto channelEntry = [](const TrackChannel& channel) {
        if (channel.ids.sum != engine::kInvalidNode) return channel.ids.sum;
        return channel.ids.inserts.empty() ? channel.ids.fader
                                           : channel.ids.inserts.front();
    };

    for (const auto& track : m_project.tracks) {
        auto found = m_channels.find(track.id);
        if (found == m_channels.end()) continue;
        TrackChannel& channel = found->second;

        engine::NodeId destination = m_masterSumId;
        if (!track.outputBusId.empty()) {
            auto bus = m_channels.find(track.outputBusId);
            if (bus != m_channels.end()) destination = channelEntry(bus->second);
        }
        graph.connect(channel.ids.meter, destination);

        channel.sends.resize(track.sends.size());
        for (std::size_t i = 0; i < track.sends.size(); ++i) {
            const SendModel& send = track.sends[i];
            auto bus = m_channels.find(send.destinationTrackId);
            if (bus == m_channels.end()) continue;

            if (!channel.sends[i]) {
                channel.sends[i] =
                    std::make_shared<engine::SendNode>(track.name + " Send");
            }
            channel.sends[i]->setLevel(send.enabled ? send.level : 0.0f);
            const engine::NodeId sendId = graph.adoptNode(channel.sends[i]);
            channel.ids.sends.push_back(sendId);

            // Pre-fader taps the end of the insert chain — before the fader,
            // after the plugins, which is what "pre-fader" means everywhere.
            // Post-fader taps after the meter, so a fader move is heard on the
            // send too.
            graph.connect(send.preFader ? channel.ids.preFaderTap
                                        : channel.ids.meter,
                          sendId);
            graph.connect(sendId, channelEntry(bus->second));
        }
    }

    // ── Stem taps ──
    //
    // Leaves, deliberately: hanging one off a node that already has consumers
    // adds an edge and nothing else, so the compiled order, the buffer
    // assignment and the latency of everything else are exactly what they would
    // have been without it. Splicing a node into the chain instead would move
    // the PDC and the stems would no longer belong to the mix they came with.
    for (auto& [channelId, tap] : m_renderTaps) {
        auto found = m_channels.find(channelId);
        if (found == m_channels.end() || !tap) continue;
        const TrackNodes& ids = found->second.ids;
        const engine::NodeId source =
            m_renderTapsPreFader ? ids.preFaderTap : ids.meter;
        if (source == engine::kInvalidNode) continue;
        graph.connect(source, graph.adoptNode(tap));
    }

    // Sidechains are wired after every strip and meter exists. They are typed
    // edges: PluginNode sends them to auxiliary bus 1 and never sums them into
    // the main/dry signal. The graph's ordinary PDC aligns them automatically.
    auto connectSidechains = [&](const std::string& destinationChannelId,
                                 std::vector<InsertSlot>& slots) {
        for (InsertSlot& live : slots) {
            if (live.nodeId == engine::kInvalidNode) continue;
            const InsertModel* model = insertModel(destinationChannelId, live.slotId);
            if (!model || model->sidechainTrackId.empty()) continue;
            if (sidechainWouldFeedback(destinationChannelId,
                                       model->sidechainTrackId)) {
                continue;
            }
            const auto source = m_channels.find(model->sidechainTrackId);
            if (source == m_channels.end() ||
                source->second.ids.meter == engine::kInvalidNode) {
                continue;
            }
            graph.connect(source->second.ids.meter, live.nodeId,
                          engine::InputRole::Sidechain);
            if (live.rightNodeId != engine::kInvalidNode) {
                graph.connect(source->second.ids.meter, live.rightNodeId,
                              engine::InputRole::Sidechain);
            }
        }
    };
    for (auto& [channelId, channel] : m_channels) {
        connectSidechains(channelId, channel.instrument);
        connectSidechains(channelId, channel.samplerInserts);
        for (auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            connectSidechains(channelId, clipFx.inserts);
        }
        connectSidechains(channelId, channel.inserts);
    }

    for (const auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        syncTrackClips(track);
        syncTrackNotes(track);
        syncTrackAutomation(track);
        // The fader and the sends are new objects after a rebuild, so their
        // curves have to be published again — a track whose volume was being
        // automated would otherwise fall back to its static level.
        syncTrackLevelAutomation(track);
    }
    syncAllTrackGains();

    auto committed = m_engine.commitGraph(reconfigurePlugins);
    if (!committed) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   std::string(engine::describe(committed.error())));
    }
    if (reconfigurePlugins) {
        // VST3 may replace its parameter table during the gated activation
        // above. Re-resolve automation by stable ParamID now that the new table
        // is live; the pre-compile pass necessarily saw the old table.
        for (const auto& track : m_project.tracks) {
            if (carriesAudio(track)) syncTrackAutomation(track);
        }
    }
    updateTimelineDuration();
    return audio::Result::ok();
}

std::shared_ptr<const engine::CompiledGraph> EngineController::routingGraph() const {
    return m_engine.compiledGraph();
}

const EngineController::TrackNodes* EngineController::trackNodes(
    const std::string& trackId) const {
    auto found = m_channels.find(trackId);
    return found == m_channels.end() ? nullptr : &found->second.ids;
}

uint32_t EngineController::latencySamples() const {
    return const_cast<engine::RealtimeEngine&>(m_engine).latencySamples();
}

unsigned EngineController::workerCount() const { return m_engine.workerCount(); }

// ── Document ───────────────────────────────────────────────────────────────

void EngineController::newProject() {
    m_project = ProjectModel{};
    m_project.sampleRate = m_sampleRate;
    m_undo.clear();
    m_midiNotesRevisions.clear();
    m_samples.clear();
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_waveforms.clear();
    m_recoveryPluginStateCache.clear();
    m_recoveryPluginCaptureCursor = 0;
    m_deferredClipSync.clear();
    announceAllRetiring();
    m_channels.clear();
    m_engine.transport().stop();
    m_engine.transport().seek(0);
    rebuildGraph();
}

void EngineController::setProjectName(std::string name) {
    if (m_project.name == name) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::Name, name},
        "Rename Project");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    m_project.name = std::move(name);
}

audio::Result EngineController::saveProject(const std::string& packageDir) {
    m_project.sampleRate = m_sampleRate;
    const audio::Result stateResult = writePluginState(m_project, packageDir);
    if (!stateResult) return stateResult;
    const audio::Result projectResult = ProjectSerializer::save(m_project, packageDir);
    if (projectResult) cleanupPluginState(m_project, packageDir);
    return projectResult;
}

cloud::CloudPublicationCapture EngineController::captureCloudPublicationV1(
    const std::string& stagingParent) {
    // This entry point is deliberately synchronous and control-thread-only.
    // It is called from Publish Project before any worker hashes or uploads a
    // byte; RenderGate makes opaque state reads disjoint from process().
    cloud::CloudPublicationCapture capture(m_project);
    capture.blockers = cloud::inspectCaptureCompatibilityV1(m_project);
    if (!capture.blockers.empty()) return capture;

    capture.document.sampleRate = m_sampleRate;
    collab::ensureStableCollaborationIds(capture.document);

    struct CapturedPluginState {
        InsertModel* model = nullptr;
        std::string channelId;
        std::string location;
        std::vector<std::uint8_t> left;
        std::vector<std::uint8_t> right;
        std::string samplerPath;
    };
    std::vector<CapturedPluginState> pluginStates;

    const auto issueForSlot = [&](cloud::PublicationCaptureIssueKind kind,
                                  const InsertModel& slot,
                                  const std::string& location,
                                  std::string detail) {
        capture.addIssue({kind, location, slot.id, slot.uid, slot.name,
                          std::move(detail)});
    };

    auto captureSlot = [&](const std::string& channelId, InsertModel& slot,
                           std::string location) {
        if (!slot.isLoaded()) return;
        InsertSlot* live = liveInsertSlot(channelId, slot.id);
        plugins::PluginInstance* left =
            live && live->node ? live->node->instance() : nullptr;
        if (!left) {
            issueForSlot(
                cloud::PublicationCaptureIssueKind::MissingLivePlugin, slot,
                location,
                "the built-in slot has no live instance to capture");
            return;
        }

        const plugins::PluginDescriptor& descriptor = left->descriptor();
        if (descriptor.format != plugins::Format::Internal ||
            descriptor.uid != slot.uid || descriptor.version.empty() ||
            descriptor.stateSchemaVersion <= 0) {
            issueForSlot(
                cloud::PublicationCaptureIssueKind::UnknownInternalPlugin,
                slot, location,
                "the live built-in does not expose exact v1 version/state-schema metadata");
            return;
        }
        slot.pluginVersion = descriptor.version;
        slot.stateSchemaVersion = descriptor.stateSchemaVersion;
        snapshotParameters(*left, slot.parameters);

        CapturedPluginState state;
        state.model = &slot;
        state.channelId = channelId;
        state.location = std::move(location);
        bool leftSaved = false;
        if (auto* sampler =
                dynamic_cast<plugins::sampler::SamplerInstance*>(left)) {
            // Empty is intentional: the `sample` AssetRef binding is the only
            // durable source identity. An opaque state blob must never smuggle
            // a private local path into the cloud snapshot.
            state.samplerPath = sampler->samplePath();
            leftSaved = sampler->saveProjectState(state.left, {});
        } else {
            leftSaved = left->saveState(state.left);
        }
        if (!leftSaved || state.left.empty()) {
            issueForSlot(
                cloud::PublicationCaptureIssueKind::PluginStateCaptureFailed,
                slot, state.location,
                "the built-in plugin could not serialize its state");
            return;
        }

        if (slot.channelMode == PluginChannelMode::DualMono) {
            plugins::PluginInstance* right =
                live && live->rightNode ? live->rightNode->instance() : nullptr;
            if (!right) {
                issueForSlot(
                    cloud::PublicationCaptureIssueKind::MissingLivePlugin,
                    slot, state.location,
                    "the right half of the dual-mono slot is unavailable");
                return;
            }
            snapshotParameters(*right, slot.rightParameters);
            if (!right->saveState(state.right) || state.right.empty()) {
                issueForSlot(
                    cloud::PublicationCaptureIssueKind::PluginStateCaptureFailed,
                    slot, state.location,
                    "the right half of the dual-mono slot could not serialize its state");
                return;
            }
        } else {
            slot.rightParameters.clear();
            slot.rightStateAsset = {};
            slot.rightStateFile.clear();
        }
        pluginStates.push_back(std::move(state));
    };

    // Capture all opaque bytes in one short renderer pause. No filesystem I/O,
    // hashing or upload is performed while the gate is held.
    {
        const engine::RealtimeEngine::RenderGate gate(m_engine);
        for (std::size_t index = 0;
             index < capture.document.masterInserts.size(); ++index) {
            captureSlot(std::string(kMasterChannelId),
                        capture.document.masterInserts[index],
                        "master/insert:" + std::to_string(index));
        }
        for (std::size_t trackIndex = 0;
             trackIndex < capture.document.tracks.size(); ++trackIndex) {
            TrackModel& track = capture.document.tracks[trackIndex];
            const std::string trackLocation =
                "track:" + (track.id.empty() ? std::to_string(trackIndex)
                                              : track.id);
            if (track.instrument.isLoaded()) {
                captureSlot(track.id, track.instrument,
                            trackLocation + "/instrument");
            }
            for (std::size_t index = 0;
                 index < track.samplerFx.inserts.size(); ++index) {
                captureSlot(track.id, track.samplerFx.inserts[index],
                            trackLocation + "/sampler-fx/insert:" +
                                std::to_string(index));
            }
            for (std::size_t clipIndex = 0;
                 clipIndex < track.clips.size(); ++clipIndex) {
                ClipModel& clip = track.clips[clipIndex];
                const std::string clipLocation =
                    trackLocation + "/clip:" +
                    (clip.id.empty() ? std::to_string(clipIndex) : clip.id);
                for (std::size_t index = 0; index < clip.inserts.size();
                     ++index) {
                    captureSlot(track.id, clip.inserts[index],
                                clipLocation + "/insert:" +
                                    std::to_string(index));
                }
            }
            for (std::size_t index = 0; index < track.inserts.size(); ++index) {
                captureSlot(track.id, track.inserts[index],
                            trackLocation + "/insert:" +
                                std::to_string(index));
            }
        }
    }
    if (!capture.blockers.empty()) return capture;

    // Resolve every ordinary document-owned media path before creating the
    // staging directory. A missing source therefore leaves no partial output.
    for (std::size_t trackIndex = 0;
         trackIndex < capture.document.tracks.size(); ++trackIndex) {
        TrackModel& track = capture.document.tracks[trackIndex];
        const std::string trackLocation =
            "track:" + (track.id.empty() ? std::to_string(trackIndex)
                                          : track.id);
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            ClipModel& clip = track.clips[clipIndex];
            const std::string clipLocation =
                trackLocation + "/clip:" +
                (clip.id.empty() ? std::to_string(clipIndex) : clip.id);
            if (clip.kind == ClipKind::Audio && !clip.filePath.empty()) {
                const AssetRef preferred = clip.asset;
                (void)capture.bindLocalFile(
                    clip.asset, preferred, clip.filePath, AssetKind::Audio,
                    clipLocation + "/audio");
            } else if (clip.kind == ClipKind::Audio && !clip.asset.empty()) {
                capture.addIssue({
                    cloud::PublicationCaptureIssueKind::MissingLocalSource,
                    clipLocation + "/audio", clip.asset.assetId, {},
                    clip.asset.originalName,
                    "the audio clip has an asset identity but no local upload source"});
            }
            for (std::size_t takeIndex = 0; takeIndex < clip.takes.size();
                 ++takeIndex) {
                TakeModel& take = clip.takes[takeIndex];
                const std::string takeLocation =
                    clipLocation + "/take:" +
                    (take.id.empty() ? std::to_string(takeIndex) : take.id);
                if (!take.filePath.empty()) {
                    const AssetRef preferred = take.asset;
                    (void)capture.bindLocalFile(
                        take.asset, preferred, take.filePath, AssetKind::Audio,
                        takeLocation + "/audio");
                } else if (!take.asset.empty()) {
                    capture.addIssue({
                        cloud::PublicationCaptureIssueKind::MissingLocalSource,
                        takeLocation + "/audio", take.asset.assetId, {},
                        take.asset.originalName,
                        "the recorded take has an asset identity but no local upload source"});
                }
            }
        }
    }

    for (CapturedPluginState& state : pluginStates) {
        InsertModel& slot = *state.model;
        if (slot.uid == "daw.sampler") {
            AssetRef preferredSample;
            const auto sample = std::find_if(
                slot.assetBindings.begin(), slot.assetBindings.end(),
                [](const PluginAssetBinding& binding) {
                    return binding.key == "sample";
                });
            if (sample != slot.assetBindings.end())
                preferredSample = sample->asset;
            std::erase_if(slot.assetBindings,
                          [](const PluginAssetBinding& binding) {
                              return binding.key == "sample";
                          });
            AssetRef sampleAsset;
            if (!state.samplerPath.empty()) {
                (void)capture.bindLocalFile(
                    sampleAsset, preferredSample, state.samplerPath,
                    AssetKind::Audio, state.location + "/binding:sample");
            }
            if (!sampleAsset.empty()) {
                slot.assetBindings.push_back(
                    PluginAssetBinding{"sample", sampleAsset, true});
            }
        }
        for (const PluginAssetBinding& binding : slot.assetBindings) {
            if (slot.uid == "daw.sampler" && binding.key == "sample")
                continue;
            if (!binding.asset.empty()) {
                capture.addIssue({
                    cloud::PublicationCaptureIssueKind::MissingLocalSource,
                    state.location + "/binding:" + binding.key,
                    binding.asset.assetId, slot.uid, slot.name,
                    "this built-in asset binding has no local capture adapter"});
            }
        }
    }
    if (!capture.blockers.empty()) return capture;

    if (!capture.prepareStaging(stagingParent)) return capture;
    for (CapturedPluginState& state : pluginStates) {
        InsertModel& slot = *state.model;
        const AssetRef preferredLeft = slot.stateAsset;
        if (!capture.stagePluginState(
                slot.stateAsset, preferredLeft, state.left,
                state.location + "/state", false, slot.stateFile)) {
            (void)capture.cleanup();
            return capture;
        }
        if (slot.channelMode == PluginChannelMode::DualMono) {
            const AssetRef preferredRight = slot.rightStateAsset;
            if (!capture.stagePluginState(
                    slot.rightStateAsset, preferredRight, state.right,
                    state.location + "/right-state", true,
                    slot.rightStateFile)) {
                (void)capture.cleanup();
                return capture;
            }
        }
    }
    return capture;
}

bool EngineController::refreshRecoveryPluginStates(
    std::size_t maxPluginStateCaptures,
    std::span<const std::string> preferredStems) {
    struct Candidate {
        plugins::PluginInstance* instance = nullptr;
        std::string stem;
    };
    std::vector<Candidate> candidates;
    const auto collectSlot = [&](InsertSlot& slot) {
        if (slot.node && slot.node->instance())
            candidates.push_back({slot.node->instance(), slot.slotId});
        if (slot.rightNode && slot.rightNode->instance()) {
            candidates.push_back(
                {slot.rightNode->instance(), slot.slotId + "-right"});
        }
    };
    for (auto& [channelId, channel] : m_channels) {
        (void)channelId;
        for (InsertSlot& slot : channel.instrument) collectSlot(slot);
        for (InsertSlot& slot : channel.samplerInserts) collectSlot(slot);
        for (auto& [clipId, clipFx] : channel.clipFx) {
            (void)clipId;
            for (InsertSlot& slot : clipFx.inserts) collectSlot(slot);
        }
        for (InsertSlot& slot : channel.inserts) collectSlot(slot);
    }

    std::unordered_set<std::string> liveStems;
    liveStems.reserve(candidates.size());
    for (const Candidate& candidate : candidates)
        liveStems.insert(candidate.stem);
    const std::size_t cacheSizeBefore = m_recoveryPluginStateCache.size();
    std::erase_if(m_recoveryPluginStateCache, [&](const auto& entry) {
        return !liveStems.contains(entry.first);
    });
    bool changed = m_recoveryPluginStateCache.size() != cacheSizeBefore;

    const std::size_t budget =
        std::min(maxPluginStateCaptures, candidates.size());
    std::unordered_set<std::string> preferred(preferredStems.begin(),
                                               preferredStems.end());
    std::vector<bool> refresh(candidates.size(), false);
    std::size_t selected = 0;
    std::size_t cursorAdvance = 0;
    if (!candidates.empty()) {
        const std::size_t start =
            m_recoveryPluginCaptureCursor % candidates.size();
        // Missing states take priority, but the starting point rotates too: a
        // plugin whose saveState fails cannot starve every candidate after it.
        for (int priority = 0; priority < 3 && selected < budget; ++priority) {
            for (std::size_t step = 0;
                 step < candidates.size() && selected < budget; ++step) {
                const std::size_t index = (start + step) % candidates.size();
                const bool missing = !m_recoveryPluginStateCache.contains(
                    candidates[index].stem);
                const bool preferredCandidate =
                    preferred.contains(candidates[index].stem);
                const bool matches = priority == 0 ? missing
                                   : priority == 1 ? !missing && preferredCandidate
                                                   : !missing;
                if (refresh[index] || !matches) continue;
                refresh[index] = true;
                ++selected;
                cursorAdvance = std::max(cursorAdvance, step + 1);
            }
        }
        m_recoveryPluginCaptureCursor =
            (start + std::max<std::size_t>(1, cursorAdvance)) %
            candidates.size();
    } else {
        m_recoveryPluginCaptureCursor = 0;
    }

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!refresh[i]) continue;
        Candidate& candidate = candidates[i];
        std::vector<std::uint8_t> bytes;
        if (!candidate.instance->saveState(bytes) || bytes.empty()) continue;
        const std::string fileName = pluginStateFileName(candidate.stem, bytes);
        const auto previous = m_recoveryPluginStateCache.find(candidate.stem);
        if (previous == m_recoveryPluginStateCache.end() ||
            previous->second.fileName != fileName) {
            changed = true;
        }
        m_recoveryPluginStateCache[candidate.stem] =
            recovery::RecoverySnapshot::PluginState{fileName, std::move(bytes)};
    }
    return changed;
}

recovery::RecoverySnapshot EngineController::captureRecoverySnapshot(
    std::size_t maxPluginStateCaptures) {
    if (maxPluginStateCaptures != 0)
        (void)refreshRecoveryPluginStates(maxPluginStateCaptures);

    recovery::RecoverySnapshot snapshot;
    snapshot.project = m_project;
    snapshot.project.sampleRate = m_sampleRate;

    auto collectInstance = [&](plugins::PluginInstance* instance,
                               const std::string& stem, std::string& stateFile,
                               std::vector<InsertParameter>& parameters) {
        if (!instance) return;
        snapshotParameters(*instance, parameters);
        const auto cached = m_recoveryPluginStateCache.find(stem);
        if (cached == m_recoveryPluginStateCache.end()) return;
        stateFile = cached->second.fileName;
        snapshot.pluginStates.push_back(cached->second);
    };
    auto collectSlot = [&](const std::string& channelId, InsertModel& slot) {
        InsertSlot* live = liveInsertSlot(channelId, slot.id);
        collectInstance(live && live->node ? live->node->instance() : nullptr,
                        slot.id, slot.stateFile, slot.parameters);
        if (slot.channelMode == PluginChannelMode::DualMono) {
            collectInstance(live && live->rightNode
                                ? live->rightNode->instance()
                                : nullptr,
                            slot.id + "-right", slot.rightStateFile,
                            slot.rightParameters);
        }
    };
    auto collectChannel = [&](const std::string& channelId,
                              std::vector<InsertModel>& slots) {
        for (InsertModel& slot : slots) collectSlot(channelId, slot);
    };
    for (TrackModel& track : snapshot.project.tracks) {
        if (track.instrument.isLoaded()) collectSlot(track.id, track.instrument);
        collectChannel(track.id, track.samplerFx.inserts);
        for (ClipModel& clip : track.clips)
            collectChannel(track.id, clip.inserts);
        collectChannel(track.id, track.inserts);
    }
    collectChannel(std::string(kMasterChannelId), snapshot.project.masterInserts);
    return snapshot;
}

audio::Result EngineController::writePluginState(ProjectModel& document,
                                                 const std::string& packageDir) {
    namespace fs = std::filesystem;
    const fs::path stateDir =
        platform::pathFromUtf8(ProjectSerializer::statePath(packageDir));
    std::error_code ec;
    fs::create_directories(stateDir, ec);
    if (ec) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "cannot create plugin state directory: " +
                                       ec.message());
    }

    // The serializer only sees a const ProjectModel, and only this class holds
    // the live instances — so the chunks are written here and the document is
    // told where they went.
    audio::Result result = audio::Result::ok();
    auto saveInstance = [&](plugins::PluginInstance* instance,
                            const std::string& stem, std::string& stateFile,
                            std::vector<InsertParameter>& parameters) {
        if (!result || !instance) return;
        snapshotParameters(*instance, parameters);
        std::vector<std::uint8_t> chunk;
        bool saved = false;
        if (auto* sampler =
                dynamic_cast<plugins::sampler::SamplerInstance*>(instance)) {
            std::string packagedSample;
            result = ProjectSerializer::copyContentFile(
                sampler->samplePath(), packageDir, packagedSample);
            if (!result) return;
            saved = sampler->saveProjectState(chunk, packagedSample);
        } else {
            saved = instance->saveState(chunk);
        }
        if (!saved || chunk.empty()) return;

        const std::string file = pluginStateFileName(stem, chunk);
        const fs::path target = stateDir / file;
        if (fs::is_regular_file(target, ec) && !ec) {
            stateFile = file;
            return;
        }
        ec.clear();
        fs::path temporary = target;
        temporary += ".tmp-" + newUuid();
        std::ofstream os(temporary, std::ios::binary | std::ios::trunc);
        if (!os) {
            result = audio::Result::fail(audio::EngineError::FileWriteError,
                                         "cannot write plugin state " + file);
            return;
        }
        os.write(reinterpret_cast<const char*>(chunk.data()),
                 std::streamsize(chunk.size()));
        os.flush();
        if (!os.good()) {
            os.close();
            fs::remove(temporary, ec);
            result = audio::Result::fail(audio::EngineError::FileWriteError,
                                         "failed to write plugin state " + file);
            return;
        }
        os.close();
        ec.clear();
        fs::rename(temporary, target, ec);
        if (ec) {
            fs::remove(temporary, ec);
            stateFile.clear();
            result = audio::Result::fail(audio::EngineError::FileWriteError,
                                         "cannot publish plugin state " + file);
            return;
        }
        stateFile = file;
    };
    auto saveSlot = [&](const std::string& channelId, InsertModel& slot) {
        if (!result) return;
        InsertSlot* live = liveInsertSlot(channelId, slot.id);
        saveInstance(live && live->node ? live->node->instance() : nullptr,
                     slot.id, slot.stateFile, slot.parameters);
        if (slot.channelMode == PluginChannelMode::DualMono) {
            saveInstance(live && live->rightNode
                             ? live->rightNode->instance()
                             : nullptr,
                         slot.id + "-right", slot.rightStateFile,
                         slot.rightParameters);
        }
    };
    auto saveChannel = [&](const std::string& channelId,
                           std::vector<InsertModel>& slots) {
        for (InsertModel& slot : slots) {
            saveSlot(channelId, slot);
        }
    };

    for (TrackModel& track : document.tracks) {
        saveChannel(track.id, track.inserts);
        saveChannel(track.id, track.samplerFx.inserts);
        for (ClipModel& clip : track.clips) saveChannel(track.id, clip.inserts);
        // The instrument is a slot like any other and its state is the whole
        // instrument — a sampler's Content filename and every knob on it.
        // Leaving it out meant a project reopened with the instrument loaded
        // and empty.
        if (track.instrument.isLoaded()) saveSlot(track.id, track.instrument);
    }
    saveChannel(std::string(kMasterChannelId), document.masterInserts);

    return result;
}

void EngineController::cleanupPluginState(const ProjectModel& document,
                                          const std::string& packageDir) {
    namespace fs = std::filesystem;
    const fs::path stateDir =
        platform::pathFromUtf8(ProjectSerializer::statePath(packageDir));
    std::set<std::string> referenced;
    const auto collect = [&referenced](const std::vector<InsertModel>& slots) {
        for (const InsertModel& slot : slots) {
            if (!slot.stateFile.empty()) referenced.insert(slot.stateFile);
            if (!slot.rightStateFile.empty()) referenced.insert(slot.rightStateFile);
        }
    };
    for (const TrackModel& track : document.tracks) {
        collect(track.inserts);
        collect(track.samplerFx.inserts);
        for (const ClipModel& clip : track.clips) collect(clip.inserts);
        if (!track.instrument.stateFile.empty())
            referenced.insert(track.instrument.stateFile);
        if (!track.instrument.rightStateFile.empty())
            referenced.insert(track.instrument.rightStateFile);
    }
    collect(document.masterInserts);

    std::error_code ec;
    if (fs::is_directory(stateDir, ec)) {
        for (const auto& entry : fs::directory_iterator(stateDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (!referenced.contains(
                    platform::pathToUtf8(entry.path().filename()))) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

audio::Result EngineController::loadPluginState(
    const std::string& packageDir,
    const std::unordered_set<std::string>* channelFilter,
    bool includeMaster, const std::string& fallbackPackageDir,
    bool tolerateStateErrors) {
    namespace fs = std::filesystem;
    const fs::path stateDir =
        platform::pathFromUtf8(ProjectSerializer::statePath(packageDir));
    const fs::path fallbackStateDir = fallbackPackageDir.empty()
                                          ? fs::path{}
                                          : platform::pathFromUtf8(
                                                ProjectSerializer::statePath(
                                                    fallbackPackageDir));
    audio::Result result = audio::Result::ok();

    auto restore = [&](const std::string& channelId,
                       const std::vector<InsertModel>& slots) {
        for (const InsertModel& slot : slots) {
            if (!result) return;
            InsertSlot* live = liveInsertSlot(channelId, slot.id);
            if (!live) continue;
            auto restoreOne = [&](plugins::PluginNode* node,
                                  const std::string& stateFile,
                                  const std::vector<InsertParameter>& values) {
                if (!node || !node->instance()) return;
                plugins::PluginInstance* instance = node->instance();
                bool restored = false;
                if (!stateFile.empty()) {
                    fs::path source = stateDir / stateFile;
                    std::string contentDir = ProjectSerializer::mediaPath(packageDir);
                    std::error_code stateError;
                    if ((!fs::is_regular_file(source, stateError) || stateError) &&
                        !fallbackStateDir.empty()) {
                        stateError.clear();
                        const fs::path fallback = fallbackStateDir / stateFile;
                        if (fs::is_regular_file(fallback, stateError) && !stateError) {
                            source = fallback;
                            contentDir = ProjectSerializer::mediaPath(
                                fallbackPackageDir);
                        }
                    }
                    std::ifstream is(source, std::ios::binary);
                    if (is) {
                        const std::vector<std::uint8_t> chunk(
                            (std::istreambuf_iterator<char>(is)),
                            std::istreambuf_iterator<char>());
                        if (!chunk.empty()) {
                            if (auto* sampler = dynamic_cast<
                                    plugins::sampler::SamplerInstance*>(instance)) {
                                restored = sampler->loadProjectState(
                                    chunk, contentDir);
                                if (!restored ||
                                    (!sampler->samplePath().empty() &&
                                     !sampler->rawSample())) {
                                    if (!tolerateStateErrors) {
                                        result = audio::Result::fail(
                                            audio::EngineError::FileNotFound,
                                            "Sampler state or embedded Content is "
                                            "missing or unreadable for slot " +
                                                slot.id);
                                        return;
                                    }
                                    restored = false;
                                }
                            } else {
                                restored = instance->loadState(chunk);
                            }
                        }
                    } else if (!tolerateStateErrors &&
                               dynamic_cast<plugins::sampler::SamplerInstance*>(
                                   instance)) {
                        result = audio::Result::fail(
                            audio::EngineError::FileNotFound,
                            "Sampler state file is missing or unreadable: " +
                                stateFile);
                        return;
                    }
                }
                // A recovery manifest may contain a state chunk captured one
                // audio block before the document's newest host-side parameter
                // event. Queue the inline values after loading the chunk so the
                // newest edit wins as soon as processing resumes. Ordinary
                // project files keep their historical chunk-first behaviour.
                if (restored && !tolerateStateErrors) return;
                for (const InsertParameter& parameter : values) {
                    const std::int32_t index =
                        instance->parameterIndexForId(parameter.id);
                    if (index < 0) continue;
                    plugins::PluginEvent event;
                    event.kind = plugins::PluginEvent::Kind::ParamValue;
                    event.paramIndex = std::uint32_t(index);
                    event.value = parameter.value;
                    node->pushEvent(event);
                    instance->setParameterFromHost(std::uint32_t(index),
                                                   parameter.value);
                }
            };
            restoreOne(live->node.get(), slot.stateFile, slot.parameters);
            if (slot.channelMode == PluginChannelMode::DualMono) {
                restoreOne(live->rightNode.get(), slot.rightStateFile,
                           slot.rightParameters.empty() ? slot.parameters
                                                        : slot.rightParameters);
            }
        }
    };

    for (const TrackModel& track : m_project.tracks) {
        if (channelFilter && !channelFilter->contains(track.id)) continue;
        restore(track.id, track.inserts);
        restore(track.id, track.samplerFx.inserts);
        for (const ClipModel& clip : track.clips) restore(track.id, clip.inserts);
        if (track.instrument.isLoaded()) {
            restore(track.id, std::vector<InsertModel>{track.instrument});
        }
    }
    if (includeMaster)
        restore(std::string(kMasterChannelId), m_project.masterInserts);
    return result;
}

audio::Result EngineController::saveProjectTemplate(
    const std::string& packageDir, const std::string& templateName) {
    const fs::path target = platform::pathFromUtf8(packageDir);
    if (target.empty()) {
        return audio::Result::fail(audio::EngineError::FileWriteError,
                                   "template path is empty");
    }

    ProjectModel templ = m_project;
    templ.sampleRate = m_sampleRate;
    stripTemplateArrangement(templ, templateName);

    fs::path staging = target;
    staging += ".tmp-" + newUuid();
    fs::path backup = target;
    backup += ".backup-" + newUuid();
    std::error_code ec;
    fs::remove_all(staging, ec);

    const std::string stagingUtf8 = platform::pathToUtf8(staging);
    audio::Result result = writePluginState(templ, stagingUtf8);
    if (result) result = ProjectSerializer::save(templ, stagingUtf8);
    if (!result) {
        fs::remove_all(staging, ec);
        return result;
    }
    cleanupPluginState(templ, stagingUtf8);

    const bool replacing = fs::exists(target, ec) && !ec;
    if (replacing) {
        ec.clear();
        fs::rename(target, backup, ec);
        if (ec) {
            const std::string replaceFailure = ec.message();
            std::error_code cleanupError;
            fs::remove_all(staging, cleanupError);
            return audio::Result::fail(
                audio::EngineError::FileWriteError,
                "cannot replace existing template: " + replaceFailure);
        }
    }

    ec.clear();
    fs::rename(staging, target, ec);
    if (ec) {
        const std::string publishFailure = ec.message();
        std::error_code rollbackError;
        if (replacing) fs::rename(backup, target, rollbackError);
        fs::remove_all(staging, rollbackError);
        return audio::Result::fail(
            audio::EngineError::FileWriteError,
            "cannot publish template: " + publishFailure);
    }
    if (replacing) fs::remove_all(backup, ec);
    return audio::Result::ok();
}

audio::Result EngineController::openProject(const std::string& packageDir) {
    ProjectModel loaded;
    auto result = ProjectSerializer::load(loaded, packageDir);
    if (!result) return result;
    return activateProject(std::move(loaded), packageDir);
}

audio::Result EngineController::materializeCollaborationProject(
    ProjectModel runtimeDocument, bool clearLegacyUndo) {
    // Collaboration owns its durable history and hands us a disposable
    // runtime copy.  Keep every piece that rebuildGraph mutates so a malformed
    // route cannot leave the audible graph and the UI-facing model disagreeing.
    // This is deliberately a separate activation path: openProject() resolves
    // package-relative Content/State files and resets the playhead, while a
    // live projection must preserve this participant's transport exactly.
    const ProjectModel previousProject = m_project;
    const UndoStack previousUndo = m_undo;
    const WaveformCache previousWaveforms = m_waveforms;
    const auto previousChannels = m_channels;
    const auto previousSamples = m_samples;
    const auto previousClipSampleCache = m_clipSampleCache;
    const auto previousSharedClipSampleCache = m_sharedClipSampleCache;
    const auto previousDeferredClipSync = m_deferredClipSync;
    const auto previousMidiNotesRevisions = m_midiNotesRevisions;
    const std::uint64_t previousMidiNotesRevisionCounter =
        m_midiNotesRevisionCounter;
    const auto previousRecoveryPluginStateCache =
        m_recoveryPluginStateCache;
    const std::size_t previousRecoveryPluginCaptureCursor =
        m_recoveryPluginCaptureCursor;
    const bool previousAutomationReadoutDirty =
        m_automationReadoutCacheDirty;
    const auto previousAutomationReadoutCurves = m_automationReadoutCurves;

    const auto restorePrevious = [&](audio::Result failure) {
        m_project = previousProject;
        m_undo = previousUndo;
        m_waveforms = previousWaveforms;
        m_channels = previousChannels;
        m_samples = previousSamples;
        m_clipSampleCache = previousClipSampleCache;
        m_sharedClipSampleCache = previousSharedClipSampleCache;
        m_deferredClipSync = previousDeferredClipSync;
        m_midiNotesRevisions = previousMidiNotesRevisions;
        m_midiNotesRevisionCounter = previousMidiNotesRevisionCounter;
        m_recoveryPluginStateCache = previousRecoveryPluginStateCache;
        m_recoveryPluginCaptureCursor =
            previousRecoveryPluginCaptureCursor;
        m_automationReadoutCacheDirty = previousAutomationReadoutDirty;
        m_automationReadoutCurves = previousAutomationReadoutCurves;

        engine::Transport& transport = m_engine.transport();
        transport.setTempo(m_project.tempo);
        transport.setTimeSignature(m_project.timeSigNumerator,
                                   m_project.timeSigDenominator);
        transport.setLoopRange(toSamples(m_project.loopStartSeconds),
                               toSamples(m_project.loopEndSeconds));
        transport.setLoopEnabled(m_project.loopEnabled &&
                                 m_project.loopEndSeconds >
                                     m_project.loopStartSeconds);
        // rebuildGraph publishes a new immutable graph.  It does not seek,
        // play, pause or stop, so the local playhead/state survive both the
        // attempted projection and this rollback without being sampled and
        // reconstructed imprecisely.
        (void)rebuildGraph();
        return failure;
    };

    m_project = std::move(runtimeDocument);
    inheritAutomationLaneColors(m_project);
    // Decoded sources are immutable and keyed by absolute cache path.  A new
    // or replaced AssetRef naturally misses this memo, while an unrelated
    // rename/plugin-knob operation should not decode every unchanged clip
    // again.  Project activation may clear it; live projection retains it.
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_deferredClipSync.clear();
    m_midiNotesRevisions.clear();
    m_recoveryPluginStateCache.clear();
    m_recoveryPluginCaptureCursor = 0;
    invalidateAutomationReadoutCache();

    engine::Transport& transport = m_engine.transport();
    transport.setTempo(m_project.tempo);
    transport.setTimeSignature(m_project.timeSigNumerator,
                               m_project.timeSigDenominator);
    transport.setLoopRange(toSamples(m_project.loopStartSeconds),
                           toSamples(m_project.loopEndSeconds));
    transport.setLoopEnabled(m_project.loopEnabled &&
                             m_project.loopEndSeconds >
                                 m_project.loopStartSeconds);

    audio::Result built = rebuildGraph();
    if (!built) return restorePrevious(built);

    // Existing slots are intentionally reused by rebuildGraph.  Reusing them
    // keeps native/built-in editors and DSP continuity alive, but it also means
    // stored parameter changes must be projected explicitly.  Opaque state is
    // loaded only when its resolved cache path changed (or the slot is new),
    // avoiding a full plugin reset for an unrelated remote track rename.
    const auto previousInsert = [&](const std::string& channelId,
                                    const std::string& insertId)
        -> const InsertModel* {
        const auto findIn = [&](const std::vector<InsertModel>& slots)
            -> const InsertModel* {
            const auto found = std::find_if(
                slots.begin(), slots.end(), [&](const InsertModel& slot) {
                    return slot.id == insertId;
                });
            return found == slots.end() ? nullptr : &*found;
        };
        if (channelId == kMasterChannelId)
            return findIn(previousProject.masterInserts);
        const TrackModel* track = previousProject.findTrack(channelId);
        if (!track) return nullptr;
        if (track->instrument.id == insertId) return &track->instrument;
        if (const InsertModel* slot = findIn(track->samplerFx.inserts))
            return slot;
        for (const ClipModel& clip : track->clips) {
            if (const InsertModel* slot = findIn(clip.inserts)) return slot;
        }
        return findIn(track->inserts);
    };

    constexpr std::uintmax_t kMaximumCollaborationPluginStateBytes =
        64u * 1024u * 1024u;
    const auto readResolvedState = [&](const std::string& path,
                                       std::vector<std::uint8_t>& bytes) {
        bytes.clear();
        if (path.empty()) return false;
        const fs::path source = platform::pathFromUtf8(path);
        if (!source.is_absolute()) return false;
        std::error_code error;
        const std::uintmax_t size = fs::file_size(source, error);
        if (error || size == 0 ||
            size > kMaximumCollaborationPluginStateBytes) {
            return false;
        }
        std::ifstream input(source, std::ios::binary);
        if (!input) return false;
        bytes.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()),
                   std::streamsize(bytes.size()));
        return input.good() || input.eof();
    };

    const auto visitRuntimeSlots = [&](auto&& visitor) {
        for (const InsertModel& slot : m_project.masterInserts)
            visitor(std::string(kMasterChannelId), slot);
        for (const TrackModel& track : m_project.tracks) {
            if (track.instrument.isLoaded())
                visitor(track.id, track.instrument);
            for (const InsertModel& slot : track.samplerFx.inserts)
                visitor(track.id, slot);
            for (const ClipModel& clip : track.clips) {
                for (const InsertModel& slot : clip.inserts)
                    visitor(track.id, slot);
            }
            for (const InsertModel& slot : track.inserts)
                visitor(track.id, slot);
        }
    };

    // Read verified cache blobs while audio continues rendering.  Only the
    // plugin's in-memory loadState call needs RenderGate; parking the renderer
    // during filesystem I/O would turn a large state asset into an avoidable
    // audible dropout.
    std::unordered_map<std::string, std::vector<std::uint8_t>> stateBytes;
    std::unordered_set<std::string> attemptedStatePaths;
    const auto cacheState = [&](const std::string& path) {
        if (path.empty() || !attemptedStatePaths.insert(path).second) return;
        std::vector<std::uint8_t> bytes;
        if (readResolvedState(path, bytes))
            stateBytes.emplace(path, std::move(bytes));
    };
    visitRuntimeSlots([&](const std::string& channelId,
                          const InsertModel& slot) {
        const InsertModel* before = previousInsert(channelId, slot.id);
        const bool slotChanged = !before || before->uid != slot.uid;
        if (slotChanged || slot.stateFile != before->stateFile)
            cacheState(slot.stateFile);
        if (slot.channelMode == PluginChannelMode::DualMono &&
            (slotChanged ||
             slot.rightStateFile != before->rightStateFile)) {
            cacheState(slot.rightStateFile);
        }
    });

    {
        // Plugin state replacement is not realtime-safe.  Park at a block
        // boundary only for instance mutation.  File reads above remain on the
        // control thread but do not keep the renderer parked.
        const engine::RealtimeEngine::RenderGate gate(m_engine);
        const auto projectSlot = [&](const std::string& channelId,
                                     const InsertModel& slot) {
            InsertSlot* live = liveInsertSlot(channelId, slot.id);
            if (!live) return;
            const InsertModel* before = previousInsert(channelId, slot.id);
            const bool slotChanged = !before || before->uid != slot.uid;

            const auto projectOne = [&](plugins::PluginNode* node,
                                        const std::string& path,
                                        const std::string& previousPath,
                                        const std::vector<InsertParameter>& values) {
                if (!node || !node->instance()) return;
                if (slotChanged || path != previousPath) {
                    const auto state = stateBytes.find(path);
                    if (state != stateBytes.end())
                        (void)node->instance()->loadState(state->second);
                }
                // The inline mirror is authoritative over an older state blob
                // and is also the complete fallback for a missing blob.
                applyStoredParameters(*node, values);
            };

            projectOne(live->node.get(), slot.stateFile,
                       before ? before->stateFile : std::string{},
                       slot.parameters);
            if (slot.channelMode == PluginChannelMode::DualMono) {
                projectOne(
                    live->rightNode.get(), slot.rightStateFile,
                    before ? before->rightStateFile : std::string{},
                    slot.rightParameters.empty() ? slot.parameters
                                                  : slot.rightParameters);
            }
        };
        visitRuntimeSlots(projectSlot);
    }

    // A state blob may change a plugin's reported latency.  Recompile once
    // after all state/parameter projection, not once per slot.
    built = rebuildGraph();
    if (!built) return restorePrevious(built);
    if (clearLegacyUndo) m_undo.clear();
    retireOrphanedPendingAudioImports();
    return built;
}

audio::Result EngineController::projectCollaborationChange(
    ProjectModel runtimeDocument, const collab::ChangeImpact& impact) {
    ProjectModel previousProject = std::move(m_project);
    const bool topologyChanged =
        !sameCollaborationTopology(previousProject, runtimeDocument);
    bool pluginLayoutChanged = std::any_of(
        impact.fieldKeys.begin(), impact.fieldKeys.end(),
        [](const std::string& key) { return key.ends_with(":channelMode"); });
    if (!pluginLayoutChanged) {
        for (const std::string& trackId : impact.trackIds) {
            const TrackModel* before = previousProject.findTrack(trackId);
            const TrackModel* after = runtimeDocument.findTrack(trackId);
            if (before && after && before->mono != after->mono &&
                (hasLoadedInsert(*before) || hasLoadedInsert(*after))) {
                pluginLayoutChanged = true;
                break;
            }
        }
    }
    const auto previousChannels =
        topologyChanged ? m_channels : decltype(m_channels){};
    m_project = std::move(runtimeDocument);
    inheritAutomationLaneColors(m_project);

    const auto syncTransport = [this] {
        engine::Transport& transport = m_engine.transport();
        transport.setTempo(m_project.tempo);
        transport.setTimeSignature(m_project.timeSigNumerator,
                                   m_project.timeSigDenominator);
    };
    if (impact.transportProjectionChanged) syncTransport();

    if (topologyChanged) {
        // One outer reducer batch owns one topology publication.  Built-in V1
        // plugins expose their state through the inline parameter mirror, so
        // no second latency/reconciliation rebuild is required here.
        audio::Result built = rebuildGraph(pluginLayoutChanged);
        if (!built) {
            m_project = std::move(previousProject);
            m_channels = previousChannels;
            syncTransport();
            (void)rebuildGraph(pluginLayoutChanged);
            return built;
        }
    } else {
        if (impact.masterGainChanged && m_masterFader) {
            m_masterFader->setGain(m_project.masterVolume);
            m_masterFader->setPan(m_project.masterPan);
        }

        const bool clipsChanged =
            !impact.clipIds.empty() || !impact.takeIds.empty() ||
            !impact.compSegmentIds.empty() ||
            (impact.transportProjectionChanged && impact.timelineChanged);
        const bool notesChanged =
            !impact.noteIds.empty() || !impact.controllerLaneIds.empty() ||
            clipsChanged;
        const bool automationChanged =
            !impact.automationPointIds.empty() ||
            !impact.controllerLaneIds.empty() || clipsChanged;

        const SoloState solo = soloState();
        for (const std::string& trackId : impact.trackIds) {
            const TrackModel* track = m_project.findTrack(trackId);
            if (!track) continue;
            syncTrackGain(*track, solo);

            auto live = m_channels.find(trackId);
            if (live != m_channels.end()) {
                const std::size_t sendCount =
                    std::min(track->sends.size(), live->second.sends.size());
                for (std::size_t index = 0; index < sendCount; ++index) {
                    if (live->second.sends[index]) {
                        const SendModel& send = track->sends[index];
                        live->second.sends[index]->setLevel(
                            send.enabled ? send.level : 0.0f);
                    }
                }
                if (live->second.samplerFader) {
                    live->second.samplerFader->setGain(track->samplerFx.volume);
                    live->second.samplerFader->setPan(track->samplerFx.pan);
                }
            }

            if (clipsChanged) syncTrackClips(*track);
            if (clipsChanged && live != m_channels.end()) {
                for (const ClipModel& clip : track->clips) {
                    if (!impact.clipIds.contains(clip.id)) continue;
                    const auto clipFx = live->second.clipFx.find(clip.id);
                    if (clipFx == live->second.clipFx.end() ||
                        !clipFx->second.fader) {
                        continue;
                    }
                    clipFx->second.fader->setGain(clip.gain);
                    clipFx->second.fader->setPan(clip.pan);
                }
            }
            if (notesChanged) syncTrackNotes(*track);
            if (automationChanged) {
                syncTrackAutomation(*track);
                syncTrackLevelAutomation(*track);
            }
        }
        if (impact.timelineChanged) updateTimelineDuration();
    }

    const auto locateInsert = [](const ProjectModel& project,
                                 const std::string& insertId)
        -> std::pair<std::string, const InsertModel*> {
        const auto findIn = [&](const std::vector<InsertModel>& slots)
            -> const InsertModel* {
            const auto found = std::find_if(
                slots.begin(), slots.end(), [&](const InsertModel& slot) {
                    return slot.id == insertId;
                });
            return found == slots.end() ? nullptr : &*found;
        };
        if (const InsertModel* slot = findIn(project.masterInserts))
            return {std::string(kMasterChannelId), slot};
        for (const TrackModel& track : project.tracks) {
            if (track.instrument.id == insertId)
                return {track.id, &track.instrument};
            if (const InsertModel* slot = findIn(track.samplerFx.inserts))
                return {track.id, slot};
            for (const ClipModel& clip : track.clips) {
                if (const InsertModel* slot = findIn(clip.inserts))
                    return {track.id, slot};
            }
            if (const InsertModel* slot = findIn(track.inserts))
                return {track.id, slot};
        }
        return {};
    };

    std::set<std::string> pluginIds = impact.pluginInsertIds;
    if (topologyChanged) {
        const auto appendIds = [&](const std::vector<InsertModel>& slots) {
            for (const InsertModel& slot : slots) {
                if (!slot.id.empty()) pluginIds.insert(slot.id);
            }
        };
        appendIds(m_project.masterInserts);
        for (const TrackModel& track : m_project.tracks) {
            if (!track.instrument.id.empty())
                pluginIds.insert(track.instrument.id);
            appendIds(track.samplerFx.inserts);
            for (const ClipModel& clip : track.clips) appendIds(clip.inserts);
            appendIds(track.inserts);
        }
    }

    struct PluginRuntimeProjection {
        std::string channelId;
        const InsertModel* slot = nullptr;
        bool loadLeft = false;
        bool loadRight = false;
        std::vector<std::uint8_t> leftState;
        std::vector<std::uint8_t> rightState;
    };
    std::vector<PluginRuntimeProjection> pluginProjections;
    pluginProjections.reserve(pluginIds.size());
    constexpr std::uintmax_t kMaximumStateBytes = 64u * 1024u * 1024u;
    const auto readState = [&](const std::string& path,
                               std::vector<std::uint8_t>& bytes) {
        if (path.empty()) return;
        const fs::path source = platform::pathFromUtf8(path);
        if (!source.is_absolute()) return;
        std::error_code error;
        const std::uintmax_t size = fs::file_size(source, error);
        if (error || size == 0 || size > kMaximumStateBytes) return;
        std::ifstream input(source, std::ios::binary);
        if (!input) return;
        bytes.resize(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()),
                   std::streamsize(bytes.size()));
        if (!input.good() && !input.eof()) bytes.clear();
    };
    for (const std::string& insertId : pluginIds) {
        const auto [channelId, slot] = locateInsert(m_project, insertId);
        if (!slot) continue;
        const auto [previousChannelId, before] =
            locateInsert(previousProject, insertId);
        PluginRuntimeProjection projection;
        projection.channelId = channelId;
        projection.slot = slot;
        projection.loadLeft = topologyChanged || !before ||
                              previousChannelId != channelId ||
                              before->stateFile != slot->stateFile;
        projection.loadRight = topologyChanged || !before ||
                               previousChannelId != channelId ||
                               before->rightStateFile != slot->rightStateFile;
        if (projection.loadLeft) readState(slot->stateFile, projection.leftState);
        if (projection.loadRight)
            readState(slot->rightStateFile, projection.rightState);
        pluginProjections.push_back(std::move(projection));
    }

    if (!pluginProjections.empty()) {
        const engine::RealtimeEngine::RenderGate gate(m_engine);
        for (const PluginRuntimeProjection& projection : pluginProjections) {
            InsertSlot* live =
                liveInsertSlot(projection.channelId, projection.slot->id);
            if (!live) continue;
            const InsertModel& slot = *projection.slot;
            const auto apply = [&](plugins::PluginNode* node,
                                   bool loadState,
                                   const std::vector<std::uint8_t>& state,
                                   const std::vector<InsertParameter>& values) {
                if (!node || !node->instance()) return;
                node->setBypassed(slot.bypassed);
                node->setMix(slot.mix);
                if (loadState && !state.empty())
                    (void)node->instance()->loadState(state);
                applyStoredParameters(*node, values);
            };
            apply(live->node.get(), projection.loadLeft,
                  projection.leftState, slot.parameters);
            if (slot.channelMode == PluginChannelMode::DualMono) {
                apply(live->rightNode.get(), projection.loadRight,
                      projection.rightState,
                      slot.rightParameters.empty() ? slot.parameters
                                                   : slot.rightParameters);
            }
        }
        m_recoveryPluginStateCache.clear();
        m_recoveryPluginCaptureCursor = 0;
    }
    // An undone or remotely deleted import leaves an upload with nothing to
    // attach to. This is the point where the document has settled, so it is
    // where that upload is called off.
    retireOrphanedPendingAudioImports();
    return audio::Result::ok();
}

audio::Result EngineController::restoreRecoveryProject(
    ProjectModel snapshot, const std::string& recoveryDir,
    const std::string& originalPackageDir) {
    // Recovery state is best-effort: one corrupt or unavailable plugin must not
    // prevent the rest of the user's project from coming back. The inline
    // parameter snapshot remains the fallback for that individual slot.
    return activateProject(std::move(snapshot), recoveryDir,
                           originalPackageDir,
                           /*toleratePluginStateErrors=*/true);
}

audio::Result EngineController::openProjectTemplate(
    const std::string& packageDir) {
    ProjectModel loaded;
    auto result = ProjectSerializer::load(loaded, packageDir);
    if (!result) return result;
    stripTemplateArrangement(loaded);
    return activateProject(std::move(loaded), packageDir);
}

audio::Result EngineController::activateProject(
    ProjectModel loaded, const std::string& packageDir,
    const std::string& fallbackPackageDir,
    bool toleratePluginStateErrors) {
    // Opening a document is transactional at the model/runtime boundary. A
    // malformed routing graph can fail only after plugin instances and nodes
    // have started being reconciled, so keep the complete live view until both
    // graph passes have succeeded. In particular, opening a broken template
    // must not strand the user in a half-replaced project.
    const ProjectModel previousProject = m_project;
    const UndoStack previousUndo = m_undo;
    const WaveformCache previousWaveforms = m_waveforms;
    const auto previousChannels = m_channels;
    const auto previousSamples = m_samples;
    const auto previousClipSampleCache = m_clipSampleCache;
    const auto previousDeferredClipSync = m_deferredClipSync;
    const auto previousMidiNotesRevisions = m_midiNotesRevisions;
    engine::Transport& transport = m_engine.transport();
    const engine::TransportState previousTransportState = transport.state();
    const engine::SamplePos previousPosition = transport.position();

    const auto restorePreviousProject = [&](audio::Result failure) {
        announceAllRetiring();
        m_project = previousProject;
        m_undo = previousUndo;
        m_waveforms = previousWaveforms;
        m_channels = previousChannels;
        m_samples = previousSamples;
        m_clipSampleCache = previousClipSampleCache;
        m_deferredClipSync = previousDeferredClipSync;
        m_midiNotesRevisions = previousMidiNotesRevisions;

        transport.setTempo(m_project.tempo);
        transport.setTimeSignature(m_project.timeSigNumerator,
                                   m_project.timeSigDenominator);
        transport.setLoopRange(toSamples(m_project.loopStartSeconds),
                               toSamples(m_project.loopEndSeconds));
        transport.setLoopEnabled(m_project.loopEnabled &&
                                 m_project.loopEndSeconds >
                                     m_project.loopStartSeconds);
        transport.seek(previousPosition);
        (void)rebuildGraph();
        switch (previousTransportState) {
        case engine::TransportState::Playing: transport.play(); break;
        case engine::TransportState::Paused: transport.pause(); break;
        case engine::TransportState::Recording: transport.startRecording(); break;
        case engine::TransportState::Stopped: transport.stop(); break;
        }
        return failure;
    };

    m_project = std::move(loaded);
    repairPatternClips();
    inheritAutomationLaneColors(m_project);
    m_samples.clear();
    m_clipSampleCache.clear();
    m_sharedClipSampleCache.clear();
    m_deferredClipSync.clear();
    m_midiNotesRevisions.clear();
    m_recoveryPluginStateCache.clear();
    m_recoveryPluginCaptureCursor = 0;
    announceAllRetiring();
    m_channels.clear();
    m_waveforms.clear();
    m_engine.transport().setTempo(m_project.tempo);
    m_engine.transport().setTimeSignature(m_project.timeSigNumerator,
                                          m_project.timeSigDenominator);
    // The cycle comes back with the arrangement it belongs to. Through the
    // transport directly: the setters above would write the values back into
    // the document they were just read from.
    m_engine.transport().setLoopRange(toSamples(m_project.loopStartSeconds),
                                      toSamples(m_project.loopEndSeconds));
    m_engine.transport().setLoopEnabled(m_project.loopEnabled &&
                                        m_project.loopEndSeconds >
                                            m_project.loopStartSeconds);
    m_engine.transport().seek(0);

    // The first rebuild instantiates every plugin the project refers to; state
    // can only be restored once they exist.
    auto built = rebuildGraph();
    if (!built) return restorePreviousProject(built);
    audio::Result state = loadPluginState(
        packageDir, nullptr, true, fallbackPackageDir,
        toleratePluginStateErrors);
    if (!state) return restorePreviousProject(state);
    // A restored preset can report a different latency than the plugin's
    // default, so compensation has to be recomputed — once, here, rather than
    // per track as each one loads.
    built = rebuildGraph();
    if (!built) return restorePreviousProject(built);
    m_undo.clear();
    return built;
}

audio::Result EngineController::importProjectTemplateTracks(
    const std::string& packageDir,
    std::vector<std::string>& outTrackIds) {
    outTrackIds.clear();

    ProjectModel imported;
    audio::Result loaded = ProjectSerializer::load(imported, packageDir);
    if (!loaded) return loaded;
    stripTemplateArrangement(imported);
    if (imported.tracks.empty()) {
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "template contains no tracks");
    }
    remapTemplateTrackIds(imported);

    if (cloudProjectBound()) {
        const double bar = beatsToSeconds(
            double(std::max(1, m_project.timeSigNumerator)) * 4.0 /
                double(std::max(1, m_project.timeSigDenominator)),
            m_project.tempo);
        for (TrackModel& track : imported.tracks) {
            if (track.kind != TrackKind::Pattern) continue;
            ClipModel owner;
            owner.id = newUuid();
            owner.name = track.name;
            owner.kind = ClipKind::Pattern;
            owner.durationSeconds = bar;
            owner.color = track.color;
            track.clips.push_back(std::move(owner));
        }
        inheritAutomationLaneColors(imported);

        const auto hasUnstagedState = [](const InsertModel& insert) {
            return insert.isLoaded() &&
                   ((!insert.stateFile.empty() && insert.stateAsset.empty()) ||
                    (!insert.rightStateFile.empty() &&
                     insert.rightStateAsset.empty()));
        };
        for (const TrackModel& track : imported.tracks) {
            if (hasUnstagedState(track.instrument) ||
                std::any_of(track.samplerFx.inserts.begin(),
                            track.samplerFx.inserts.end(), hasUnstagedState) ||
                std::any_of(track.inserts.begin(), track.inserts.end(),
                            hasUnstagedState)) {
                outTrackIds.clear();
                return audio::Result::fail(
                    audio::EngineError::InvalidArgument,
                    "template plugin state must be uploaded before cloud import");
            }
        }

        auto batch = std::make_shared<collab::BatchCommand>();
        std::string anchor = m_project.tracks.empty()
                                 ? std::string()
                                 : m_project.tracks.back().id;
        outTrackIds.reserve(imported.tracks.size());
        for (const TrackModel& track : imported.tracks) {
            appendCommand(batch, collab::AddTrack{
                track.id, track.kind, track.name, track.color, {}, anchor});
            anchor = track.id;
            outTrackIds.push_back(track.id);
        }
        for (const TrackModel& track : imported.tracks) {
            if (!appendSharedTrackContents(batch, track)) {
                outTrackIds.clear();
                return audio::Result::fail(
                    audio::EngineError::InvalidArgument,
                    "template contains unsupported shared plugin state");
            }
        }
        for (const TrackModel& track : imported.tracks) {
            if (!track.parentId.empty()) {
                appendCommand(batch,
                              collab::SetTrackParent{track.id,
                                                     track.parentId});
            }
        }
        if (!sharedBatchApplies(m_project, batch)) {
            outTrackIds.clear();
            return audio::Result::fail(
                audio::EngineError::InvalidArgument,
                "template cannot be represented as one shared batch");
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            "Add Tracks from Template");
        if (result == collab::SharedMutationResult::Submitted)
            return audio::Result::ok();
        outTrackIds.clear();
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "cloud template import was blocked");
    }

    const ProjectModel before = m_project;
    outTrackIds.reserve(imported.tracks.size());
    for (const TrackModel& track : imported.tracks)
        outTrackIds.push_back(track.id);
    const std::unordered_set<std::string> importedIds(outTrackIds.begin(),
                                                       outTrackIds.end());

    m_project.tracks.insert(m_project.tracks.end(),
                            std::make_move_iterator(imported.tracks.begin()),
                            std::make_move_iterator(imported.tracks.end()));
    repairPatternClips();
    inheritAutomationLaneColors(m_project);
    m_deferredClipSync.clear();

    audio::Result built = rebuildGraph();
    if (built) {
        built = loadPluginState(packageDir, &importedIds,
                                /*includeMaster=*/false);
        if (built) built = rebuildGraph();
    }
    if (!built) {
        m_project = before;
        m_deferredClipSync.clear();
        (void)rebuildGraph();
        outTrackIds.clear();
        return built;
    }
    updateTimelineDuration();

    const ProjectModel after = m_project;
    const auto apply = [this, packageDir,
                        importedIds](const ProjectModel& state,
                                     bool restoreImportedState) {
        m_project = state;
        inheritAutomationLaneColors(m_project);
        m_deferredClipSync.clear();
        (void)rebuildGraph();
        if (restoreImportedState) {
            (void)loadPluginState(packageDir, &importedIds,
                                  /*includeMaster=*/false);
            (void)rebuildGraph();
        }
        updateTimelineDuration();
    };
    m_undo.push("Add Tracks from Template",
                [apply, before] { apply(before, false); },
                [apply, after] { apply(after, true); });
    return audio::Result::ok();
}

void EngineController::repairPatternClips() {
    const double bar = beatsToSeconds(
        double(std::max(1, m_project.timeSigNumerator)) * 4.0 /
            double(std::max(1, m_project.timeSigDenominator)),
        m_project.tempo);

    std::vector<std::string> patternIds;
    for (const TrackModel& track : m_project.tracks) {
        if (track.kind == TrackKind::Pattern) patternIds.push_back(track.id);
    }

    for (const std::string& patternId : patternIds) {
        TrackModel* pattern = m_project.findTrack(patternId);
        if (!pattern) continue;

        std::vector<std::string> childIds = subtreeOf(m_project, patternId);
        std::vector<ClipModel*> patternClips;
        for (ClipModel& clip : pattern->clips) {
            if (clip.kind == ClipKind::Pattern) patternClips.push_back(&clip);
        }

        if (patternClips.empty()) {
            double first = std::numeric_limits<double>::max();
            double last = 0.0;
            for (const std::string& childId : childIds) {
                const TrackModel* child = m_project.findTrack(childId);
                if (!child) continue;
                for (const ClipModel& clip : child->clips) {
                    if (clip.kind != ClipKind::Midi) continue;
                    first = std::min(first, clip.startSeconds);
                    last = std::max(last,
                                    clip.startSeconds + clip.durationSeconds);
                }
            }
            if (!std::isfinite(first)) first = 0.0;

            ClipModel container;
            container.id = newUuid();
            container.name = pattern->name;
            container.kind = ClipKind::Pattern;
            container.startSeconds = first;
            container.durationSeconds = std::max(bar, last - first);
            container.color = pattern->color;
            pattern->clips.push_back(std::move(container));
            patternClips.push_back(&pattern->clips.back());
        }

        std::unordered_set<std::string> validOwners;
        for (const ClipModel* clip : patternClips) validOwners.insert(clip->id);

        // Old projects had only geometry. Assign every child clip to the
        // container it overlaps most; the usual one-container project becomes
        // an exact migration, while a hand-edited file with several instances
        // still gets a deterministic result.
        for (const std::string& childId : childIds) {
            TrackModel* child = m_project.findTrack(childId);
            if (!child) continue;
            for (ClipModel& clip : child->clips) {
                if (clip.kind != ClipKind::Midi ||
                    validOwners.contains(clip.patternClipId)) {
                    continue;
                }
                ClipModel* best = patternClips.front();
                double bestOverlap = -1.0;
                const double clipEnd = clip.startSeconds + clip.durationSeconds;
                for (ClipModel* candidate : patternClips) {
                    const double end = candidate->startSeconds +
                                       candidate->durationSeconds;
                    const double overlap = std::max(
                        0.0, std::min(clipEnd, end) -
                                 std::max(clip.startSeconds,
                                          candidate->startSeconds));
                    if (overlap > bestOverlap) {
                        best = candidate;
                        bestOverlap = overlap;
                    }
                }
                clip.patternClipId = best->id;
            }
        }
    }
}

void EngineController::pushProjectSnapshotUndo(const ProjectModel& before,
                                               const std::string& label) {
    const ProjectModel after = m_project;
    auto apply = [this](const ProjectModel& state) {
        m_project = state;
        inheritAutomationLaneColors(m_project);
        m_deferredClipSync.clear();
        rebuildGraph();
        updateTimelineDuration();
    };
    m_undo.push(label, [apply, before] { apply(before); },
                [apply, after] { apply(after); });
}

// ── Transport ──────────────────────────────────────────────────────────────

void EngineController::applyTransportStartPolicy() {
    if (m_playbackMode == PlaybackMode::Restart)
        m_engine.transport().seekSeconds(m_playAnchorSeconds);
}

void EngineController::play() {
    // An audition is a thing you do *instead* of playing; letting it run into
    // the transport would put a stray sample over the first bar, and into a
    // take if the run is a recording.
    stopPreview();
    // Pressing play is a request to hear the project as it stands, so a clip
    // whose bake is still queued is rendered now rather than a tick into the run.
    flushDeferredClipSync();
    flushSamplerPrecompute();
    applyTransportStartPolicy();
    auto& t = m_engine.transport();
    // With a cycle armed the playhead travels round the region and nowhere
    // else — so pressing play from outside it starts inside it. The transport
    // wraps at the end but has no way to pull the position *in*, and without
    // this a cycle set past the playhead simply never happens.
    if (isLoopEnabled()) {
        // In samples, not seconds: the loop bounds *are* sample positions, and
        // converting them out and back to land on one is a round trip that can
        // only lose.
        const engine::SamplePos from = t.loopStart();
        const engine::SamplePos to = t.loopEnd();
        const engine::SamplePos at = t.position();
        if (to > from && (at < from || at >= to)) t.seek(from);
    }
    t.play();
}
void EngineController::stop() { m_engine.transport().stop(); }
void EngineController::pause() { m_engine.transport().pause(); }
void EngineController::seekSeconds(double seconds) {
    // A recording owns the transport until it is stopped. Letting any UI
    // surface relocate it mid-take makes the visible playhead disagree with
    // the recorder's continuous stream and can create a malformed clip.
    if (isRecording()) return;
    const double s = std::max(0.0, seconds);
    // Any repositioning while stopped/paused is the start of the next run, so
    // Restart mode can return there. Seeks during playback don't move it.
    if (!m_engine.transport().isPlaying()) m_playAnchorSeconds = s;
    m_engine.transport().seekSeconds(s);
}
double EngineController::positionSeconds() const {
    return m_engine.transport().positionSeconds();
}
double EngineController::presentationPositionSeconds() const {
    return m_engine.transport().presentationPositionSeconds();
}
bool EngineController::isPlaying() const {
    return m_engine.transport().isPlaying();
}
double EngineController::durationSeconds() const {
    return toSeconds(m_engine.transport().duration());
}

// Tempo and metre live on the transport and reach the graph through each
// block's ProcessContext, so the click, the clips and any hosted plugin all
// change over on the same block instead of whenever each was told separately.
void EngineController::setTempo(double bpm) {
    if (bpm <= 0.0 || m_project.tempo == bpm) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::Tempo, bpm},
        "Set Tempo");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    const double previous = m_project.tempo;
    m_project.tempo = bpm;
    m_engine.transport().setTempo(bpm);
    // Everything the tempo decides the position of moves with it: clips onto
    // the bars they were written against, and the loop and the playhead with
    // them. Without this the grid slides out from under a project the moment
    // its tempo is touched.
    retimeToTempo(previous, bpm);
    // Clip starts live in seconds while note and automation snapshots live in
    // absolute beats. Rebuild those derived positions whenever the conversion
    // ratio changes, otherwise the curves and notes retain the previous tempo.
    syncAllNotes();
    syncAllAutomation();

    // Undoable: a note's position is in beats, so the tempo decides where every
    // MIDI clip actually sounds, and the tempo prompt on a MIDI import can move
    // it without the user ever typing in the field.
    m_undo.push("Set Tempo",
                [this, previous] { setTempo(previous); },
                [this, bpm] { setTempo(bpm); });
}

// A project is written in bars, not in seconds. The document stores clip times
// in seconds because that is what the engine plays, so a tempo change has to
// carry them: a clip left at its old second lands on a different bar the moment
// the grid stretches, which reads as every clip in the song sliding sideways.
//
// What each kind keeps is what it *is*:
//   - Every clip keeps the beat it starts on. Nothing changes place musically.
//   - A MIDI clip keeps its length in beats — it is music, its notes are already
//     in beats, and a bar of it stays a bar of it.
//   - An audio clip keeps its length in seconds, because nothing is stretched.
//     The same sound simply covers more bars at a faster tempo, which is the
//     "clips get longer" a tempo change is supposed to produce.
void EngineController::retimeToTempo(double from, double to) {
    if (from <= 0.0 || to <= 0.0 || from == to) return;
    const double ratio = from / to;    // one beat is this much longer now

    for (auto& track : m_project.tracks) {
        for (auto& clip : track.clips) {
            clip.startSeconds *= ratio;
            if (clip.kind == ClipKind::Midi) {
                clip.durationSeconds *= ratio;
                // Fades on a MIDI clip are musical too; audio keeps its own,
                // since the material under them has not moved.
                clip.fadeInSeconds *= ratio;
                clip.fadeOutSeconds *= ratio;
            }
        }
        syncTrackClips(track);
    }

    // The loop is a musical range — bar 5 to bar 9 stays bar 5 to bar 9.
    const double loopFrom = loopStartSeconds();
    const double loopTo = loopEndSeconds();
    if (loopTo > loopFrom) setLoopRangeSeconds(loopFrom * ratio, loopTo * ratio);
    m_playAnchorSeconds *= ratio;

    // The playhead is musical as well, so the transport stays on the beat it
    // was parked on. A take in progress is the one thing that is not: its file
    // is being written against the clock the transport is running on right now,
    // and moving that clock mid-capture would misplace everything recorded
    // after it.
    if (!isRecording()) {
        const double position = m_engine.transport().positionSeconds();
        if (position > 0.0) m_engine.transport().seekSeconds(position * ratio);
    }

    updateTimelineDuration();
}

collab::SharedMutationResult EngineController::setTimeSignature(
    int numerator, int denominator) {
    if (m_project.timeSigNumerator == numerator &&
        m_project.timeSigDenominator == denominator)
        return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result =
            m_sharedMutationSink->setTimeSignature(numerator, denominator);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    const int previousNum = m_project.timeSigNumerator;
    const int previousDen = m_project.timeSigDenominator;
    m_project.timeSigNumerator = numerator;
    m_project.timeSigDenominator = denominator;
    m_engine.transport().setTimeSignature(numerator, denominator);

    m_undo.push(
        "Set Time Signature",
        [this, previousNum, previousDen] {
            setTimeSignature(previousNum, previousDen);
        },
        [this, numerator, denominator] {
            setTimeSignature(numerator, denominator);
        });
    return collab::SharedMutationResult::LocalFallback;
}

void EngineController::restoreProject(const ProjectModel& snapshot,
                                       const std::string& label) {
    if (cloudProjectBound()) return;
    const ProjectModel before = m_project;
    auto apply = [this](const ProjectModel& state) {
        m_project = state;
        // Every track is about to be synced from the restored document, so
        // anything queued against the document being replaced is moot.
        m_deferredClipSync.clear();
        rebuildGraph();
        updateTimelineDuration();
    };
    apply(snapshot);

    const ProjectModel after = m_project;
    m_undo.push(label, [apply, before] { apply(before); },
                [apply, after] { apply(after); });
}

void EngineController::commitProjectGesture(const ProjectModel& before,
                                             std::size_t undoDepthBefore,
                                             const std::string& label) {
    if (cloudProjectBound()) return;
    // Some complex gestures use ordinary commands while live (region splitting
    // and the eraser are the two examples). Their internal entries are not the
    // user's actions, so replace them with the exact document endpoints.
    m_undo.discardSince(undoDepthBefore);
    pushProjectSnapshotUndo(before, label);
}

collab::SharedMutationResult EngineController::setProjectKey(
    int root, const std::string& scaleId) {
    const int pitchClass = ((root % 12) + 12) % 12;
    if (m_project.keyRoot == pitchClass && m_project.scale == scaleId)
        return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result =
            m_sharedMutationSink->setProjectKey(pitchClass, scaleId);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    const int previousRoot = m_project.keyRoot;
    const std::string previousScale = m_project.scale;
    m_project.keyRoot = pitchClass;
    m_project.scale = scaleId;

    m_undo.push("Set Key",
                [this, previousRoot, previousScale] {
                    setProjectKey(previousRoot, previousScale);
                },
                [this, pitchClass, scaleId] { setProjectKey(pitchClass, scaleId); });
    return collab::SharedMutationResult::LocalFallback;
}

collab::SharedMutationResult EngineController::setAiInstructions(
    std::string text) {
    if (m_project.aiInstructions == text)
        return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result = m_sharedMutationSink->setAiInstructions(text);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    m_project.aiInstructions = std::move(text);
    return collab::SharedMutationResult::LocalFallback;
}

void EngineController::setMetronomeEnabled(bool enabled) {
    m_metronomeEnabled = enabled;
    if (m_metronome) m_metronome->setEnabled(enabled);
}

bool EngineController::setMetronomeSample(const std::string& filePath) {
    if (filePath.empty()) {
        m_metronomeSamplePath.clear();
        if (m_metronome) m_metronome->setSample({});
        return true;
    }
    std::shared_ptr<const engine::SampleBuffer> sample = loadSamples(filePath);
    if (!sample || sample->frames() == 0) return false;
    m_metronomeSamplePath = filePath;
    if (!m_metronome)
        m_metronome = std::make_shared<engine::MetronomeNode>();
    m_metronome->setEnabled(m_metronomeEnabled);
    m_metronome->setSample(std::move(sample));
    return true;
}

void EngineController::setLoopEnabled(bool enabled) {
    m_engine.transport().setLoopEnabled(enabled);
    // Mirrored into the document as it is set, so saving needs no separate
    // "collect the transport state" pass that could be forgotten.
    m_project.loopEnabled = enabled;
}
bool EngineController::isLoopEnabled() const {
    return m_engine.transport().isLoopEnabled();
}
void EngineController::setLoopRangeSeconds(double startSeconds, double endSeconds) {
    m_engine.transport().setLoopRange(toSamples(startSeconds), toSamples(endSeconds));
    m_project.loopStartSeconds = std::max(0.0, startSeconds);
    m_project.loopEndSeconds = std::max(0.0, endSeconds);
}
double EngineController::loopStartSeconds() const {
    return toSeconds(m_engine.transport().loopStart());
}
double EngineController::loopEndSeconds() const {
    return toSeconds(m_engine.transport().loopEnd());
}

// ── Tracks ─────────────────────────────────────────────────────────────────

std::string EngineController::addTrack(TrackKind kind, const std::string& name) {
    TrackModel model;
    model.id = newUuid();
    model.kind = kind;
    model.name = name.empty() ? defaultTrackName(kind) : name;
    model.color = defaultTrackColor(kind);
    const std::string afterId = m_project.tracks.empty()
        ? std::string()
        : m_project.tracks.back().id;
    const auto shared = submitSharedMutation(
        collab::AddTrack{model.id, model.kind, model.name, model.color, {},
                         afterId},
        "Add Track");
    if (shared == collab::SharedMutationResult::Submitted) return model.id;
    if (shared == collab::SharedMutationResult::Blocked) return {};
    return appendTrack(std::move(model));
}

std::string EngineController::addFolder(bool summing, const std::string& name) {
    TrackModel model;
    model.id = newUuid();
    model.kind = TrackKind::Folder;
    model.summing = summing;
    // Two different things deserve two different default names: one is a bus
    // with tracks in it, the other is a drawer.
    model.name = !name.empty() ? name : (summing ? "Group" : "Folder");
    model.color = defaultTrackColor(summing ? TrackKind::Group
                                            : TrackKind::Folder);
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::AddTrack{
            model.id, model.kind, model.name, model.color, {},
            m_project.tracks.empty() ? std::string()
                                     : m_project.tracks.back().id});
        if (summing) {
            appendCommand(batch, collab::SetTrackProperty{
                model.id, collab::TrackProperty::Summing, true});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Add Folder");
        return result == collab::SharedMutationResult::Submitted ? model.id
                                                                 : std::string{};
    }
    return appendTrack(std::move(model));
}

std::string EngineController::addPattern(const std::string& name) {
    TrackModel model;
    model.id = newUuid();
    model.kind = TrackKind::Pattern;
    model.summing = true;
    model.name = name.empty() ? "Pattern" : name;
    model.color = defaultTrackColor(TrackKind::Pattern);

    // A Pattern is visible and editable on the arrangement from the moment it
    // is created. The child tracks carry the notes and audio routing; this clip
    // is their persistent container and remains on the parent lane when it is
    // expanded.
    ClipModel clip;
    clip.id = newUuid();
    clip.name = model.name;
    clip.kind = ClipKind::Pattern;
    clip.durationSeconds = beatsToSeconds(
        double(std::max(1, m_project.timeSigNumerator)) * 4.0 /
            double(std::max(1, m_project.timeSigDenominator)),
        m_project.tempo);
    clip.color = model.color;
    model.clips.push_back(std::move(clip));
    if (cloudProjectBound()) {
        const ClipModel& patternClip = model.clips.front();
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::AddTrack{
            model.id, model.kind, model.name, model.color, {},
            m_project.tracks.empty() ? std::string()
                                     : m_project.tracks.back().id});
        appendCommand(batch, collab::AddClip{
            model.id, patternClip.id, patternClip.kind, patternClip.name,
            patternClip.startSeconds, patternClip.durationSeconds,
            patternClip.color, {}});
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Add Pattern");
        return result == collab::SharedMutationResult::Submitted ? model.id
                                                                 : std::string{};
    }
    return appendTrack(std::move(model));
}

std::string EngineController::addPatternClip(const std::string& patternTrackId,
                                             double startSeconds,
                                             double lengthSeconds) {
    TrackModel* pattern = m_project.findTrack(patternTrackId);
    if (!pattern || pattern->kind != TrackKind::Pattern) return {};

    ClipModel clip;
    clip.id = newUuid();
    clip.name = pattern->name;
    clip.kind = ClipKind::Pattern;
    clip.startSeconds = std::max(0.0, startSeconds);
    clip.durationSeconds = lengthSeconds > 0.0
                               ? lengthSeconds
                               : beatsToSeconds(
                                     double(std::max(1, m_project.timeSigNumerator)) *
                                         4.0 /
                                         double(std::max(
                                             1, m_project.timeSigDenominator)),
                                     m_project.tempo);
    clip.color = pattern->color;
    const auto shared = submitSharedMutation(
        collab::AddClip{patternTrackId, clip.id, clip.kind, clip.name,
                        clip.startSeconds, clip.durationSeconds, clip.color,
                        pattern->clips.empty() ? std::string()
                                               : pattern->clips.back().id},
        "Add Pattern Clip");
    if (shared == collab::SharedMutationResult::Submitted) return clip.id;
    if (shared == collab::SharedMutationResult::Blocked) return {};
    pattern->clips.push_back(clip);
    updateTimelineDuration();

    const std::string id = clip.id;
    m_undo.push(
        "Add Pattern Clip",
        [this, patternTrackId, id] { removeClip(patternTrackId, id); },
        [this, patternTrackId, clip] {
            if (TrackModel* track = m_project.findTrack(patternTrackId)) {
                track->clips.push_back(clip);
                updateTimelineDuration();
            }
        });
    return id;
}

std::string EngineController::addPatternInstrument(
    const std::string& patternId,
    const plugins::PluginDescriptor& descriptor,
    double startSeconds) {
    const TrackModel* pattern = m_project.findTrack(patternId);
    if (!pattern || pattern->kind != TrackKind::Pattern ||
        !descriptor.isInstrument) {
        return {};
    }

    if (cloudProjectBound()) {
        InsertModel instrument;
        instrument.id = newUuid();
        applyDescriptor(instrument, descriptor);
        InsertModel clean;
        if (!cleanSharedInsert(instrument, clean, false) ||
            clean.uid != "daw.sampler") {
            return {};
        }

        const double resolvedStart = std::max(0.0, startSeconds);
        double resolvedLength = beatsToSeconds(
            double(std::max(1, m_project.timeSigNumerator)), m_project.tempo);
        auto batch = std::make_shared<collab::BatchCommand>();
        std::string patternClipId;
        for (const ClipModel& candidate : pattern->clips) {
            if (candidate.kind != ClipKind::Pattern) continue;
            const double end = candidate.startSeconds +
                               candidate.durationSeconds;
            if (resolvedStart + 1e-9 < candidate.startSeconds ||
                resolvedStart >= end - 1e-9) {
                continue;
            }
            patternClipId = candidate.id;
            resolvedLength = std::max(kMinClipSeconds, end - resolvedStart);
            break;
        }
        if (patternClipId.empty()) {
            ClipModel owner;
            owner.id = newUuid();
            owner.name = pattern->name;
            owner.kind = ClipKind::Pattern;
            owner.startSeconds = resolvedStart;
            owner.durationSeconds = resolvedLength;
            owner.color = pattern->color;
            patternClipId = owner.id;
            if (!appendSharedClip(
                    batch, patternId, owner,
                    pattern->clips.empty() ? std::string()
                                           : pattern->clips.back().id)) {
                return {};
            }
        }

        TrackModel lane;
        lane.id = newUuid();
        lane.kind = TrackKind::Instrument;
        lane.name = descriptor.name.empty() ? std::string("Instrument")
                                             : descriptor.name;
        lane.color = defaultTrackColor(lane.kind);
        lane.parentId = patternId;
        lane.outputBusId = patternId;
        lane.instrument = std::move(clean);
        lane.samplerFx.ownerInstrumentId = lane.instrument.id;
        ClipModel clip;
        clip.id = newUuid();
        clip.name = lane.name;
        clip.kind = ClipKind::Midi;
        clip.patternClipId = patternClipId;
        clip.startSeconds = resolvedStart;
        clip.durationSeconds = resolvedLength;
        clip.color = lane.color;
        lane.clips.push_back(std::move(clip));

        const std::vector<std::string> descendants =
            subtreeOf(m_project, patternId);
        const std::string anchor = descendants.empty()
                                       ? patternId
                                       : descendants.back();
        if (!appendSharedTrack(batch, lane, anchor) ||
            !sharedBatchApplies(m_project, batch)) {
            return {};
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Add Pattern Instrument");
        return result == collab::SharedMutationResult::Submitted ? lane.id
                                                                 : std::string{};
    }

    const std::size_t undoStart = m_undo.depth();
    const std::string trackId = addTrack(
        TrackKind::Instrument,
        descriptor.name.empty() ? std::string("Instrument") : descriptor.name);
    // Keep the membership in the collapsed history entry. appendTrack's redo
    // owns the pristine root-level model captured at creation time; an
    // undoable move is what restores the Pattern parent after that redo.
    moveTrack(trackId, m_project.indexOf(trackId), patternId);
    if (!setTrackInstrumentPlugin(trackId, descriptor)) {
        removeTrack(trackId);
        return {};
    }
    addMidiClip(trackId, std::max(0.0, startSeconds));
    collapseUndo(undoStart, "Add Pattern Instrument");
    return trackId;
}

std::string EngineController::addPatternSample(const std::string& patternId,
                                               const std::string& filePath,
                                               double startSeconds) {
    const TrackModel* pattern = m_project.findTrack(patternId);
    if (!pattern || pattern->kind != TrackKind::Pattern || filePath.empty())
        return {};

    if (cloudProjectBound()) {
        const auto sampler =
            m_pluginManager.find(plugins::Format::Internal, "daw.sampler");
        auto request = sharedAudioRequest(filePath);
        if (!sampler || !request) return {};

        InsertModel instrument;
        instrument.id = newUuid();
        applyDescriptor(instrument, *sampler);
        InsertModel clean;
        if (!cleanSharedInsert(instrument, clean, false) ||
            clean.uid != "daw.sampler") {
            return {};
        }

        const double resolvedStart = std::max(0.0, startSeconds);
        double resolvedLength = beatsToSeconds(
            double(std::max(1, m_project.timeSigNumerator)),
            m_project.tempo);
        std::optional<ClipModel> owner;
        std::string patternClipId;
        for (const ClipModel& candidate : pattern->clips) {
            if (candidate.kind != ClipKind::Pattern) continue;
            const double end = candidate.startSeconds +
                               candidate.durationSeconds;
            if (resolvedStart + 1e-9 < candidate.startSeconds ||
                resolvedStart >= end - 1e-9) {
                continue;
            }
            patternClipId = candidate.id;
            resolvedLength = std::max(kMinClipSeconds,
                                      end - resolvedStart);
            break;
        }
        const std::string ownerAfterId = pattern->clips.empty()
            ? std::string()
            : pattern->clips.back().id;
        if (patternClipId.empty()) {
            owner.emplace();
            owner->id = newUuid();
            owner->name = pattern->name;
            owner->kind = ClipKind::Pattern;
            owner->startSeconds = resolvedStart;
            owner->durationSeconds = resolvedLength;
            owner->color = pattern->color;
            patternClipId = owner->id;
        }

        TrackModel lane;
        lane.id = newUuid();
        lane.kind = TrackKind::Instrument;
        lane.name = platform::pathToUtf8(
            platform::pathFromUtf8(filePath).stem());
        if (lane.name.empty()) lane.name = "Sample";
        lane.color = defaultTrackColor(lane.kind);
        lane.parentId = patternId;
        lane.outputBusId = patternId;
        lane.instrument = std::move(clean);
        lane.samplerFx.ownerInstrumentId = lane.instrument.id;
        ClipModel clip;
        clip.id = newUuid();
        clip.name = lane.name;
        clip.kind = ClipKind::Midi;
        clip.patternClipId = patternClipId;
        clip.startSeconds = resolvedStart;
        clip.durationSeconds = resolvedLength;
        clip.color = lane.color;
        lane.clips.push_back(std::move(clip));
        const std::string laneId = lane.id;
        const std::vector<std::string> descendants =
            subtreeOf(m_project, patternId);
        const std::string trackAnchor = descendants.empty()
            ? patternId
            : descendants.back();

        PendingSharedAssetMutation pending;
        pending.expected = expectedSharedAudioAsset(*request);
        pending.complete =
            [patternId, lane = std::move(lane), owner = std::move(owner),
             ownerAfterId, trackAnchor](
                EngineController& controller,
                const AssetRef& verifiedAsset) mutable {
                setSharedSampleBinding(lane.instrument, verifiedAsset);
                auto batch = std::make_shared<collab::BatchCommand>();
                if (owner &&
                    !appendSharedClip(batch, patternId, *owner,
                                      ownerAfterId)) {
                    return collab::SharedMutationResult::Blocked;
                }
                if (!appendSharedTrack(batch, lane, trackAnchor) ||
                    !sharedBatchApplies(controller.m_project, batch)) {
                    return collab::SharedMutationResult::Blocked;
                }
                return controller.submitSharedMutation(
                    collab::CommandBody{std::move(batch)},
                    "Add Pattern Sample");
            };
        return prepareSharedAssetMutation(std::move(*request),
                                          std::move(pending)) ==
                       collab::SharedMutationResult::Submitted
                   ? laneId
                   : std::string{};
    }

    const std::size_t undoStart = m_undo.depth();
    std::string name =
        platform::pathToUtf8(platform::pathFromUtf8(filePath).stem());
    if (name.empty()) name = "Sample";
    const std::string trackId = addTrack(TrackKind::Instrument, name);
    moveTrack(trackId, m_project.indexOf(trackId), patternId);
    if (!loadInstrumentSampler(trackId, filePath)) {
        removeTrack(trackId);
        return {};
    }
    addMidiClip(trackId, std::max(0.0, startSeconds));
    collapseUndo(undoStart, "Add Pattern Sample");
    return trackId;
}

std::string EngineController::appendTrack(TrackModel model) {
    // Complex constructors must expand their complete model into a typed batch
    // before reaching this legacy local helper.
    if (cloudProjectBound()) return {};
    const std::string id = model.id;
    m_project.tracks.push_back(model);
    rebuildGraph();

    m_undo.push("Add Track",
                [this, id] { removeTrack(id); },
                [this, model] {
                    m_project.tracks.push_back(model);
                    rebuildGraph();
                });
    return id;
}

void EngineController::removeTrack(const std::string& trackId) {
    const TrackModel* requested = m_project.findTrack(trackId);
    if (!requested) return;
    if (cloudProjectBound()) {
        std::vector<std::string> deleting;
        if (requested->kind == TrackKind::Pattern)
            deleting = subtreeOf(m_project, trackId);
        deleting.push_back(trackId);
        const std::unordered_set<std::string> deletingSet(
            deleting.begin(), deleting.end());
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const TrackModel& candidate : m_project.tracks) {
            if (deletingSet.contains(candidate.id)) continue;
            if (deletingSet.contains(candidate.outputBusId)) {
                collab::ProjectCommand child;
                child.body = collab::SetTrackOutput{candidate.id, {}};
                batch->commands.push_back(std::move(child));
            }
            if (deletingSet.contains(candidate.parentId)) {
                collab::ProjectCommand child;
                child.body = collab::SetTrackParent{candidate.id, {}};
                batch->commands.push_back(std::move(child));
            }
            for (const SendModel& send : candidate.sends) {
                if (!deletingSet.contains(send.destinationTrackId)) continue;
                collab::ProjectCommand child;
                child.body = collab::DeleteSend{candidate.id, send.id};
                batch->commands.push_back(std::move(child));
            }
        }
        for (const std::string& deletingId : deleting) {
            collab::ProjectCommand child;
            child.body = collab::DeleteTrack{deletingId};
            batch->commands.push_back(std::move(child));
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   requested->kind == TrackKind::Pattern
                                       ? "Remove Pattern"
                                       : "Remove Track");
        return;
    }
    const bool removePatternTree = requested->kind == TrackKind::Pattern;
    const std::size_t compoundUndoStart = m_undo.depth();
    if (removePatternTree) {
        // A Pattern is one musical object. Its source lanes are implementation
        // detail, so deleting the container must not detach and reveal them.
        std::vector<std::string> children = subtreeOf(m_project, trackId);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            removeTrack(*it);
    }

    const size_t index = m_project.indexOf(trackId);
    if (index == std::string::npos) return;

    struct RemovedSend {
        std::size_t index = 0;
        SendModel model;
    };
    struct RelatedTrack {
        std::string id;
        bool outputWasRemovedTrack = false;
        bool parentWasRemovedTrack = false;
        std::vector<RemovedSend> sends;
    };
    struct RemovalDelta {
        std::string trackId;
        std::size_t index = 0;
        TrackModel removed;
        std::vector<RelatedTrack> related;
    };

    auto delta = std::make_shared<RemovalDelta>();
    delta->trackId = trackId;
    delta->index = index;
    delta->removed = m_project.tracks[index];
    for (const TrackModel& candidate : m_project.tracks) {
        if (candidate.id == trackId) continue;
        RelatedTrack related;
        related.id = candidate.id;
        related.outputWasRemovedTrack = candidate.outputBusId == trackId;
        related.parentWasRemovedTrack = candidate.parentId == trackId;
        for (std::size_t send = 0; send < candidate.sends.size(); ++send) {
            if (candidate.sends[send].destinationTrackId == trackId) {
                related.sends.push_back({send, candidate.sends[send]});
            }
        }
        if (related.outputWasRemovedTrack || related.parentWasRemovedTrack ||
            !related.sends.empty()) {
            delta->related.push_back(std::move(related));
        }
    }

    for (const ClipModel& clip : delta->removed.clips)
        m_clipSampleCache.erase(clip.id);
    m_project.tracks.erase(m_project.tracks.begin() + std::ptrdiff_t(index));
    m_midiNotesRevisions.erase(trackId);

    // Anything that fed or was fed by the track has to lose those edges.
    for (auto& t : m_project.tracks) {
        if (t.outputBusId == trackId) t.outputBusId.clear();
        std::erase_if(t.sends, [&](const SendModel& s) {
            return s.destinationTrackId == trackId;
        });
        if (t.parentId == trackId) t.parentId.clear();
    }
    rebuildGraph();
    pruneDecodedSampleCache();

    auto apply = [this, delta](bool remove) {
        if (remove) {
            if (const std::size_t at = m_project.indexOf(delta->trackId);
                at != std::string::npos) {
                for (const ClipModel& clip : m_project.tracks[at].clips)
                    m_clipSampleCache.erase(clip.id);
                m_project.tracks.erase(
                    m_project.tracks.begin() + std::ptrdiff_t(at));
            }
            m_midiNotesRevisions.erase(delta->trackId);
            for (const RelatedTrack& related : delta->related) {
                TrackModel* target = m_project.findTrack(related.id);
                if (!target) continue;
                if (related.outputWasRemovedTrack &&
                    target->outputBusId == delta->trackId) {
                    target->outputBusId.clear();
                }
                if (related.parentWasRemovedTrack &&
                    target->parentId == delta->trackId) {
                    target->parentId.clear();
                }
                std::erase_if(target->sends, [&](const SendModel& send) {
                    return send.destinationTrackId == delta->trackId;
                });
            }
        } else {
            if (!m_project.findTrack(delta->trackId)) {
                const std::size_t at =
                    std::min(delta->index, m_project.tracks.size());
                m_project.tracks.insert(
                    m_project.tracks.begin() + std::ptrdiff_t(at),
                    delta->removed);
            }
            for (const RelatedTrack& related : delta->related) {
                TrackModel* target = m_project.findTrack(related.id);
                if (!target) continue;
                if (related.outputWasRemovedTrack)
                    target->outputBusId = delta->trackId;
                if (related.parentWasRemovedTrack)
                    target->parentId = delta->trackId;
                for (const RemovedSend& removed : related.sends) {
                    const bool present = std::any_of(
                        target->sends.begin(), target->sends.end(),
                        [&](const SendModel& send) {
                            return !removed.model.id.empty() &&
                                   send.id == removed.model.id;
                        });
                    if (present) continue;
                    const std::size_t at =
                        std::min(removed.index, target->sends.size());
                    target->sends.insert(
                        target->sends.begin() + std::ptrdiff_t(at),
                        removed.model);
                }
            }
        }
        m_deferredClipSync.clear();
        rebuildGraph();
        pruneDecodedSampleCache();
    };
    // Removing a folder/Pattern also rewrites its children's parent and bus,
    // and removing any track clears incoming sends. Restoring only the erased
    // TrackModel left those related tracks permanently detached after undo.
    m_undo.push("Remove Track", [apply] { apply(false); },
                [apply] { apply(true); });
    if (removePatternTree)
        collapseUndo(compoundUndoStart, "Remove Pattern");
}

collab::SharedMutationResult EngineController::renameTrack(
    const std::string& trackId, const std::string& name) {
    auto* track = m_project.findTrack(trackId);
    if (!track || track->name == name)
        return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result = m_sharedMutationSink->renameTrack(trackId, name);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    // A LocalFallback sink is synchronous but may still have inspected other
    // controller state.  Reacquire the vector-backed pointer at the mutation
    // boundary instead of retaining it across external code.
    track = m_project.findTrack(trackId);
    if (!track || track->name == name)
        return collab::SharedMutationResult::LocalFallback;
    const std::string previous = track->name;
    track->name = name;

    // Undoable like every other track property. Mute and solo deliberately are
    // not — they are performance controls, and a stack full of them would bury
    // the edits the user actually wants back.
    m_undo.push("Rename Track",
                [this, trackId, previous] { renameTrack(trackId, previous); },
                [this, trackId, name] { renameTrack(trackId, name); });
    return collab::SharedMutationResult::LocalFallback;
}

void EngineController::setTrackVolume(const std::string& trackId, float volume) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    const float previous = track->volume;
    const float requested = std::clamp(volume, 0.0f, 2.0f);
    if (requested == previous) return;
    const auto shared = submitSharedMutation(
        collab::SetTrackProperty{trackId, collab::TrackProperty::Volume,
                                 double(requested)},
        "Set Volume");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    setTrackVolumeLive(trackId, volume);
    const float applied = track->volume;
    if (applied == previous) return;

    m_undo.push("Set Volume",
                [this, trackId, previous] {
                    setTrackVolumeLive(trackId, previous);
                },
                [this, trackId, applied] {
                    setTrackVolumeLive(trackId, applied);
                });
}

void EngineController::setTrackPan(const std::string& trackId, float pan) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    const float previous = track->pan;
    const float requested = std::clamp(pan, -1.0f, 1.0f);
    if (requested == previous) return;
    const auto shared = submitSharedMutation(
        collab::SetTrackProperty{trackId, collab::TrackProperty::Pan,
                                 double(requested)},
        "Set Pan");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    setTrackPanLive(trackId, pan);
    const float applied = track->pan;
    if (applied == previous) return;

    m_undo.push("Set Pan",
                [this, trackId, previous] { setTrackPanLive(trackId, previous); },
                [this, trackId, applied] { setTrackPanLive(trackId, applied); });
}

void EngineController::setTrackVolumeLive(const std::string& trackId,
                                          float volume) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    const float applied = std::clamp(volume, 0.0f, 2.0f);
    if (track->volume == applied) return;
    track->volume = applied;
    AutomationTarget target;
    target.kind = AutomationTargetKind::TrackVolume;
    target.channelId = trackId;
    followPassiveAutomation(target, normalizedFromGain(applied));
    syncTrackGain(*track);
}

void EngineController::setTrackPanLive(const std::string& trackId, float pan) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    const float applied = std::clamp(pan, -1.0f, 1.0f);
    if (track->pan == applied) return;
    track->pan = applied;
    AutomationTarget target;
    target.kind = AutomationTargetKind::TrackPan;
    target.channelId = trackId;
    followPassiveAutomation(target, plainToAutomation(target, applied));
    syncTrackGain(*track);
}

void EngineController::commitTrackVolumeEdit(
    const std::vector<std::pair<std::string, float>>& before,
    const std::string& label) {
    std::vector<std::pair<std::string, float>> from;
    std::vector<std::pair<std::string, float>> to;
    for (const auto& [trackId, value] : before) {
        const TrackModel* track = m_project.findTrack(trackId);
        if (!track || track->volume == value) continue;
        from.emplace_back(trackId, value);
        to.emplace_back(trackId, track->volume);
    }
    if (from.empty()) return;
    const auto apply = [this](const auto& values) {
        for (const auto& [trackId, value] : values)
            setTrackVolumeLive(trackId, value);
    };
    auto batch = std::make_shared<collab::BatchCommand>();
    batch->commands.reserve(to.size());
    for (const auto& [trackId, value] : to) {
        collab::ProjectCommand child;
        child.body = collab::SetTrackProperty{
            trackId, collab::TrackProperty::Volume, double(value)};
        batch->commands.push_back(std::move(child));
    }
    const auto shared = submitSharedMutation(
        collab::CommandBody{std::move(batch)}, label);
    if (shared == collab::SharedMutationResult::Blocked) {
        apply(from);
        return;
    }
    if (shared == collab::SharedMutationResult::Submitted) return;
    m_undo.push(label, [apply, from] { apply(from); },
                [apply, to] { apply(to); });
}

void EngineController::commitTrackPanEdit(
    const std::vector<std::pair<std::string, float>>& before,
    const std::string& label) {
    std::vector<std::pair<std::string, float>> from;
    std::vector<std::pair<std::string, float>> to;
    for (const auto& [trackId, value] : before) {
        const TrackModel* track = m_project.findTrack(trackId);
        if (!track || track->pan == value) continue;
        from.emplace_back(trackId, value);
        to.emplace_back(trackId, track->pan);
    }
    if (from.empty()) return;
    const auto apply = [this](const auto& values) {
        for (const auto& [trackId, value] : values)
            setTrackPanLive(trackId, value);
    };
    auto batch = std::make_shared<collab::BatchCommand>();
    batch->commands.reserve(to.size());
    for (const auto& [trackId, value] : to) {
        collab::ProjectCommand child;
        child.body = collab::SetTrackProperty{
            trackId, collab::TrackProperty::Pan, double(value)};
        batch->commands.push_back(std::move(child));
    }
    const auto shared = submitSharedMutation(
        collab::CommandBody{std::move(batch)}, label);
    if (shared == collab::SharedMutationResult::Blocked) {
        apply(from);
        return;
    }
    if (shared == collab::SharedMutationResult::Submitted) return;
    m_undo.push(label, [apply, from] { apply(from); },
                [apply, to] { apply(to); });
}

void EngineController::setTrackMono(const std::string& trackId, bool mono) {
    auto* track = m_project.findTrack(trackId);
    if (!track || track->mono == mono) return;
    const auto shared = submitSharedMutation(
        collab::SetTrackProperty{trackId, collab::TrackProperty::Mono, mono},
        mono ? "Track Mono" : "Track Stereo");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    const bool previous = track->mono;
    track->mono = mono;
    // The fader fold and every plugin's main-bus arrangement have to change as
    // one operation. `true` gates the audio thread while invalidated plugin
    // nodes deactivate, negotiate mono/stereo and reactivate.
    bool hasLivePlugins = false;
    if (const auto found = m_channels.find(trackId); found != m_channels.end()) {
        const auto anyLoaded = [](const std::vector<InsertSlot>& slots) {
            return std::any_of(slots.begin(), slots.end(),
                               [](const InsertSlot& slot) { return bool(slot.node); });
        };
        const TrackChannel& channel = found->second;
        hasLivePlugins = anyLoaded(channel.instrument) ||
                         anyLoaded(channel.samplerInserts) ||
                         anyLoaded(channel.inserts);
        if (!hasLivePlugins) {
            for (const auto& [clipId, clipFx] : channel.clipFx) {
                (void)clipId;
                if (anyLoaded(clipFx.inserts)) {
                    hasLivePlugins = true;
                    break;
                }
            }
        }
    }
    if (hasLivePlugins) {
        rebuildGraph(/*reconfigurePlugins=*/true);
    } else {
        // Preserve the old lock-free, gapless toggle when there is no plugin
        // bus to renegotiate.
        syncTrackGain(*track);
    }

    m_undo.push(mono ? "Track Mono" : "Track Stereo",
                [this, trackId, previous] { setTrackMono(trackId, previous); },
                [this, trackId, mono] { setTrackMono(trackId, mono); });
}

collab::SharedMutationResult EngineController::setTrackMuted(
    const std::string& trackId, bool muted) {
    const TrackModel* current = m_project.findTrack(trackId);
    if (!current || current->muted == muted)
        return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result = m_sharedMutationSink->setTrackMuted(trackId, muted);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    auto* track = m_project.findTrack(trackId);
    if (!track) return collab::SharedMutationResult::LocalFallback;
    track->muted = muted;
    syncTrackGain(*track);
    return collab::SharedMutationResult::LocalFallback;
}

collab::SharedMutationResult EngineController::setTracksMuted(
    std::span<const std::string> trackIds, bool muted) {
    std::vector<std::string> targets;
    std::unordered_set<std::string> seen;
    const auto appendIfChanged = [&](const std::string& id) {
        const TrackModel* track = m_project.findTrack(id);
        if (!track || track->muted == muted || !seen.insert(id).second) return;
        targets.push_back(id);
    };

    for (const std::string& requestedId : trackIds) {
        const TrackModel* requested = m_project.findTrack(requestedId);
        if (!requested) continue;
        const std::string rootId = isAutomationLane(*requested)
                                       ? requested->parentId
                                       : requested->id;
        const TrackModel* root = m_project.findTrack(rootId);
        if (!root) continue;
        appendIfChanged(rootId);
        if (isFolder(*root) && !carriesAudio(*root)) {
            for (const std::string& childId : subtreeOf(m_project, rootId))
                appendIfChanged(childId);
        }
    }

    if (targets.empty()) return collab::SharedMutationResult::LocalFallback;
    if (m_sharedMutationSink) {
        const auto result = m_sharedMutationSink->setTracksMuted(targets, muted);
        if (result != collab::SharedMutationResult::LocalFallback)
            return result;
    }
    for (const std::string& id : targets) {
        if (TrackModel* track = m_project.findTrack(id)) track->muted = muted;
    }
    syncAllTrackGains();
    return collab::SharedMutationResult::LocalFallback;
}

void EngineController::setTrackSoloed(const std::string& trackId, bool soloed) {
    if (auto* t = m_project.findTrack(trackId)) {
        t->soloed = soloed;
        syncAllTrackGains();   // solo changes every other channel's gain
    }
}

void EngineController::setTrackArmed(const std::string& trackId, bool armed) {
    auto* track = m_project.findTrack(trackId);
    if (!track || track->armed == armed) return;
    track->armed = armed;
    rebuildGraph();            // an armed track grows an input node
}

void EngineController::setTrackMonitor(const std::string& trackId, bool monitor) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    // A deliberate click outranks smart monitoring: from here on this track's
    // monitor is the user's, and the "A" mark goes away.
    if (m_recording.manualMonitorDisablesAuto) track->monitorAuto = false;
    if (track->monitor == monitor) return;
    track->monitor = monitor;
    rebuildGraph();
}

double EngineController::recordingStartSeconds(const std::string& trackId) const {
    for (const auto& cap : m_captures) {
        if (cap.trackId == trackId) return cap.startSeconds;
    }
    return -1.0;
}

bool EngineController::isInputMonitoringActive() const {
    for (const auto& t : m_project.tracks) {
        if (t.monitor) return true;
    }
    return false;
}

float EngineController::inputPeak(uint32_t channel) const {
    return m_devices->diagInputPeak(audio::ChannelCount(channel));
}

void EngineController::setTrackColor(const std::string& trackId, uint32_t color) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    if (cloudProjectBound()) {
        std::vector<std::string> affected{trackId};
        for (const TrackModel& child : m_project.tracks) {
            if (isAutomationLane(child) && child.parentId == trackId)
                affected.push_back(child.id);
        }
        if (isFolder(*track)) {
            for (const std::string& childId : subtreeOf(m_project, trackId)) {
                if (std::find(affected.begin(), affected.end(), childId) ==
                    affected.end()) {
                    affected.push_back(childId);
                }
            }
        }
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const std::string& id : affected) {
            const TrackModel* affectedTrack = m_project.findTrack(id);
            if (!affectedTrack) continue;
            appendCommand(batch, collab::SetTrackProperty{
                id, collab::TrackProperty::Color, std::int64_t(color)});
            for (const ClipModel& clip : affectedTrack->clips) {
                appendCommand(batch, collab::SetClipProperty{
                    id, clip.id, collab::ClipProperty::Color,
                    std::int64_t(color)});
            }
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       "Set Track Color");
        }
        return;
    }
    const auto paint = [color](TrackModel& t) {
        t.color = color;
        // A clip's colour is its track's colour — clips get one at import and
        // have no way to be given their own. Recolouring the track has to take
        // the clips already on it with it, or the lane ends up striped in
        // whatever colours it happened to have when each clip landed.
        for (auto& clip : t.clips) clip.color = color;
    };
    paint(*track);
    // An automation lane is a visual child of the channel it describes. It
    // never owns an independent colour: parent recolouring updates existing
    // lanes and all their clips in the same operation.
    for (TrackModel& child : m_project.tracks) {
        if (isAutomationLane(child) && child.parentId == trackId) paint(child);
    }
    // A folder is how a group of tracks is named, seen and reasoned about, so
    // its colour is the group's colour: everything filed inside it follows,
    // however deep. Both kinds of folder — summing changes what a folder does
    // to the sound, not what it does to the eye.
    if (!isFolder(*track)) return;
    for (const std::string& childId : subtreeOf(m_project, trackId)) {
        if (auto* child = m_project.findTrack(childId)) paint(*child);
    }
}

void EngineController::setTrackHeight(const std::string& trackId, double height) {
    if (auto* t = m_project.findTrack(trackId))
        t->height = std::clamp(height, 30.0, 400.0);
}

void EngineController::commitTrackHeightEdit(
    const std::vector<std::pair<std::string, double>>& before,
    const std::string& label) {
    std::vector<std::pair<std::string, double>> from;
    std::vector<std::pair<std::string, double>> to;
    for (const auto& [trackId, value] : before) {
        const TrackModel* track = m_project.findTrack(trackId);
        if (!track || track->height == value) continue;
        from.emplace_back(trackId, value);
        to.emplace_back(trackId, track->height);
    }
    if (from.empty()) return;
    const auto apply = [this](const auto& values) {
        for (const auto& [trackId, value] : values)
            setTrackHeight(trackId, value);
    };
    m_undo.push(label, [apply, from] { apply(from); },
                [apply, to] { apply(to); });
}

std::string EngineController::duplicateTrack(const std::string& trackId,
                                             bool withInserts) {
    const size_t index = m_project.indexOf(trackId);
    if (index == std::string::npos) return {};

    if (cloudProjectBound()) {
        TrackModel copy = mintTrackCopy(m_project.tracks[index], withInserts);
        copy.name += " copy";
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedTrack(batch, copy, trackId) ||
            !sharedBatchApplies(m_project, batch)) {
            return {};
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Duplicate Track");
        return result == collab::SharedMutationResult::Submitted ? copy.id
                                                                 : std::string{};
    }

    TrackModel copy = m_project.tracks[index];
    copy.id = newUuid();
    copy.name = copy.name + " copy";
    // A copied source stays inside the same Pattern/folder. Apart from being
    // what Duplicate normally means, this also keeps its summing route intact.
    copy.parentId = m_project.tracks[index].parentId;
    copy.soloed = false;
    std::string samplerPath;
    if (copy.instrument.uid == "daw.sampler") {
        if (auto* sampler = samplerInstance(trackId, copy.instrument.id))
            samplerPath = sampler->samplePath();
    }
    // New clip ids so the two tracks' clips stay independent — and new note ids
    // with them, or an edit in one track's piano roll would find the other's.
    for (auto& c : copy.clips) {
        c.id = newUuid();
        for (auto& n : c.notes) n.id = newUuid();
    }
    // A duplicate's sends target the same buses but need fresh ids.
    for (auto& s : copy.sends) s.id = newUuid();

    std::unordered_map<std::string, std::string> duplicatedSlotIds;
    if (copy.instrument.isLoaded()) {
        const std::string previous = copy.instrument.id;
        copy.instrument.id = newUuid();
        duplicatedSlotIds[previous] = copy.instrument.id;
        if (copy.samplerFx.ownerInstrumentId == previous) {
            copy.samplerFx.ownerInstrumentId = copy.instrument.id;
        }
    }
    if (withInserts) {
        for (auto& ins : copy.inserts) {
            const std::string previous = ins.id;
            ins.id = newUuid();
            duplicatedSlotIds[previous] = ins.id;
        }
        for (auto& ins : copy.samplerFx.inserts) {
            const std::string previous = ins.id;
            ins.id = newUuid();
            duplicatedSlotIds[previous] = ins.id;
        }
        for (ClipModel& clip : copy.clips) {
            for (InsertModel& ins : clip.inserts) {
                const std::string previous = ins.id;
                ins.id = newUuid();
                duplicatedSlotIds[previous] = ins.id;
            }
        }
    } else {
        copy.inserts.clear();
        copy.samplerFx.inserts.clear();
        for (ClipModel& clip : copy.clips) clip.inserts.clear();
    }
    // Plugin automation is channel-local. A copied lane has to address the
    // copied slot, including a sampler-owned slot, rather than the original
    // track's identically configured plugin.
    for (ClipModel& clip : copy.clips) {
        for (ControllerLane& lane : clip.lanes) {
            if (const auto mapped = duplicatedSlotIds.find(lane.slotId);
                mapped != duplicatedSlotIds.end()) {
                lane.slotId = mapped->second;
            }
        }
    }

    const std::string newId = copy.id;
    m_project.tracks.insert(m_project.tracks.begin() +
                                std::ptrdiff_t(index + 1),
                            copy);
    rebuildGraph();
    if (!samplerPath.empty()) {
        loadSamplerSampleSilently(newId, copy.instrument.id, samplerPath);
    }

    m_undo.push("Duplicate Track",
                [this, newId] { removeTrack(newId); },
                [this, copy, index, samplerPath] {
                    const size_t at =
                        std::min(index + 1, m_project.tracks.size());
                    m_project.tracks.insert(
                        m_project.tracks.begin() + std::ptrdiff_t(at), copy);
                    rebuildGraph();
                    if (!samplerPath.empty()) {
                        loadSamplerSampleSilently(copy.id, copy.instrument.id,
                                                  samplerPath);
                    }
                });
    return newId;
}

std::string EngineController::duplicatePattern(const std::string& patternId) {
    const TrackModel* pattern = m_project.findTrack(patternId);
    if (!pattern || pattern->kind != TrackKind::Pattern) return {};

    if (cloudProjectBound()) {
        TrackModel patternCopy = mintTrackCopy(*pattern, true);
        patternCopy.name += " copy";
        std::unordered_map<std::string, std::string> patternClipIds;
        for (std::size_t index = 0;
             index < pattern->clips.size() &&
             index < patternCopy.clips.size();
             ++index) {
            if (pattern->clips[index].kind == ClipKind::Pattern)
                patternClipIds[pattern->clips[index].id] =
                    patternCopy.clips[index].id;
        }

        std::vector<TrackModel> children;
        for (const TrackModel& source : m_project.tracks) {
            if (source.parentId != patternId) continue;
            TrackModel child = mintTrackCopy(source, true);
            child.name = source.name;
            child.parentId = patternCopy.id;
            if (child.outputBusId == patternId)
                child.outputBusId = patternCopy.id;
            for (ClipModel& clip : child.clips) {
                if (const auto found = patternClipIds.find(clip.patternClipId);
                    found != patternClipIds.end()) {
                    clip.patternClipId = found->second;
                }
            }
            children.push_back(std::move(child));
        }

        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedTrack(batch, patternCopy, patternId)) return {};
        std::string anchor = patternCopy.id;
        for (const TrackModel& child : children) {
            if (!appendSharedTrack(batch, child, anchor)) return {};
            anchor = child.id;
        }
        if (!sharedBatchApplies(m_project, batch)) return {};
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Duplicate Pattern");
        return result == collab::SharedMutationResult::Submitted
                   ? patternCopy.id
                   : std::string{};
    }

    std::vector<std::string> originalPatternClips;
    for (const ClipModel& clip : pattern->clips) {
        if (clip.kind == ClipKind::Pattern)
            originalPatternClips.push_back(clip.id);
    }

    std::vector<std::string> sources;
    for (const TrackModel& track : m_project.tracks) {
        if (track.parentId == patternId) sources.push_back(track.id);
    }

    const std::size_t undoStart = m_undo.depth();
    const std::string copyId = duplicateTrack(patternId, /*withInserts=*/true);
    if (copyId.empty()) return {};

    std::unordered_map<std::string, std::string> patternClipMap;
    if (const TrackModel* patternCopy = m_project.findTrack(copyId)) {
        std::size_t index = 0;
        for (const ClipModel& clip : patternCopy->clips) {
            if (clip.kind != ClipKind::Pattern ||
                index >= originalPatternClips.size()) {
                continue;
            }
            patternClipMap[originalPatternClips[index++]] = clip.id;
        }
    }

    size_t slot = m_project.indexOf(copyId) + 1;
    for (const std::string& sourceId : sources) {
        const TrackModel* source = m_project.findTrack(sourceId);
        if (!source) continue;
        const std::string sourceName = source->name;
        const std::string sourceCopy =
            duplicateTrack(sourceId, /*withInserts=*/true);
        if (sourceCopy.empty()) continue;
        if (TrackModel* copiedSource = m_project.findTrack(sourceCopy)) {
            for (ClipModel& clip : copiedSource->clips) {
                const auto mapped = patternClipMap.find(clip.patternClipId);
                if (mapped == patternClipMap.end()) continue;
                const std::string beforeOwner = clip.patternClipId;
                const std::string afterOwner = mapped->second;
                const std::string clipId = clip.id;
                clip.patternClipId = afterOwner;
                auto setOwner = [this, sourceCopy,
                                 clipId](const std::string& owner) {
                    if (TrackModel* target = m_project.findTrack(sourceCopy)) {
                        for (ClipModel& candidate : target->clips) {
                            if (candidate.id == clipId) {
                                candidate.patternClipId = owner;
                                if (trackAccepts(target->kind, ClipKind::Midi))
                                    syncTrackNotes(*target);
                                return;
                            }
                        }
                    }
                };
                m_undo.push("Relink Pattern Clip",
                            [setOwner, beforeOwner] { setOwner(beforeOwner); },
                            [setOwner, afterOwner] { setOwner(afterOwner); });
            }
        }
        moveTrack(sourceCopy, slot, copyId);
        // Source names describe instruments/files inside the Pattern. The
        // Pattern itself gets the "copy" suffix; its internal labels do not.
        renameTrack(sourceCopy, sourceName);
        slot = m_project.indexOf(sourceCopy) + 1;
    }
    collapseUndo(undoStart, "Duplicate Pattern");
    return copyId;
}

void EngineController::setTrackInputChannel(const std::string& trackId,
                                            uint32_t channel) {
    auto* track = m_project.findTrack(trackId);
    if (!track || track->inputChannel == channel) return;
    track->inputChannel = channel;
    // A different input is a different signal, so whatever smart monitoring
    // decided about the old one no longer holds — including mid-recording.
    if (track->monitorAuto) applySmartMonitoring(*track);
    retargetCaptureInput(*track);
    if (track->monitor || track->armed) rebuildGraph();
}

void EngineController::setTrackInputChannelCount(const std::string& trackId,
                                                 uint32_t count) {
    auto* track = m_project.findTrack(trackId);
    count = std::clamp(count, 1u, 2u);
    if (!track || track->inputChannelCount == count) return;
    track->inputChannelCount = count;
    retargetCaptureInput(*track);
    if (track->monitor || track->armed) rebuildGraph();
}

void EngineController::retargetCaptureInput(const TrackModel& track) {
    for (auto& capture : m_captures) {
        if (capture.trackId != track.id || !capture.recorder) continue;
        capture.recorder->setInputChannels(
            audio::ChannelCount(track.inputChannel),
            audio::ChannelCount(track.inputChannelCount));
    }
}

void EngineController::moveTrackToFolder(const std::string& trackId,
                                         const std::string& parentId) {
    if (isDescendantOf(m_project, parentId, trackId)) return;
    TrackModel* moving = m_project.findTrack(trackId);
    if (!moving || moving->parentId == parentId) return;
    if (cloudProjectBound()) {
        ProjectModel scratch = m_project;
        TrackModel* candidate = scratch.findTrack(trackId);
        if (!candidate) return;
        candidate->parentId = parentId;
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetTrackParent{trackId, parentId});
        for (const TrackModel& before : m_project.tracks) {
            if (!carriesAudio(before)) continue;
            const TrackModel* current = before.outputBusId.empty()
                                            ? nullptr
                                            : m_project.findTrack(before.outputBusId);
            if (!before.outputBusId.empty() &&
                !(current && isFolder(*current))) {
                continue;
            }
            const std::string desired = summingParent(scratch, before.id);
            if (desired != before.outputBusId)
                appendCommand(batch, collab::SetTrackOutput{before.id, desired});
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Move Track to Folder");
        return;
    }
    if (auto* d = m_project.findTrack(trackId)) d->parentId = parentId;
    syncFolderRouting();
}

// ── Order and folders ──────────────────────────────────────────────────────

bool EngineController::moveTrack(const std::string& trackId, size_t targetIndex,
                                 const std::string& newParentId) {
    const size_t from = m_project.indexOf(trackId);
    if (from == std::string::npos) return false;
    if (!newParentId.empty() &&
        (newParentId == trackId ||
         isDescendantOf(m_project, newParentId, trackId))) {
        return false;
    }
    if (!newParentId.empty() && !m_project.findTrack(newParentId)) return false;

    if (cloudProjectBound()) {
        const std::vector<std::string> childIds = subtreeOf(m_project, trackId);
        std::vector<std::string> blockIds{trackId};
        blockIds.insert(blockIds.end(), childIds.begin(), childIds.end());
        std::vector<std::string> remaining;
        remaining.reserve(m_project.tracks.size() - blockIds.size());
        std::size_t removedBefore = 0;
        for (std::size_t index = 0; index < m_project.tracks.size(); ++index) {
            const std::string& id = m_project.tracks[index].id;
            if (std::find(blockIds.begin(), blockIds.end(), id) !=
                blockIds.end()) {
                if (index < targetIndex) ++removedBefore;
            } else {
                remaining.push_back(id);
            }
        }
        const std::size_t insertAt = std::min(
            targetIndex >= removedBefore ? targetIndex - removedBefore : 0,
            remaining.size());
        std::string anchor = insertAt == 0 ? std::string()
                                           : remaining[insertAt - 1];
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const std::string& id : blockIds) {
            appendCommand(batch, collab::MoveTrack{id, anchor});
            anchor = id;
        }
        appendCommand(batch, collab::SetTrackParent{trackId, newParentId});

        ProjectModel scratch = m_project;
        if (TrackModel* candidate = scratch.findTrack(trackId))
            candidate->parentId = newParentId;
        for (const TrackModel& before : m_project.tracks) {
            if (!carriesAudio(before)) continue;
            const TrackModel* current = before.outputBusId.empty()
                                            ? nullptr
                                            : m_project.findTrack(before.outputBusId);
            if (!before.outputBusId.empty() &&
                !(current && isFolder(*current))) {
                continue;
            }
            const std::string desired = summingParent(scratch, before.id);
            if (desired != before.outputBusId)
                appendCommand(batch, collab::SetTrackOutput{before.id, desired});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Move Track");
        return result == collab::SharedMutationResult::Submitted;
    }

    const std::vector<std::string> childIds = subtreeOf(m_project, trackId);
    std::vector<TrackModel> block;
    block.reserve(childIds.size() + 1);
    block.push_back(m_project.tracks[from]);
    for (const auto& childId : childIds) {
        const size_t index = m_project.indexOf(childId);
        if (index != std::string::npos) block.push_back(m_project.tracks[index]);
    }

    const std::string previousParent = block.front().parentId;
    block.front().parentId = newParentId;

    std::vector<std::string> blockIds;
    blockIds.reserve(block.size());
    for (const auto& t : block) blockIds.push_back(t.id);

    size_t removedBefore = 0;
    for (const auto& id : blockIds) {
        const size_t index = m_project.indexOf(id);
        if (index != std::string::npos && index < targetIndex) ++removedBefore;
    }

    std::erase_if(m_project.tracks, [&](const TrackModel& t) {
        return std::find(blockIds.begin(), blockIds.end(), t.id) != blockIds.end();
    });

    size_t insertAt = targetIndex >= removedBefore ? targetIndex - removedBefore : 0;
    insertAt = std::min(insertAt, m_project.tracks.size());
    m_project.tracks.insert(m_project.tracks.begin() + std::ptrdiff_t(insertAt),
                            std::make_move_iterator(block.begin()),
                            std::make_move_iterator(block.end()));

    // Dropping a track into (or out of) a summing folder is the one gesture
    // that routes it — nobody opens the channel strip to wire up a group.
    syncFolderRouting();

    m_undo.push("Move Track",
                [this, trackId, from, previousParent] {
                    moveTrack(trackId, from, previousParent);
                },
                [this, trackId, targetIndex, newParentId] {
                    moveTrack(trackId, targetIndex, newParentId);
                });
    return true;
}

void EngineController::setFolderExpanded(const std::string& folderId,
                                         bool expanded) {
    if (auto* folder = m_project.findTrack(folderId)) folder->expanded = expanded;
}

void EngineController::setAutomationExpanded(const std::string& trackId,
                                             bool expanded) {
    if (auto* track = m_project.findTrack(trackId))
        track->automationExpanded = expanded;
}

void EngineController::syncFolderRouting() {
    bool changed = false;
    for (auto& track : m_project.tracks) {
        if (!carriesAudio(track)) continue;
        const std::string want = summingParent(m_project, track.id);
        if (track.outputBusId == want) continue;

        // Whose routing is this? A route pointing at a folder was made by the
        // folders and may be rewritten; anything else the user chose in the
        // channel strip, and moving the track between folders must not undo
        // that choice. "Points at a folder" rather than "points at a *summing*
        // folder" on purpose — turning summing off is exactly the case where
        // the destination has stopped being one.
        const TrackModel* current = track.outputBusId.empty()
                                        ? nullptr
                                        : m_project.findTrack(track.outputBusId);
        if (!track.outputBusId.empty() && !(current && isFolder(*current))) {
            continue;
        }
        track.outputBusId = want;
        changed = true;
    }
    if (changed) rebuildGraph();
}

void EngineController::setFolderSumming(const std::string& folderId,
                                        bool summing) {
    auto* folder = m_project.findTrack(folderId);
    if (!folder || !isFolder(*folder) || folder->summing == summing) return;
    if (cloudProjectBound()) {
        ProjectModel scratch = m_project;
        TrackModel* candidate = scratch.findTrack(folderId);
        if (!candidate) return;
        candidate->summing = summing;
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetTrackProperty{
            folderId, collab::TrackProperty::Summing, summing});
        for (const TrackModel& before : m_project.tracks) {
            if (!carriesAudio(before)) continue;
            const TrackModel* current = before.outputBusId.empty()
                                            ? nullptr
                                            : m_project.findTrack(before.outputBusId);
            if (!before.outputBusId.empty() &&
                !(current && isFolder(*current))) {
                continue;
            }
            const std::string desired = summingParent(scratch, before.id);
            if (desired != before.outputBusId)
                appendCommand(batch, collab::SetTrackOutput{before.id, desired});
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Folder Summing");
        return;
    }
    folder->summing = summing;
    // Its children follow it: into the new bus, or back out to whatever
    // encloses the folder now that there is nothing here to sum into.
    syncFolderRouting();
    // Unconditional — an empty folder changes no child routing but still gains
    // or loses a channel strip of its own.
    rebuildGraph();

    m_undo.push("Folder Summing",
                [this, folderId, summing] { setFolderSumming(folderId, !summing); },
                [this, folderId, summing] { setFolderSumming(folderId, summing); });
}

std::string EngineController::packIntoFolder(
    const std::vector<std::string>& trackIds, const std::string& name,
    bool summing) {
    if (trackIds.empty()) return {};

    size_t firstIndex = m_project.tracks.size();
    for (const auto& id : trackIds) {
        const size_t index = m_project.indexOf(id);
        if (index != std::string::npos) firstIndex = std::min(firstIndex, index);
    }
    if (firstIndex >= m_project.tracks.size()) return {};

    struct Origin { std::string id; size_t index; std::string parentId; };
    std::vector<Origin> origins;
    for (const auto& id : trackIds) {
        const size_t index = m_project.indexOf(id);
        if (index == std::string::npos) continue;
        origins.push_back({id, index, m_project.tracks[index].parentId});
    }
    if (origins.empty()) return {};

    const std::string parentId = m_project.tracks[firstIndex].parentId;
    if (cloudProjectBound()) {
        std::unordered_set<std::string> requested;
        for (const Origin& origin : origins) requested.insert(origin.id);
        std::vector<std::string> roots;
        for (const Origin& origin : origins) {
            bool nested = false;
            const TrackModel* current = m_project.findTrack(origin.id);
            for (std::string cursor = current ? current->parentId : std::string();
                 !cursor.empty();) {
                if (requested.contains(cursor)) {
                    nested = true;
                    break;
                }
                const TrackModel* ancestor = m_project.findTrack(cursor);
                cursor = ancestor ? ancestor->parentId : std::string();
            }
            if (!nested &&
                std::find(roots.begin(), roots.end(), origin.id) == roots.end()) {
                roots.push_back(origin.id);
            }
        }
        if (roots.empty()) return {};

        TrackModel folder;
        folder.id = newUuid();
        folder.kind = TrackKind::Folder;
        folder.summing = summing;
        folder.name = !name.empty() ? name : (summing ? "Group" : "Folder");
        folder.color = defaultTrackColor(summing ? TrackKind::Group
                                                 : TrackKind::Folder);
        folder.parentId = parentId;
        auto batch = std::make_shared<collab::BatchCommand>();
        const std::string beforeFirst = firstIndex == 0
                                            ? std::string()
                                            : m_project.tracks[firstIndex - 1].id;
        appendCommand(batch, collab::AddTrack{
            folder.id, folder.kind, folder.name, folder.color,
            folder.parentId, beforeFirst});
        if (summing) {
            appendCommand(batch, collab::SetTrackProperty{
                folder.id, collab::TrackProperty::Summing, true});
        }

        std::string anchor = folder.id;
        for (const std::string& root : roots) {
            std::vector<std::string> block{root};
            const auto children = subtreeOf(m_project, root);
            block.insert(block.end(), children.begin(), children.end());
            for (const std::string& id : block) {
                appendCommand(batch, collab::MoveTrack{id, anchor});
                anchor = id;
            }
            appendCommand(batch,
                          collab::SetTrackParent{root, folder.id});
        }

        ProjectModel planned = m_project;
        planned.tracks.push_back(folder);
        for (const std::string& root : roots) {
            if (TrackModel* moved = planned.findTrack(root))
                moved->parentId = folder.id;
        }
        for (const TrackModel& before : m_project.tracks) {
            if (!carriesAudio(before)) continue;
            const TrackModel* current = before.outputBusId.empty()
                                            ? nullptr
                                            : m_project.findTrack(before.outputBusId);
            if (!before.outputBusId.empty() &&
                !(current && isFolder(*current))) {
                continue;
            }
            const std::string desired = summingParent(planned, before.id);
            if (desired != before.outputBusId) {
                appendCommand(batch,
                              collab::SetTrackOutput{before.id, desired});
            }
        }
        if (carriesAudio(folder)) {
            const std::string desired = summingParent(planned, folder.id);
            if (!desired.empty()) {
                appendCommand(batch,
                              collab::SetTrackOutput{folder.id, desired});
            }
        }
        if (!sharedBatchApplies(m_project, batch)) return {};
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Pack into Folder");
        return result == collab::SharedMutationResult::Submitted
                   ? folder.id
                   : std::string{};
    }

    std::string folderId;
    {
        UndoStack::Suspend quiet(m_undo);
        folderId = addFolder(summing, name);
        if (auto* folder = m_project.findTrack(folderId)) folder->parentId = parentId;
        moveTrack(folderId, firstIndex, parentId);

        size_t slot = m_project.indexOf(folderId) + 1;
        for (const auto& origin : origins) {
            if (origin.id == folderId) continue;
            if (!moveTrack(origin.id, slot, folderId)) continue;
            slot = m_project.indexOf(origin.id) + 1 +
                   subtreeOf(m_project, origin.id).size();
        }
    }

    m_undo.push("Pack into Folder",
                [this, origins, folderId] {
                    UndoStack::Suspend quiet(m_undo);
                    for (auto it = origins.rbegin(); it != origins.rend(); ++it) {
                        moveTrack(it->id, it->index, it->parentId);
                    }
                    removeTrack(folderId);
                },
                [this, trackIds, name, summing] {
                    packIntoFolder(trackIds, name, summing);
                });
    return folderId;
}

// ── Sends / inserts ────────────────────────────────────────────────────────

std::string EngineController::addSend(const std::string& trackId,
                                      const std::string& destinationTrackId) {
    auto* track = m_project.findTrack(trackId);
    if (!track || trackId == destinationTrackId) return {};
    SendModel send;
    send.id = newUuid();
    send.destinationTrackId = destinationTrackId;
    const std::string afterId =
        track->sends.empty() ? std::string() : track->sends.back().id;
    const auto shared = submitSharedMutation(
        collab::AddSend{trackId, send, afterId}, "Add Send");
    if (shared == collab::SharedMutationResult::Submitted) return send.id;
    if (shared == collab::SharedMutationResult::Blocked) return {};
    track->sends.push_back(send);

    if (!rebuildGraph()) {                 // a send can close a loop too
        track->sends.pop_back();
        rebuildGraph();
        return {};
    }
    return send.id;
}

void EngineController::removeSend(const std::string& trackId,
                                  const std::string& sendId) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    if (std::none_of(track->sends.begin(), track->sends.end(),
                     [&](const SendModel& send) { return send.id == sendId; }))
        return;
    const auto shared = submitSharedMutation(
        collab::DeleteSend{trackId, sendId}, "Remove Send");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    const size_t before = track->sends.size();
    std::erase_if(track->sends, [&](const SendModel& s) { return s.id == sendId; });
    if (track->sends.size() != before) rebuildGraph();
}

namespace {
SendModel* findSend(TrackModel* track, const std::string& sendId) {
    if (!track) return nullptr;
    for (auto& s : track->sends) {
        if (s.id == sendId) return &s;
    }
    return nullptr;
}
} // namespace

void EngineController::setSendLevel(const std::string& trackId,
                                    const std::string& sendId, float level) {
    auto* track = m_project.findTrack(trackId);
    auto* send = findSend(track, sendId);
    if (!send) return;
    // Up to +6 dB, like every other level in the project. A send capped at
    // unity cannot push a quiet source into a reverb hard enough to hear it
    // without turning the destination bus up and unbalancing everything else
    // feeding it.
    send->level = std::clamp(level, 0.0f, kMaxSendLevel);

    // Level changes go straight to the node — no recompile, so a slider drag
    // costs nothing.
    auto found = m_channels.find(trackId);
    if (found == m_channels.end()) return;
    const size_t index = size_t(send - track->sends.data());
    if (index >= found->second.sends.size() || !found->second.sends[index]) return;
    found->second.sends[index]->setLevel(send->enabled ? send->level : 0.0f);
}

void EngineController::commitSendLevelEdit(const std::string& trackId,
                                           const std::string& sendId,
                                           float before,
                                           const std::string& label) {
    const auto* send = findSend(m_project.findTrack(trackId), sendId);
    if (!send || send->level == before) return;
    const float after = send->level;
    const auto shared = submitSharedMutation(
        collab::SetSendProperty{trackId, sendId,
                                collab::SendProperty::Level, double(after)},
        label);
    if (shared == collab::SharedMutationResult::Blocked) {
        setSendLevel(trackId, sendId, before);
        return;
    }
    if (shared == collab::SharedMutationResult::Submitted) return;
    m_undo.push(label,
                [this, trackId, sendId, before] {
                    setSendLevel(trackId, sendId, before);
                },
                [this, trackId, sendId, after] {
                    setSendLevel(trackId, sendId, after);
                });
}

void EngineController::setSendPreFader(const std::string& trackId,
                                       const std::string& sendId, bool preFader) {
    auto* send = findSend(m_project.findTrack(trackId), sendId);
    if (!send || send->preFader == preFader) return;
    const auto shared = submitSharedMutation(
        collab::SetSendProperty{trackId, sendId,
                                collab::SendProperty::PreFader, preFader},
        "Set Send Tap");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    send->preFader = preFader;
    rebuildGraph();               // the tap point is an edge, so recompile
}

void EngineController::setSendEnabled(const std::string& trackId,
                                      const std::string& sendId, bool enabled) {
    auto* track = m_project.findTrack(trackId);
    auto* send = findSend(track, sendId);
    if (!send || send->enabled == enabled) return;
    const auto shared = submitSharedMutation(
        collab::SetSendProperty{trackId, sendId,
                                collab::SendProperty::Enabled, enabled},
        enabled ? "Enable Send" : "Disable Send");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    send->enabled = enabled;
    setSendLevel(trackId, sendId, send->level);
}

bool EngineController::setTrackOutputBus(const std::string& trackId,
                                         const std::string& busTrackId) {
    auto* track = m_project.findTrack(trackId);
    if (!track || trackId == busTrackId) return false;
    if (track->outputBusId == busTrackId) return true;
    const auto shared = submitSharedMutation(
        collab::SetTrackOutput{trackId, busTrackId}, "Set Track Output");
    if (shared == collab::SharedMutationResult::Submitted) return true;
    if (shared == collab::SharedMutationResult::Blocked) return false;

    const std::string previous = track->outputBusId;
    track->outputBusId = busTrackId;
    if (!rebuildGraph()) {
        // Feedback loop (A → B → A): put the old destination back so the
        // document never describes a graph the engine refused.
        track->outputBusId = previous;
        rebuildGraph();
        return false;
    }
    return true;
}

void EngineController::setTrackInputEnabled(const std::string& trackId,
                                            bool enabled) {
    if (auto* t = m_project.findTrack(trackId)) t->inputEnabled = enabled;
}

void EngineController::ensureInsertSlots(const std::string& trackId,
                                          size_t count) {
    if (cloudProjectBound()) return;
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    while (track->inserts.size() < count) {
        InsertModel slot;
        slot.id = newUuid();
        track->inserts.push_back(slot);
    }
}

void EngineController::ensureMasterInsertSlots(size_t count) {
    if (cloudProjectBound()) return;
    while (m_project.masterInserts.size() < count) {
        InsertModel slot;
        slot.id = newUuid();
        m_project.masterInserts.push_back(slot);
    }
}

// ── Plugin insert commands ─────────────────────────────────────────────────
//
// Structural edits (add, remove, move, replace) mutate the document, rebuild
// the graph and push an undo entry — the shape `addTrack` established. Live
// edits (bypass, mix, parameters) go straight to the node with no recompile,
// the shape `setSendLevel` established, because they are dragged or clicked
// repeatedly and a recompile per change would be audible.

std::string EngineController::addInsert(const std::string& channelId,
                                        const plugins::PluginDescriptor& descriptor,
                                        size_t index) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return {};

    InsertModel slot;
    slot.id = newUuid();
    applyDescriptor(slot, descriptor);

    const size_t at = std::min(index, slots->size());
    if (cloudProjectBound()) {
        if (slot.format != PluginFormat::Internal) return {};
        const auto result = submitSharedMutation(
            collab::AddPluginInsert{channelPluginLocation(channelId), slot,
                                    previousIdAt(*slots, at)},
            "Add " + descriptor.name);
        return result == collab::SharedMutationResult::Submitted ? slot.id
                                                                 : std::string{};
    }
    slots->insert(slots->begin() + std::ptrdiff_t(at), slot);
    rebuildGraph();

    // The plugin may have failed to load — a stale cache entry, a plugin
    // deleted since the scan. Do not leave a slot the user cannot use.
    if (!insertNode(channelId, slot.id)) {
        slots = mutableChannelInserts(channelId);
        if (slots) std::erase_if(*slots, [&](const InsertModel& s) { return s.id == slot.id; });
        rebuildGraph();
        return {};
    }

    const std::string id = slot.id;
    m_undo.push("Add " + descriptor.name,
                [this, channelId, id] { removeInsert(channelId, id); },
                [this, channelId, slot, at] {
                    std::vector<InsertModel>* target = mutableChannelInserts(channelId);
                    if (!target) return;
                    target->insert(target->begin() +
                                       std::ptrdiff_t(std::min(at, target->size())),
                                   slot);
                    rebuildGraph();
                });
    return id;
}

void EngineController::removeInsert(const std::string& channelId,
                                    const std::string& insertId) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return;
    auto found = std::find_if(slots->begin(), slots->end(),
                              [&](const InsertModel& s) { return s.id == insertId; });
    if (found == slots->end()) return;

    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        (void)submitSharedMutation(
            collab::DeletePluginInsert{channelPluginLocation(channelId),
                                       insertId},
            "Remove " + found->name);
        return;
    }

    // Capture the slot before erasing so redo can put back exactly what was
    // there, state file reference included.
    const InsertModel removed = *found;
    const size_t at = size_t(found - slots->begin());
    slots->erase(found);
    rebuildGraph();

    m_undo.push("Remove " + removed.name,
                [this, channelId, removed, at] {
                    std::vector<InsertModel>* target = mutableChannelInserts(channelId);
                    if (!target) return;
                    target->insert(target->begin() +
                                       std::ptrdiff_t(std::min(at, target->size())),
                                   removed);
                    rebuildGraph();
                },
                [this, channelId, insertId] { removeInsert(channelId, insertId); });
}

void EngineController::moveInsert(const std::string& channelId,
                                  const std::string& insertId,
                                  size_t targetIndex) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return;
    auto found = std::find_if(slots->begin(), slots->end(),
                              [&](const InsertModel& s) { return s.id == insertId; });
    if (found == slots->end()) return;

    const size_t from = size_t(found - slots->begin());
    const size_t to = std::min(targetIndex, slots->size() - 1);
    if (from == to) return;

    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        std::vector<InsertModel> scratch = *slots;
        InsertModel moved = scratch[from];
        scratch.erase(scratch.begin() + std::ptrdiff_t(from));
        scratch.insert(scratch.begin() + std::ptrdiff_t(to), moved);
        (void)submitSharedMutation(
            collab::MovePluginInsert{
                channelPluginLocation(channelId), insertId,
                previousIdAt(scratch, to)},
            "Reorder " + moved.name);
        return;
    }

    const InsertModel moved = *found;
    slots->erase(found);
    slots->insert(slots->begin() + std::ptrdiff_t(to), moved);
    rebuildGraph();

    m_undo.push("Reorder " + moved.name,
                [this, channelId, insertId, from] {
                    moveInsert(channelId, insertId, from);
                },
                [this, channelId, insertId, to] {
                    moveInsert(channelId, insertId, to);
                });
}

bool EngineController::replaceInsert(const std::string& channelId,
                                     const std::string& insertId,
                                     const plugins::PluginDescriptor& descriptor) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return false;
    auto found = std::find_if(slots->begin(), slots->end(),
                              [&](const InsertModel& s) { return s.id == insertId; });
    if (found == slots->end()) return false;

    const InsertModel before = *found;
    // The slot id survives on purpose: automation lanes address a parameter as
    // "<insertId>:<parameterId>", and an open editor window is keyed on it too.
    InsertModel replacement = before;
    applyDescriptor(replacement, descriptor);
    if (cloudProjectBound()) {
        if (before.format != PluginFormat::Internal ||
            replacement.format != PluginFormat::Internal) {
            return false;
        }
        const auto result = submitSharedMutation(
            collab::ReplacePluginInsert{channelPluginLocation(channelId),
                                        insertId, replacement},
            "Replace " + before.name);
        return result == collab::SharedMutationResult::Submitted;
    }
    *found = replacement;
    rebuildGraph();

    // The same guard `addInsert` and `setTrackInstrumentPlugin` have, and for
    // the same reason: a plugin the cache still lists can fail to instantiate
    // here — a licence that lapsed, a module that no longer advertises the
    // class, a plugin deleted since the scan. Keeping the plugin that was
    // working beats leaving a slot that names one and has none.
    if (!insertNode(channelId, insertId)) {
        slots = mutableChannelInserts(channelId);
        if (slots) {
            for (InsertModel& slot : *slots)
                if (slot.id == insertId) slot = before;
        }
        rebuildGraph();
        return false;
    }

    const InsertModel after = *found;
    m_undo.push("Replace " + before.name,
                [this, channelId, insertId, before] {
                    std::vector<InsertModel>* target = mutableChannelInserts(channelId);
                    if (!target) return;
                    for (InsertModel& slot : *target) {
                        if (slot.id == insertId) slot = before;
                    }
                    rebuildGraph();
                },
                [this, channelId, insertId, after] {
                    std::vector<InsertModel>* target = mutableChannelInserts(channelId);
                    if (!target) return;
                    for (InsertModel& slot : *target) {
                        if (slot.id == insertId) slot = after;
                    }
                    rebuildGraph();
                });
    return true;
}

void EngineController::setInsertBypassed(const std::string& channelId,
                                         const std::string& insertId,
                                         bool bypassed) {
    InsertModel* slot = mutableInsertSlot(channelId, insertId);
    if (!slot || slot->bypassed == bypassed) return;
    const auto shared = submitSharedMutation(
        collab::SetPluginProperty{channelPluginLocation(channelId), insertId,
                                  collab::PluginProperty::Bypassed, bypassed},
        bypassed ? "Bypass " + slot->name : "Enable " + slot->name);
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    slot->bypassed = bypassed;

    // Straight to the node: bypass is a crossfade inside PluginNode and
    // must not cost a graph recompile.
    if (InsertSlot* live = liveInsertSlot(channelId, insertId)) {
        if (live->node) live->node->setBypassed(bypassed);
        if (live->rightNode) live->rightNode->setBypassed(bypassed);
    }
    m_undo.push(bypassed ? "Bypass " + slot->name : "Enable " + slot->name,
                [this, channelId, insertId, bypassed] {
                    setInsertBypassed(channelId, insertId, !bypassed);
                },
                [this, channelId, insertId, bypassed] {
                    setInsertBypassed(channelId, insertId, bypassed);
                });
}

void EngineController::setAllInsertsBypassed(const std::string& channelId,
                                             bool bypassed) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return;

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const InsertModel& slot : *slots) {
            if (slot.bypassed == bypassed) continue;
            if (slot.format != PluginFormat::Internal) return;
            appendCommand(batch, collab::SetPluginProperty{
                channelPluginLocation(channelId), slot.id,
                collab::PluginProperty::Bypassed, bypassed});
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       bypassed ? "Bypass All" : "Enable All");
        }
        return;
    }

    // Remember which slots this actually changes, so undo can put back the
    // hand-set flags rather than enabling everything indiscriminately.
    std::vector<std::string> changed;
    for (InsertModel& slot : *slots) {
        if (slot.bypassed == bypassed) continue;
        slot.bypassed = bypassed;
        if (InsertSlot* live = liveInsertSlot(channelId, slot.id)) {
            if (live->node) live->node->setBypassed(bypassed);
            if (live->rightNode) live->rightNode->setBypassed(bypassed);
        }
        changed.push_back(slot.id);
    }
    if (changed.empty()) return;

    auto setEach = [this, channelId, changed](bool to) {
        for (const std::string& id : changed) {
            InsertModel* slot = mutableInsertSlot(channelId, id);
            if (!slot) continue;   // removed since; nothing to restore
            slot->bypassed = to;
            if (InsertSlot* live = liveInsertSlot(channelId, id)) {
                if (live->node) live->node->setBypassed(to);
                if (live->rightNode) live->rightNode->setBypassed(to);
            }
        }
    };
    m_undo.push(bypassed ? "Bypass All" : "Enable All",
                [setEach, bypassed] { setEach(!bypassed); },
                [setEach, bypassed] { setEach(bypassed); });
}

void EngineController::setInsertMix(const std::string& channelId,
                                    const std::string& insertId, float mix) {
    InsertModel* slot = mutableInsertSlot(channelId, insertId);
    if (!slot) return;
    slot->mix = std::clamp(mix, 0.0f, 1.0f);
    if (InsertSlot* live = liveInsertSlot(channelId, insertId)) {
        if (live->node) live->node->setMix(slot->mix);
        if (live->rightNode) live->rightNode->setMix(slot->mix);
    }
}

void EngineController::commitInsertMixEdit(const std::string& channelId,
                                            const std::string& insertId,
                                            float beforeMix,
                                            const std::string& label) {
    InsertModel* slot = mutableInsertSlot(channelId, insertId);
    if (!slot) return;
    const float afterMix = slot->mix;
    if (std::abs(afterMix - beforeMix) < 1e-6f) return;
    const auto shared = submitSharedMutation(
        collab::SetPluginProperty{channelPluginLocation(channelId), insertId,
                                  collab::PluginProperty::Mix,
                                  double(afterMix)},
        label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked)
            setInsertMix(channelId, insertId, beforeMix);
        return;
    }
    m_undo.push(label,
                [this, channelId, insertId, beforeMix] {
                    setInsertMix(channelId, insertId, beforeMix);
                },
                [this, channelId, insertId, afterMix] {
                    setInsertMix(channelId, insertId, afterMix);
                });
}

bool EngineController::setInsertChannelMode(const std::string& channelId,
                                            const std::string& insertId,
                                            PluginChannelMode mode) {
    InsertModel* slot = mutableInsertSlot(channelId, insertId);
    if (!slot || slot->channelMode == mode) return slot != nullptr;
    if (mode == PluginChannelMode::DualMono) {
        plugins::PluginNode* primary = insertNode(channelId, insertId);
        if (!primary || !primary->instance() ||
            primary->instance()->descriptor().isInstrument ||
            primary->instance()->busLayout().inputs.empty()) {
            return false;
        }
    }
    const PluginChannelMode before = slot->channelMode;
    const std::string name = slot->name;
    const auto shared = submitSharedMutation(
        collab::SetPluginProperty{
            channelPluginLocation(channelId), insertId,
            collab::PluginProperty::ChannelMode,
            toString(mode)},
        "Change " + name + " Channel Mode");
    if (shared != collab::SharedMutationResult::LocalFallback)
        return shared == collab::SharedMutationResult::Submitted;
    slot->channelMode = mode;
    const bool rebuilt = bool(rebuildGraph(/*reconfigurePlugins=*/true));
    const InsertSlot* live = liveInsertSlot(channelId, insertId);
    if (!rebuilt ||
        (mode == PluginChannelMode::DualMono &&
         (!live || !live->rightNode))) {
        slot = mutableInsertSlot(channelId, insertId);
        if (slot) slot->channelMode = before;
        rebuildGraph(/*reconfigurePlugins=*/true);
        return false;
    }
    m_undo.push("Change " + name + " Channel Mode",
                [this, channelId, insertId, before] {
                    setInsertChannelMode(channelId, insertId, before);
                },
                [this, channelId, insertId, mode] {
                    setInsertChannelMode(channelId, insertId, mode);
                });
    return true;
}

void EngineController::setInsertEditorChannel(
    const std::string& channelId, const std::string& insertId,
    PluginEditorChannel channel) {
    if (InsertModel* slot = mutableInsertSlot(channelId, insertId)) {
        slot->editorChannel = channel;
    }
}

std::vector<EngineController::SidechainSource>
EngineController::insertSidechainSources(const std::string& channelId) const {
    // A source closes a feedback loop iff the destination can already reach
    // it through normal outputs/sends. The old implementation ran a fresh BFS
    // for every candidate track: O(T * (T + E)) every 200 ms per open editor.
    // Reachability depends only on the destination, so compute it once.
    std::unordered_set<std::string> feedbackSources;
    feedbackSources.reserve(m_project.tracks.size() + 1);
    std::vector<std::string> pending{channelId};
    pending.reserve(m_project.tracks.size() + 1);
    while (!pending.empty()) {
        std::string current = std::move(pending.back());
        pending.pop_back();
        if (!feedbackSources.insert(current).second) continue;
        const TrackModel* track = m_project.findTrack(current);
        if (!track) continue; // master has no outgoing project route
        if (!track->outputBusId.empty()) pending.push_back(track->outputBusId);
        for (const SendModel& send : track->sends) {
            // Disabled sends remain graph edges at zero gain, so they still
            // count for cycle safety, matching sidechainWouldFeedback().
            if (!send.destinationTrackId.empty())
                pending.push_back(send.destinationTrackId);
        }
    }

    std::vector<SidechainSource> out;
    out.reserve(m_project.tracks.size());
    for (const TrackModel& track : m_project.tracks) {
        if (!carriesAudio(track) || feedbackSources.contains(track.id)) {
            continue;
        }
        out.push_back(SidechainSource{track.id, track.name});
    }
    return out;
}

bool EngineController::insertSupportsSidechain(
    const std::string& channelId, const std::string& insertId) const {
    auto* self = const_cast<EngineController*>(this);
    plugins::PluginNode* node = self->insertNode(channelId, insertId);
    if (!node || !node->instance()) return false;
    return node->instance()->busLayout().inputs.size() > 1;
}

bool EngineController::setInsertSidechainSource(
    const std::string& channelId, const std::string& insertId,
    const std::string& sourceTrackId) {
    InsertModel* slot = mutableInsertSlot(channelId, insertId);
    if (!slot || slot->sidechainTrackId == sourceTrackId) return slot != nullptr;
    if (!sourceTrackId.empty()) {
        const TrackModel* source = m_project.findTrack(sourceTrackId);
        if (!source || !carriesAudio(*source) ||
            !insertSupportsSidechain(channelId, insertId) ||
            sidechainWouldFeedback(channelId, sourceTrackId)) {
            return false;
        }
    }

    const std::string before = slot->sidechainTrackId;
    const std::string name = slot->name;
    const auto shared = submitSharedMutation(
        collab::SetPluginProperty{channelPluginLocation(channelId), insertId,
                                  collab::PluginProperty::SidechainTrackId,
                                  sourceTrackId},
        "Change " + name + " Sidechain");
    if (shared != collab::SharedMutationResult::LocalFallback)
        return shared == collab::SharedMutationResult::Submitted;
    slot->sidechainTrackId = sourceTrackId;
    if (!rebuildGraph()) {
        slot = mutableInsertSlot(channelId, insertId);
        if (slot) slot->sidechainTrackId = before;
        rebuildGraph();
        return false;
    }
    m_undo.push("Change " + name + " Sidechain",
                [this, channelId, insertId, before] {
                    setInsertSidechainSource(channelId, insertId, before);
                },
                [this, channelId, insertId, sourceTrackId] {
                    setInsertSidechainSource(channelId, insertId,
                                             sourceTrackId);
                });
    return true;
}

const SamplerFxModel* EngineController::samplerFx(
    const std::string& trackId, const std::string& samplerSlotId) const {
    const TrackModel* track = m_project.findTrack(trackId);
    if (!track || track->instrument.id != samplerSlotId ||
        !track->samplerFx.isOwnedBy(track->instrument)) {
        return nullptr;
    }
    return &track->samplerFx;
}

std::string EngineController::addSamplerFxInsert(
    const std::string& trackId, const std::string& samplerSlotId,
    const plugins::PluginDescriptor& descriptor, size_t index) {
    std::vector<InsertModel>* slots = mutableSamplerFxInserts(trackId, samplerSlotId);
    if (!slots || slots->size() >= kSamplerFxSlots || descriptor.isInstrument) return {};

    InsertModel slot;
    slot.id = newUuid();
    applyDescriptor(slot, descriptor);
    const size_t at = std::min(index, slots->size());
    if (cloudProjectBound()) {
        if (slot.format != PluginFormat::Internal) return {};
        const auto result = submitSharedMutation(
            collab::AddPluginInsert{
                {collab::PluginChain::SamplerFx, trackId, {}}, slot,
                previousIdAt(*slots, at)},
            "Add Sampler FX " + descriptor.name);
        return result == collab::SharedMutationResult::Submitted ? slot.id
                                                                 : std::string{};
    }
    slots->insert(slots->begin() + std::ptrdiff_t(at), slot);
    rebuildGraph();

    if (!insertNode(trackId, slot.id)) {
        slots = mutableSamplerFxInserts(trackId, samplerSlotId);
        if (slots) {
            std::erase_if(*slots, [&](const InsertModel& value) {
                return value.id == slot.id;
            });
        }
        rebuildGraph();
        return {};
    }

    const std::string id = slot.id;
    m_undo.push("Add Sampler FX " + descriptor.name,
                [this, trackId, samplerSlotId, id] {
                    removeSamplerFxInsert(trackId, samplerSlotId, id);
                },
                [this, trackId, samplerSlotId, slot, at] {
                    auto* target = mutableSamplerFxInserts(trackId, samplerSlotId);
                    if (!target || target->size() >= kSamplerFxSlots) return;
                    target->insert(target->begin() + std::ptrdiff_t(
                                       std::min(at, target->size())),
                                   slot);
                    rebuildGraph();
                });
    return id;
}

void EngineController::removeSamplerFxInsert(const std::string& trackId,
                                              const std::string& samplerSlotId,
                                              const std::string& insertId) {
    auto* slots = mutableSamplerFxInserts(trackId, samplerSlotId);
    if (!slots) return;
    const auto found = std::find_if(slots->begin(), slots->end(),
                                    [&](const InsertModel& value) {
                                        return value.id == insertId;
                                    });
    if (found == slots->end()) return;
    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        (void)submitSharedMutation(
            collab::DeletePluginInsert{
                {collab::PluginChain::SamplerFx, trackId, {}}, insertId},
            "Remove Sampler FX " + found->name);
        return;
    }
    const InsertModel removed = *found;
    const size_t at = size_t(found - slots->begin());
    slots->erase(found);
    rebuildGraph();

    m_undo.push("Remove Sampler FX " + removed.name,
                [this, trackId, samplerSlotId, removed, at] {
                    auto* target = mutableSamplerFxInserts(trackId, samplerSlotId);
                    if (!target || target->size() >= kSamplerFxSlots) return;
                    target->insert(target->begin() + std::ptrdiff_t(
                                       std::min(at, target->size())),
                                   removed);
                    rebuildGraph();
                },
                [this, trackId, samplerSlotId, insertId] {
                    removeSamplerFxInsert(trackId, samplerSlotId, insertId);
                });
}

void EngineController::moveSamplerFxInsert(const std::string& trackId,
                                            const std::string& samplerSlotId,
                                            const std::string& insertId,
                                            size_t targetIndex) {
    auto* slots = mutableSamplerFxInserts(trackId, samplerSlotId);
    if (!slots || slots->empty()) return;
    const auto found = std::find_if(slots->begin(), slots->end(),
                                    [&](const InsertModel& value) {
                                        return value.id == insertId;
                                    });
    if (found == slots->end()) return;
    const size_t from = size_t(found - slots->begin());
    const size_t to = std::min(targetIndex, slots->size() - 1);
    if (from == to) return;
    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        std::vector<InsertModel> scratch = *slots;
        InsertModel moved = scratch[from];
        scratch.erase(scratch.begin() + std::ptrdiff_t(from));
        scratch.insert(scratch.begin() + std::ptrdiff_t(to), moved);
        (void)submitSharedMutation(
            collab::MovePluginInsert{
                {collab::PluginChain::SamplerFx, trackId, {}}, insertId,
                previousIdAt(scratch, to)},
            "Reorder Sampler FX");
        return;
    }
    const InsertModel moved = *found;
    slots->erase(found);
    slots->insert(slots->begin() + std::ptrdiff_t(to), moved);
    rebuildGraph();

    m_undo.push("Reorder Sampler FX",
                [this, trackId, samplerSlotId, insertId, from] {
                    moveSamplerFxInsert(trackId, samplerSlotId, insertId, from);
                },
                [this, trackId, samplerSlotId, insertId, to] {
                    moveSamplerFxInsert(trackId, samplerSlotId, insertId, to);
                });
}

bool EngineController::replaceSamplerFxInsert(
    const std::string& trackId, const std::string& samplerSlotId,
    const std::string& insertId, const plugins::PluginDescriptor& descriptor) {
    if (descriptor.isInstrument) return false;
    auto* slots = mutableSamplerFxInserts(trackId, samplerSlotId);
    if (!slots) return false;
    auto found = std::find_if(slots->begin(), slots->end(),
                              [&](const InsertModel& value) {
                                  return value.id == insertId;
                              });
    if (found == slots->end()) return false;
    const InsertModel before = *found;
    InsertModel replacement = before;
    applyDescriptor(replacement, descriptor);
    if (cloudProjectBound()) {
        if (before.format != PluginFormat::Internal ||
            replacement.format != PluginFormat::Internal) return false;
        const auto result = submitSharedMutation(
            collab::ReplacePluginInsert{
                {collab::PluginChain::SamplerFx, trackId, {}}, insertId,
                replacement},
            "Replace Sampler FX " + before.name);
        return result == collab::SharedMutationResult::Submitted;
    }
    *found = replacement;
    rebuildGraph();
    if (!insertNode(trackId, insertId)) {
        slots = mutableSamplerFxInserts(trackId, samplerSlotId);
        if (slots) {
            for (InsertModel& value : *slots) {
                if (value.id == insertId) value = before;
            }
        }
        rebuildGraph();
        return false;
    }
    const InsertModel after = *found;
    auto apply = [this, trackId, samplerSlotId, insertId](const InsertModel& value) {
        auto* target = mutableSamplerFxInserts(trackId, samplerSlotId);
        if (!target) return;
        for (InsertModel& slot : *target) {
            if (slot.id == insertId) slot = value;
        }
        rebuildGraph();
    };
    m_undo.push("Replace Sampler FX " + before.name,
                [apply, before] { apply(before); },
                [apply, after] { apply(after); });
    return true;
}

void EngineController::setAllSamplerFxBypassed(const std::string& trackId,
                                                const std::string& samplerSlotId,
                                                bool bypassed) {
    auto* slots = mutableSamplerFxInserts(trackId, samplerSlotId);
    if (!slots) return;
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const InsertModel& slot : *slots) {
            if (slot.bypassed == bypassed) continue;
            if (slot.format != PluginFormat::Internal) return;
            appendCommand(batch, collab::SetPluginProperty{
                {collab::PluginChain::SamplerFx, trackId, {}}, slot.id,
                collab::PluginProperty::Bypassed, bypassed});
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       bypassed ? "Bypass Sampler FX"
                                                : "Enable Sampler FX");
        }
        return;
    }
    std::vector<std::string> changed;
    for (InsertModel& slot : *slots) {
        if (slot.bypassed == bypassed) continue;
        slot.bypassed = bypassed;
        if (InsertSlot* live = liveInsertSlot(trackId, slot.id)) {
            if (live->node) live->node->setBypassed(bypassed);
            if (live->rightNode) live->rightNode->setBypassed(bypassed);
        }
        changed.push_back(slot.id);
    }
    if (changed.empty()) return;
    auto apply = [this, trackId, changed](bool value) {
        for (const std::string& id : changed) {
            if (InsertModel* slot = mutableInsertSlot(trackId, id)) {
                slot->bypassed = value;
                if (InsertSlot* live = liveInsertSlot(trackId, id)) {
                    if (live->node) live->node->setBypassed(value);
                    if (live->rightNode) live->rightNode->setBypassed(value);
                }
            }
        }
    };
    m_undo.push(bypassed ? "Bypass Sampler FX" : "Enable Sampler FX",
                [apply, bypassed] { apply(!bypassed); },
                [apply, bypassed] { apply(bypassed); });
}

void EngineController::setSamplerFxVolume(const std::string& trackId,
                                           const std::string& samplerSlotId,
                                           float volume) {
    TrackModel* track = m_project.findTrack(trackId);
    if (!track || track->instrument.id != samplerSlotId ||
        !track->samplerFx.isOwnedBy(track->instrument)) return;
    track->samplerFx.volume = std::clamp(volume, 0.0f, 2.0f);
    if (TrackChannel* channel = findChannel(trackId); channel && channel->samplerFader) {
        channel->samplerFader->setGain(track->samplerFx.volume);
    }
}

void EngineController::setSamplerFxPan(const std::string& trackId,
                                        const std::string& samplerSlotId,
                                        float pan) {
    TrackModel* track = m_project.findTrack(trackId);
    if (!track || track->instrument.id != samplerSlotId ||
        !track->samplerFx.isOwnedBy(track->instrument)) return;
    track->samplerFx.pan = std::clamp(pan, -1.0f, 1.0f);
    if (TrackChannel* channel = findChannel(trackId); channel && channel->samplerFader) {
        channel->samplerFader->setPan(track->samplerFx.pan);
    }
}

void EngineController::commitSamplerFxLevelEdit(
    const std::string& trackId, const std::string& samplerSlotId,
    float beforeVolume, float beforePan, const std::string& label) {
    const SamplerFxModel* model = samplerFx(trackId, samplerSlotId);
    if (!model) return;
    const float afterVolume = model->volume;
    const float afterPan = model->pan;
    if (beforeVolume == afterVolume && beforePan == afterPan) return;
    const auto shared = submitSharedMutation(
        collab::SetSamplerFxLevels{trackId, samplerSlotId,
                                   double(afterVolume), double(afterPan)},
        label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked) {
            setSamplerFxVolume(trackId, samplerSlotId, beforeVolume);
            setSamplerFxPan(trackId, samplerSlotId, beforePan);
        }
        return;
    }
    auto apply = [this, trackId, samplerSlotId](float volume, float pan) {
        setSamplerFxVolume(trackId, samplerSlotId, volume);
        setSamplerFxPan(trackId, samplerSlotId, pan);
    };
    m_undo.push(label,
                [apply, beforeVolume, beforePan] { apply(beforeVolume, beforePan); },
                [apply, afterVolume, afterPan] { apply(afterVolume, afterPan); });
}

const std::vector<InsertModel>* EngineController::clipFx(
    const std::string& trackId, const std::string& clipId) const {
    const ClipModel* clip = audioClip(trackId, clipId);
    return clip ? &clip->inserts : nullptr;
}

std::string EngineController::addClipFxInsert(
    const std::string& trackId, const std::string& clipId,
    const plugins::PluginDescriptor& descriptor, size_t index) {
    auto* slots = mutableClipFxInserts(trackId, clipId);
    if (!slots || slots->size() >= kSamplerFxSlots || descriptor.isInstrument) return {};
    InsertModel slot;
    slot.id = newUuid();
    applyDescriptor(slot, descriptor);
    const size_t at = std::min(index, slots->size());
    if (cloudProjectBound()) {
        if (slot.format != PluginFormat::Internal) return {};
        const auto result = submitSharedMutation(
            collab::AddPluginInsert{
                {collab::PluginChain::Clip, trackId, clipId}, slot,
                previousIdAt(*slots, at)},
            "Add Clip FX " + descriptor.name);
        return result == collab::SharedMutationResult::Submitted ? slot.id
                                                                 : std::string{};
    }
    slots->insert(slots->begin() + std::ptrdiff_t(at), slot);
    rebuildGraph();
    if (!insertNode(trackId, slot.id)) {
        if (auto* current = mutableClipFxInserts(trackId, clipId)) {
            std::erase_if(*current, [&](const InsertModel& value) {
                return value.id == slot.id;
            });
        }
        rebuildGraph();
        return {};
    }
    const std::string id = slot.id;
    m_undo.push("Add Clip FX " + descriptor.name,
                [this, trackId, clipId, id] {
                    removeClipFxInsert(trackId, clipId, id);
                },
                [this, trackId, clipId, slot, at] {
                    auto* target = mutableClipFxInserts(trackId, clipId);
                    if (!target || target->size() >= kSamplerFxSlots) return;
                    target->insert(target->begin() + std::ptrdiff_t(
                                       std::min(at, target->size())), slot);
                    rebuildGraph();
                });
    return id;
}

void EngineController::removeClipFxInsert(const std::string& trackId,
                                           const std::string& clipId,
                                           const std::string& insertId) {
    auto* slots = mutableClipFxInserts(trackId, clipId);
    if (!slots) return;
    const auto found = std::find_if(slots->begin(), slots->end(),
                                    [&](const InsertModel& value) {
                                        return value.id == insertId;
                                    });
    if (found == slots->end()) return;
    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        (void)submitSharedMutation(
            collab::DeletePluginInsert{
                {collab::PluginChain::Clip, trackId, clipId}, insertId},
            "Remove Clip FX " + found->name);
        return;
    }
    const InsertModel removed = *found;
    const size_t at = size_t(found - slots->begin());
    slots->erase(found);
    rebuildGraph();
    m_undo.push("Remove Clip FX " + removed.name,
                [this, trackId, clipId, removed, at] {
                    auto* target = mutableClipFxInserts(trackId, clipId);
                    if (!target || target->size() >= kSamplerFxSlots) return;
                    target->insert(target->begin() + std::ptrdiff_t(
                                       std::min(at, target->size())), removed);
                    rebuildGraph();
                },
                [this, trackId, clipId, insertId] {
                    removeClipFxInsert(trackId, clipId, insertId);
                });
}

void EngineController::moveClipFxInsert(const std::string& trackId,
                                         const std::string& clipId,
                                         const std::string& insertId,
                                         size_t targetIndex) {
    auto* slots = mutableClipFxInserts(trackId, clipId);
    if (!slots || slots->empty()) return;
    const auto found = std::find_if(slots->begin(), slots->end(),
                                    [&](const InsertModel& value) {
                                        return value.id == insertId;
                                    });
    if (found == slots->end()) return;
    const size_t from = size_t(found - slots->begin());
    const size_t to = std::min(targetIndex, slots->size() - 1);
    if (from == to) return;
    if (cloudProjectBound()) {
        if (found->format != PluginFormat::Internal) return;
        std::vector<InsertModel> scratch = *slots;
        InsertModel moved = scratch[from];
        scratch.erase(scratch.begin() + std::ptrdiff_t(from));
        scratch.insert(scratch.begin() + std::ptrdiff_t(to), moved);
        (void)submitSharedMutation(
            collab::MovePluginInsert{
                {collab::PluginChain::Clip, trackId, clipId}, insertId,
                previousIdAt(scratch, to)},
            "Reorder Clip FX");
        return;
    }
    const InsertModel moved = *found;
    slots->erase(found);
    slots->insert(slots->begin() + std::ptrdiff_t(to), moved);
    rebuildGraph();
    m_undo.push("Reorder Clip FX",
                [this, trackId, clipId, insertId, from] {
                    moveClipFxInsert(trackId, clipId, insertId, from);
                },
                [this, trackId, clipId, insertId, to] {
                    moveClipFxInsert(trackId, clipId, insertId, to);
                });
}

bool EngineController::replaceClipFxInsert(
    const std::string& trackId, const std::string& clipId,
    const std::string& insertId, const plugins::PluginDescriptor& descriptor) {
    if (descriptor.isInstrument) return false;
    auto* slots = mutableClipFxInserts(trackId, clipId);
    if (!slots) return false;
    auto found = std::find_if(slots->begin(), slots->end(),
                              [&](const InsertModel& value) {
                                  return value.id == insertId;
                              });
    if (found == slots->end()) return false;
    const InsertModel before = *found;
    InsertModel replacement = before;
    applyDescriptor(replacement, descriptor);
    if (cloudProjectBound()) {
        if (before.format != PluginFormat::Internal ||
            replacement.format != PluginFormat::Internal) return false;
        const auto result = submitSharedMutation(
            collab::ReplacePluginInsert{
                {collab::PluginChain::Clip, trackId, clipId}, insertId,
                replacement},
            "Replace Clip FX " + before.name);
        return result == collab::SharedMutationResult::Submitted;
    }
    *found = replacement;
    rebuildGraph();
    if (!insertNode(trackId, insertId)) {
        if (auto* current = mutableClipFxInserts(trackId, clipId)) {
            for (InsertModel& value : *current) {
                if (value.id == insertId) value = before;
            }
        }
        rebuildGraph();
        return false;
    }
    const InsertModel after = *found;
    auto apply = [this, trackId, clipId, insertId](const InsertModel& value) {
        auto* target = mutableClipFxInserts(trackId, clipId);
        if (!target) return;
        for (InsertModel& slot : *target) {
            if (slot.id == insertId) slot = value;
        }
        rebuildGraph();
    };
    m_undo.push("Replace Clip FX " + before.name,
                [apply, before] { apply(before); },
                [apply, after] { apply(after); });
    return true;
}

void EngineController::setAllClipFxBypassed(const std::string& trackId,
                                             const std::string& clipId,
                                             bool bypassed) {
    auto* slots = mutableClipFxInserts(trackId, clipId);
    if (!slots) return;
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const InsertModel& slot : *slots) {
            if (slot.bypassed == bypassed) continue;
            if (slot.format != PluginFormat::Internal) return;
            appendCommand(batch, collab::SetPluginProperty{
                {collab::PluginChain::Clip, trackId, clipId}, slot.id,
                collab::PluginProperty::Bypassed, bypassed});
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       bypassed ? "Bypass Clip FX"
                                                : "Enable Clip FX");
        }
        return;
    }
    std::vector<std::string> changed;
    for (InsertModel& slot : *slots) {
        if (slot.bypassed == bypassed) continue;
        slot.bypassed = bypassed;
        if (InsertSlot* live = liveInsertSlot(trackId, slot.id)) {
            if (live->node) live->node->setBypassed(bypassed);
            if (live->rightNode) live->rightNode->setBypassed(bypassed);
        }
        changed.push_back(slot.id);
    }
    if (changed.empty()) return;
    auto apply = [this, trackId, changed](bool value) {
        for (const std::string& id : changed) {
            if (InsertModel* slot = mutableInsertSlot(trackId, id)) {
                slot->bypassed = value;
                if (InsertSlot* live = liveInsertSlot(trackId, id)) {
                    if (live->node) live->node->setBypassed(value);
                    if (live->rightNode) live->rightNode->setBypassed(value);
                }
            }
        }
    };
    m_undo.push(bypassed ? "Bypass Clip FX" : "Enable Clip FX",
                [apply, bypassed] { apply(!bypassed); },
                [apply, bypassed] { apply(bypassed); });
}

void EngineController::setClipFxVolume(const std::string& trackId,
                                        const std::string& clipId, float volume) {
    TrackModel* track = m_project.findTrack(trackId);
    ClipModel* clip = findClip(trackId, clipId);
    if (!track || !clip || clip->kind != ClipKind::Audio) return;
    // The arrangement handle is the broad creative range (+24 dB). Compact
    // context controls deliberately expose a narrower +12 dB range themselves.
    constexpr float kMaxClipGain = 15.8489319f; // 10^(24/20)
    const float applied = std::clamp(volume, 0.0f, kMaxClipGain);
    if (clip->gain == applied) return;
    clip->gain = applied;
    if (TrackChannel* channel = findChannel(trackId)) {
        auto found = channel->clipFx.find(clipId);
        if (found != channel->clipFx.end() && found->second.fader) {
            found->second.fader->setGain(clip->gain);
            return;
        }
    }
    syncTrackClips(*track);
}

void EngineController::setClipFxPan(const std::string& trackId,
                                     const std::string& clipId, float pan) {
    TrackModel* track = m_project.findTrack(trackId);
    ClipModel* clip = findClip(trackId, clipId);
    if (!track || !clip || clip->kind != ClipKind::Audio) return;
    const float applied = std::clamp(pan, -1.0f, 1.0f);
    if (clip->pan == applied) return;
    clip->pan = applied;
    if (TrackChannel* channel = findChannel(trackId)) {
        auto found = channel->clipFx.find(clipId);
        if (found != channel->clipFx.end() && found->second.fader) {
            found->second.fader->setPan(clip->pan);
            return;
        }
    }
    syncTrackClips(*track);
}

void EngineController::commitClipFxLevelEdit(
    const std::string& trackId, const std::string& clipId,
    float beforeVolume, float beforePan, const std::string& label) {
    const ClipModel* clip = audioClip(trackId, clipId);
    if (!clip) return;
    const float afterVolume = clip->gain;
    const float afterPan = clip->pan;
    if (beforeVolume == afterVolume && beforePan == afterPan) return;
    auto batch = std::make_shared<collab::BatchCommand>();
    appendCommand(batch, collab::SetClipProperty{
        trackId, clipId, collab::ClipProperty::Gain, double(afterVolume)});
    appendCommand(batch, collab::SetClipProperty{
        trackId, clipId, collab::ClipProperty::Pan, double(afterPan)});
    const auto shared = submitSharedMutation(
        collab::CommandBody{std::move(batch)}, label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked) {
            setClipFxVolume(trackId, clipId, beforeVolume);
            setClipFxPan(trackId, clipId, beforePan);
        }
        return;
    }
    auto apply = [this, trackId, clipId](float volume, float pan) {
        setClipFxVolume(trackId, clipId, volume);
        setClipFxPan(trackId, clipId, pan);
    };
    m_undo.push(label,
                [apply, beforeVolume, beforePan] { apply(beforeVolume, beforePan); },
                [apply, afterVolume, afterPan] { apply(afterVolume, afterPan); });
}

std::vector<plugins::ParameterInfo> EngineController::insertParameters(
    const std::string& channelId, const std::string& insertId) const {
    auto* self = const_cast<EngineController*>(this);
    plugins::PluginNode* node = self->editorInsertNode(channelId, insertId);
    if (!node || !node->instance()) return {};
    const std::span<const plugins::ParameterInfo> parameters =
        node->instance()->parameters();
    return std::vector<plugins::ParameterInfo>(parameters.begin(), parameters.end());
}

double EngineController::insertParameter(const std::string& channelId,
                                         const std::string& insertId,
                                         const std::string& parameterId) const {
    auto* self = const_cast<EngineController*>(this);
    plugins::PluginNode* node = self->editorInsertNode(channelId, insertId);
    if (!node || !node->instance()) return 0.0;
    const std::int32_t index = node->instance()->parameterIndexForId(parameterId);
    if (index < 0) return 0.0;
    return node->instance()->parameterValue(std::uint32_t(index));
}

void EngineController::setInsertParameter(const std::string& channelId,
                                          const std::string& insertId,
                                          const std::string& parameterId,
                                          double plainValue) {
    plugins::PluginNode* node = editorInsertNode(channelId, insertId);
    if (!node || !node->instance()) return;
    const std::int32_t index = node->instance()->parameterIndexForId(parameterId);
    if (index < 0) return;

    // A timestamped event, not an atomic: the plugin applies it at a frame
    // offset inside the block, which is what makes a swept parameter smooth.
    plugins::PluginEvent event;
    event.kind = plugins::PluginEvent::Kind::ParamValue;
    event.paramIndex = std::uint32_t(index);
    event.value = plainValue;
    node->pushEvent(event);
    // The editor half has to be told separately — see setParameterFromHost.
    node->instance()->setParameterFromHost(std::uint32_t(index), plainValue);
    logParameterWrite("knob", node->instance(), index, plainValue);

    // Mirror into the document so a save right now records what is heard.
    // Through `mutableInsertSlot`, which finds the instrument as well: the
    // insert list alone silently dropped every knob on an instrument.
    if (InsertModel* slot = mutableInsertSlot(channelId, insertId)) {
        bool found = false;
        std::vector<InsertParameter>& stored =
            slot->channelMode == PluginChannelMode::DualMono &&
                    slot->editorChannel == PluginEditorChannel::Right
                ? slot->rightParameters
                : slot->parameters;
        for (InsertParameter& parameter : stored) {
            if (parameter.id != parameterId) continue;
            parameter.value = plainValue;
            found = true;
        }
        if (!found) stored.push_back(InsertParameter{parameterId, plainValue});
    }
}

void EngineController::commitInsertParameterEdit(const std::string& channelId,
                                                 const std::string& insertId,
                                                 const std::string& parameterId,
                                                 double beforeValue,
                                                 const std::string& label) {
    const double after = insertParameter(channelId, insertId, parameterId);
    if (after == beforeValue) return;
    const InsertModel* slot = insertModel(channelId, insertId);
    const bool right = slot && slot->channelMode == PluginChannelMode::DualMono &&
                       slot->editorChannel == PluginEditorChannel::Right;
    const auto shared = submitSharedMutation(
        collab::SetPluginParameter{channelPluginLocation(channelId), insertId,
                                   parameterId, after, right},
        label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked)
            setInsertParameter(channelId, insertId, parameterId, beforeValue);
        return;
    }
    m_undo.push(label,
                [this, channelId, insertId, parameterId, beforeValue] {
                    setInsertParameter(channelId, insertId, parameterId, beforeValue);
                },
                [this, channelId, insertId, parameterId, after] {
                    setInsertParameter(channelId, insertId, parameterId, after);
                });
}

plugins::PluginInstance* EngineController::insertInstance(
    const std::string& channelId, const std::string& insertId) {
    plugins::PluginNode* node = editorInsertNode(channelId, insertId);
    return node ? node->instance() : nullptr;
}

void EngineController::applyStoredParameters(
    plugins::PluginNode& node, const std::vector<InsertParameter>& values) {
    plugins::PluginInstance* instance = node.instance();
    if (!instance) return;
    for (const InsertParameter& parameter : values) {
        const std::int32_t index = instance->parameterIndexForId(parameter.id);
        if (index < 0) continue;
        plugins::PluginEvent event;
        event.kind = plugins::PluginEvent::Kind::ParamValue;
        event.paramIndex = std::uint32_t(index);
        event.value = parameter.value;
        node.pushEvent(event);
        instance->setParameterFromHost(std::uint32_t(index), parameter.value);
        logParameterWrite("restore", instance, index, parameter.value);
    }
}

// ── Channel strip clipboard ───────────────────────────────────────────────
//
// A chain is copied with the *sound* in it: each slot carries whatever its live
// plugin reports as its state, so pasting reproduces a tuned compressor rather
// than a fresh one with the same name.

EngineController::ChannelSnapshot EngineController::copyChannelStrip(
    const std::string& channelId, bool withSettings) {
    ChannelSnapshot snapshot;
    const std::vector<InsertModel>* slots = channelInserts(channelId);
    if (!slots) return snapshot;

    const bool master = channelId == kMasterChannelId;
    const TrackModel* track = master ? nullptr : m_project.findTrack(channelId);
    if (!master && !track) return snapshot;
    snapshot.sourceName = master ? "Master" : track->name;

    for (const InsertModel& slot : *slots) {
        ChainSlotSnapshot copied;
        copied.model = slot;
        if (plugins::PluginInstance* instance = insertInstance(channelId, slot.id)) {
            std::vector<std::uint8_t> chunk;
            if (instance->saveState(chunk)) copied.state = std::move(chunk);
        }
        if (InsertSlot* live = liveInsertSlot(channelId, slot.id);
            live && live->rightNode && live->rightNode->instance()) {
            std::vector<std::uint8_t> chunk;
            if (live->rightNode->instance()->saveState(chunk))
                copied.rightState = std::move(chunk);
        }
        snapshot.inserts.push_back(std::move(copied));
    }

    if (!withSettings) return snapshot;
    snapshot.hasSettings = true;
    if (master) {
        snapshot.volume = m_project.masterVolume;
        snapshot.pan = m_project.masterPan;
        return snapshot;
    }
    snapshot.volume = track->volume;
    snapshot.pan = track->pan;
    snapshot.muted = track->muted;
    snapshot.soloed = track->soloed;
    snapshot.mono = track->mono;
    snapshot.outputBusId = track->outputBusId;
    snapshot.sends = track->sends;
    return snapshot;
}

void EngineController::applyChain(const std::string& channelId,
                                  const std::vector<ChainSlotSnapshot>& chain) {
    std::vector<InsertModel>* slots = mutableChannelInserts(channelId);
    if (!slots) return;

    slots->clear();
    slots->reserve(chain.size());
    for (const ChainSlotSnapshot& slot : chain) slots->push_back(slot.model);
    // Retiring the old plugins and instantiating the new ones both happen here,
    // through the same reconciliation every other chain edit uses — an editor
    // window open on a slot that is going away is told before its plugin dies.
    rebuildGraph();

    // The instances came up at their defaults; hand them the state they were
    // copied at. Rendering is parked for it: a plugin rewriting its whole
    // parameter set underneath a process() call is exactly the race the reload
    // path already guards against.
    const engine::RealtimeEngine::RenderGate gate(m_engine);
    for (const ChainSlotSnapshot& slot : chain) {
        if (slot.state.empty() && slot.rightState.empty()) continue;
        InsertSlot* live = liveInsertSlot(channelId, slot.model.id);
        if (!live) continue;
        if (!slot.state.empty() && live->node && live->node->instance()) {
            (void)live->node->instance()->loadState(slot.state);
            // The document's own mirror of the knobs goes on top. A CLAP
            // plugin only learns of a parameter change when it next processes
            // a block, so a chain copied while nothing is rendering carries a
            // state chunk that predates the last few edits — and the mirror is
            // the one thing that is always current.
            applyStoredParameters(*live->node, slot.model.parameters);
        }
        if (!slot.rightState.empty() && live->rightNode &&
            live->rightNode->instance()) {
            (void)live->rightNode->instance()->loadState(slot.rightState);
            applyStoredParameters(*live->rightNode,
                                  slot.model.rightParameters.empty()
                                      ? slot.model.parameters
                                      : slot.model.rightParameters);
        }
    }
}

namespace {
/// A chain copied onto a channel: the same plugins and state, but slot ids of
/// its own. Ids are minted once, at the paste, so undo and redo keep putting
/// back the *same* slots instead of new ones each time.
std::vector<EngineController::ChainSlotSnapshot> mintChain(
    const std::vector<EngineController::ChainSlotSnapshot>& source) {
    std::vector<EngineController::ChainSlotSnapshot> chain = source;
    for (auto& slot : chain) slot.model.id = newUuid();
    return chain;
}
}  // namespace

bool EngineController::pasteChannelInserts(const std::string& channelId,
                                           const ChannelSnapshot& what) {
    const std::vector<InsertModel>* current = channelInserts(channelId);
    if (!current) return false;
    const ChannelSnapshot before = copyChannelStrip(channelId, /*withSettings=*/false);
    if (cloudProjectBound()) {
        if (before.inserts.empty() && what.inserts.empty()) return false;
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedChainReplacement(
                batch, channelPluginLocation(channelId), *current,
                what.inserts) ||
            !sharedBatchApplies(m_project, batch)) {
            return false;
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Paste Plugins");
        return result == collab::SharedMutationResult::Submitted;
    }
    const std::vector<ChainSlotSnapshot> next = mintChain(what.inserts);
    if (before.inserts.empty() && next.empty()) return false;

    applyChain(channelId, next);
    const std::vector<ChainSlotSnapshot> restore = before.inserts;
    m_undo.push("Paste Plugins",
                [this, channelId, restore] { applyChain(channelId, restore); },
                [this, channelId, next] { applyChain(channelId, next); });
    return true;
}

bool EngineController::pasteChannelStrip(const std::string& channelId,
                                         const ChannelSnapshot& what) {
    if (!what.hasSettings) return pasteChannelInserts(channelId, what);
    const std::vector<InsertModel>* current = channelInserts(channelId);
    if (!current) return false;

    const bool master = channelId == kMasterChannelId;
    if (!master && !m_project.findTrack(channelId)) return false;

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedChainReplacement(
                batch, channelPluginLocation(channelId), *current,
                what.inserts)) {
            return false;
        }
        if (master) {
            appendCommand(batch, collab::SetProjectScalar{
                collab::ProjectScalar::MasterVolume,
                double(std::clamp(what.volume, 0.0f, 2.0f))});
            appendCommand(batch, collab::SetProjectScalar{
                collab::ProjectScalar::MasterPan,
                double(std::clamp(what.pan, -1.0f, 1.0f))});
        } else {
            const TrackModel* target = m_project.findTrack(channelId);
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Volume,
                double(std::clamp(what.volume, 0.0f, 2.0f))});
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Pan,
                double(std::clamp(what.pan, -1.0f, 1.0f))});
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Muted, what.muted});
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Mono, what.mono});
            const bool routable = !what.outputBusId.empty() &&
                                  what.outputBusId != channelId &&
                                  m_project.findTrack(what.outputBusId);
            appendCommand(batch, collab::SetTrackOutput{
                channelId, routable ? what.outputBusId : std::string()});
            for (const SendModel& send : target->sends) {
                appendCommand(batch,
                              collab::DeleteSend{channelId, send.id});
            }
            std::string anchor;
            for (SendModel send : what.sends) {
                if (send.destinationTrackId == channelId ||
                    !m_project.findTrack(send.destinationTrackId)) {
                    continue;
                }
                send.id = newUuid();
                appendCommand(batch,
                              collab::AddSend{channelId, send, anchor});
                anchor = send.id;
            }
        }
        if (!sharedBatchApplies(m_project, batch)) return false;
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Paste Channel Strip");
        return result == collab::SharedMutationResult::Submitted;
    }

    const ChannelSnapshot before = copyChannelStrip(channelId, /*withSettings=*/true);
    const std::vector<ChainSlotSnapshot> next = mintChain(what.inserts);

    // Sends get ids of their own for the same reason the slots do.
    ChannelSnapshot applied = what;
    applied.inserts = next;
    for (SendModel& send : applied.sends) send.id = newUuid();

    auto put = [this, channelId, master](const ChannelSnapshot& state) {
        applyChain(channelId, state.inserts);
        if (master) {
            m_project.masterVolume = std::clamp(state.volume, 0.0f, 2.0f);
            m_project.masterPan = std::clamp(state.pan, -1.0f, 1.0f);
            if (m_masterFader) m_masterFader->setGain(m_project.masterVolume);
            rebuildGraph();
            return;
        }
        TrackModel* track = m_project.findTrack(channelId);
        if (!track) return;
        track->volume = std::clamp(state.volume, 0.0f, 2.0f);
        track->pan = std::clamp(state.pan, -1.0f, 1.0f);
        track->muted = state.muted;
        track->soloed = state.soloed;
        track->mono = state.mono;
        // A strip pasted onto a track must not route that track into itself,
        // and a bus that no longer exists is "straight to master".
        const bool routable = !state.outputBusId.empty() &&
                              state.outputBusId != channelId &&
                              m_project.findTrack(state.outputBusId) != nullptr;
        track->outputBusId = routable ? state.outputBusId : std::string();
        track->sends.clear();
        for (const SendModel& send : state.sends) {
            if (send.destinationTrackId == channelId) continue;   // no self-send
            if (!m_project.findTrack(send.destinationTrackId)) continue;
            track->sends.push_back(send);
        }
        // A pasted routing can close a loop; put it back if it does.
        if (!rebuildGraph()) {
            track->outputBusId.clear();
            track->sends.clear();
            rebuildGraph();
        }
        // Solo is a project-wide state — pasting one onto a strip changes what
        // every other channel is allowed to make.
        syncAllTrackGains();
    };

    put(applied);
    m_undo.push("Paste Channel Strip",
                [put, before] { put(before); },
                [put, applied] { put(applied); });
    return true;
}

bool EngineController::pasteChannelStripPreset(const std::string& channelId,
                                               const ChannelSnapshot& what) {
    const std::vector<InsertModel>* current = channelInserts(channelId);
    if (!what.hasSettings || !current) return false;

    const bool master = channelId == kMasterChannelId;
    if (!master && !m_project.findTrack(channelId)) return false;

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedChainReplacement(
                batch, channelPluginLocation(channelId), *current,
                what.inserts)) {
            return false;
        }
        if (master) {
            appendCommand(batch, collab::SetProjectScalar{
                collab::ProjectScalar::MasterVolume,
                double(std::clamp(what.volume, 0.0f, 2.0f))});
            appendCommand(batch, collab::SetProjectScalar{
                collab::ProjectScalar::MasterPan,
                double(std::clamp(what.pan, -1.0f, 1.0f))});
        } else {
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Volume,
                double(std::clamp(what.volume, 0.0f, 2.0f))});
            appendCommand(batch, collab::SetTrackProperty{
                channelId, collab::TrackProperty::Pan,
                double(std::clamp(what.pan, -1.0f, 1.0f))});
        }
        if (!sharedBatchApplies(m_project, batch)) return false;
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            "Apply Channel Strip Preset");
        return result == collab::SharedMutationResult::Submitted;
    }

    const ChannelSnapshot before =
        copyChannelStrip(channelId, /*withSettings=*/true);
    ChannelSnapshot applied = what;
    applied.inserts = mintChain(what.inserts);

    // This is intentionally smaller than pasteChannelStrip's `put`: a VLTS
    // preset is a sound, not a routing command. All destination flags, routing
    // and sends stay exactly as they were.
    auto put = [this, channelId, master](const ChannelSnapshot& state) {
        applyChain(channelId, state.inserts);
        if (master) {
            m_project.masterVolume = std::clamp(state.volume, 0.0f, 2.0f);
            m_project.masterPan = std::clamp(state.pan, -1.0f, 1.0f);
            if (m_masterFader) {
                m_masterFader->setGain(m_project.masterVolume);
                m_masterFader->setPan(m_project.masterPan);
            }
            return;
        }
        TrackModel* track = m_project.findTrack(channelId);
        if (!track) return;
        track->volume = std::clamp(state.volume, 0.0f, 2.0f);
        track->pan = std::clamp(state.pan, -1.0f, 1.0f);
        syncTrackGain(*track);
    };

    put(applied);
    m_undo.push("Apply Channel Strip Preset",
                [put, before] { put(before); },
                [put, applied] { put(applied); });
    return true;
}

audio::Result EngineController::saveChannelStripPreset(
    const std::string& channelId, const std::string& filePath) {
    ChannelSnapshot snapshot =
        copyChannelStrip(channelId, /*withSettings=*/true);
    if (!snapshot.hasSettings) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "channel does not exist");
    }
    return ChannelStripPreset::save(snapshot, filePath);
}

audio::Result EngineController::applyChannelStripPreset(
    const std::string& channelId, const std::string& filePath) {
    ChannelSnapshot preset;
    audio::Result loaded = ChannelStripPreset::load(preset, filePath);
    if (!loaded) return loaded;
    if (!pasteChannelStripPreset(channelId, preset)) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "destination channel does not exist");
    }
    return audio::Result::ok();
}

bool EngineController::moveInsertBetweenChannels(const std::string& fromChannel,
                                                 const std::string& insertId,
                                                 const std::string& toChannel,
                                                 size_t index, bool copy) {
    if (fromChannel == toChannel) return false;
    if (!mutableChannelInserts(fromChannel) || !mutableChannelInserts(toChannel))
        return false;

    const ChannelSnapshot source = copyChannelStrip(fromChannel, false);
    const ChannelSnapshot target = copyChannelStrip(toChannel, false);
    const auto moved = std::find_if(
        source.inserts.begin(), source.inserts.end(),
        [&](const ChainSlotSnapshot& slot) { return slot.model.id == insertId; });
    if (moved == source.inserts.end()) return false;

    if (cloudProjectBound()) {
        const std::vector<InsertModel>* destination = channelInserts(toChannel);
        if (!destination) return false;
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!copy) {
            appendCommand(batch, collab::DeletePluginInsert{
                channelPluginLocation(fromChannel), insertId});
        }
        InsertModel clean;
        if (!appendSharedPluginCopy(
                batch, channelPluginLocation(toChannel), moved->model,
                previousIdAt(*destination,
                             std::min(index, destination->size())),
                &clean) ||
            !sharedBatchApplies(m_project, batch)) {
            return false;
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            copy ? "Copy Plugin" : "Move Plugin");
        return result == collab::SharedMutationResult::Submitted;
    }

    std::vector<ChainSlotSnapshot> nextTarget = target.inserts;
    ChainSlotSnapshot landed = *moved;
    landed.model.id = newUuid();     // a slot id belongs to one chain only
    nextTarget.insert(nextTarget.begin() +
                          std::ptrdiff_t(std::min(index, nextTarget.size())),
                      landed);

    std::vector<ChainSlotSnapshot> nextSource = source.inserts;
    if (!copy) {
        nextSource.erase(nextSource.begin() +
                         std::ptrdiff_t(moved - source.inserts.begin()));
    }

    auto put = [this, fromChannel, toChannel](
                   const std::vector<ChainSlotSnapshot>& from,
                   const std::vector<ChainSlotSnapshot>& to) {
        applyChain(fromChannel, from);
        applyChain(toChannel, to);
    };
    put(nextSource, nextTarget);
    const std::vector<ChainSlotSnapshot> beforeSource = source.inserts;
    const std::vector<ChainSlotSnapshot> beforeTarget = target.inserts;
    m_undo.push(copy ? "Copy Plugin" : "Move Plugin",
                [put, beforeSource, beforeTarget] { put(beforeSource, beforeTarget); },
                [put, nextSource, nextTarget] { put(nextSource, nextTarget); });
    return true;
}

bool EngineController::copySendsTo(const std::string& fromTrackId,
                                   const std::string& toTrackId, bool move) {
    TrackModel* from = m_project.findTrack(fromTrackId);
    TrackModel* to = m_project.findTrack(toTrackId);
    if (!from || !to || from == to) return false;

    const std::vector<SendModel> beforeTo = to->sends;
    const std::vector<SendModel> beforeFrom = from->sends;

    std::vector<SendModel> next;
    for (const SendModel& send : beforeFrom) {
        if (send.destinationTrackId == toTrackId) continue;   // never into itself
        SendModel copy = send;
        copy.id = newUuid();
        next.push_back(copy);
    }
    if (next.empty() && !move) return false;

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const SendModel& send : beforeTo) {
            appendCommand(batch,
                          collab::DeleteSend{toTrackId, send.id});
        }
        if (move) {
            for (const SendModel& send : beforeFrom) {
                appendCommand(batch,
                              collab::DeleteSend{fromTrackId, send.id});
            }
        }
        std::string anchor;
        for (const SendModel& send : next) {
            appendCommand(batch, collab::AddSend{toTrackId, send, anchor});
            anchor = send.id;
        }
        if (batch->commands.empty() || !sharedBatchApplies(m_project, batch))
            return false;
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            move ? "Move Sends" : "Copy Sends");
        return result == collab::SharedMutationResult::Submitted;
    }

    auto put = [this, fromTrackId, toTrackId](const std::vector<SendModel>& source,
                                              const std::vector<SendModel>& target) {
        if (TrackModel* a = m_project.findTrack(fromTrackId)) a->sends = source;
        if (TrackModel* b = m_project.findTrack(toTrackId)) b->sends = target;
        if (!rebuildGraph()) {
            if (TrackModel* b = m_project.findTrack(toTrackId)) b->sends.clear();
            rebuildGraph();
        }
    };

    put(move ? std::vector<SendModel>{} : beforeFrom, next);
    m_undo.push(move ? "Move Sends" : "Copy Sends",
                [put, beforeFrom, beforeTo] { put(beforeFrom, beforeTo); },
                [put, move, beforeFrom, next] {
                    put(move ? std::vector<SendModel>{} : beforeFrom, next);
                });
    return true;
}

plugins::sampler::SamplerInstance* EngineController::samplerInstance(
    const std::string& channelId, const std::string& slotId) {
    return dynamic_cast<plugins::sampler::SamplerInstance*>(
        insertInstance(channelId, slotId));
}

bool EngineController::loadSamplerSample(const std::string& channelId,
                                         const std::string& slotId,
                                         const std::string& filePath) {
    if (cloudProjectBound()) {
        const TrackModel* track = m_project.findTrack(channelId);
        if (!track || track->instrument.id != slotId ||
            track->instrument.uid != "daw.sampler" ||
            track->instrument.format != PluginFormat::Internal) {
            return false;
        }
        auto request = sharedAudioRequest(filePath);
        if (!request) return false;

        PendingSharedAssetMutation pending;
        pending.expected = expectedSharedAudioAsset(*request);
        pending.complete =
            [channelId, slotId](EngineController& controller,
                                const AssetRef& verifiedAsset) {
                const TrackModel* current =
                    controller.m_project.findTrack(channelId);
                if (!current || current->instrument.id != slotId ||
                    current->instrument.uid != "daw.sampler") {
                    return collab::SharedMutationResult::Blocked;
                }
                return controller.submitSharedMutation(
                    collab::SetPluginAssetBinding{
                        {collab::PluginChain::Instrument, channelId, {}},
                        slotId,
                        PluginAssetBinding{"sample", verifiedAsset, true}},
                    "Load Sample");
            };
        return prepareSharedAssetMutation(std::move(*request),
                                          std::move(pending)) ==
               collab::SharedMutationResult::Submitted;
    }
    plugins::sampler::SamplerInstance* sampler = samplerInstance(channelId, slotId);
    if (!sampler) return false;

    const std::string previous = sampler->samplePath();
    if (!sampler->loadSample(filePath)) return false;

    // The waveform the panel draws comes from the instance, but the arrangement
    // and the browser share one peak cache — priming it here keeps a later
    // paint off the file system.
    m_waveforms.peaks(filePath);

    m_undo.push("Load Sample",
                [this, channelId, slotId, previous] {
                    if (previous.empty()) {
                        clearSamplerSample(channelId, slotId);
                    } else {
                        loadSamplerSampleSilently(channelId, slotId, previous);
                    }
                },
                [this, channelId, slotId, filePath] {
                    loadSamplerSampleSilently(channelId, slotId, filePath);
                });
    return true;
}

bool EngineController::loadInstrumentSampler(const std::string& trackId,
                                             const std::string& filePath) {
    const TrackModel* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Midi)) return false;

    const auto sampler =
        m_pluginManager.find(plugins::Format::Internal, "daw.sampler");
    if (!sampler) return false;

    if (cloudProjectBound()) {
        auto request = sharedAudioRequest(filePath);
        if (!request) return false;
        const InsertModel before = track->instrument;
        if (!before.id.empty() &&
            (before.format != PluginFormat::Internal ||
             !supportedSharedBuiltin(before))) {
            return false;
        }

        PendingSharedAssetMutation pending;
        pending.expected = expectedSharedAudioAsset(*request);
        if (before.uid == "daw.sampler") {
            const std::string slotId = before.id;
            pending.complete =
                [trackId, slotId](EngineController& controller,
                                  const AssetRef& verifiedAsset) {
                    const TrackModel* current =
                        controller.m_project.findTrack(trackId);
                    if (!current || current->instrument.id != slotId ||
                        current->instrument.uid != "daw.sampler") {
                        return collab::SharedMutationResult::Blocked;
                    }
                    return controller.submitSharedMutation(
                        collab::SetPluginAssetBinding{
                            {collab::PluginChain::Instrument, trackId, {}},
                            slotId,
                            PluginAssetBinding{"sample", verifiedAsset, true}},
                        "Load Sampler with File");
                };
        } else {
            InsertModel candidate;
            candidate.id = before.id.empty() ? newUuid() : before.id;
            candidate.bypassed = before.bypassed;
            applyDescriptor(candidate, *sampler);
            InsertModel replacement;
            if (!cleanSharedInsert(candidate, replacement, false) ||
                replacement.uid != "daw.sampler") {
                return false;
            }
            pending.complete =
                [trackId, beforeId = before.id,
                 replacement = std::move(replacement)](
                    EngineController& controller,
                    const AssetRef& verifiedAsset) mutable {
                    const TrackModel* current =
                        controller.m_project.findTrack(trackId);
                    if (!current || current->instrument.id != beforeId)
                        return collab::SharedMutationResult::Blocked;
                    setSharedSampleBinding(replacement, verifiedAsset);
                    const collab::PluginLocation location{
                        collab::PluginChain::Instrument, trackId, {}};
                    collab::CommandBody body;
                    if (beforeId.empty()) {
                        body = collab::AddPluginInsert{location, replacement,
                                                       {}};
                    } else {
                        body = collab::ReplacePluginInsert{
                            location, beforeId, replacement};
                    }
                    return controller.submitSharedMutation(
                        std::move(body), "Load Sampler with File");
                };
        }
        return prepareSharedAssetMutation(std::move(*request),
                                          std::move(pending)) ==
               collab::SharedMutationResult::Submitted;
    }

    const InsertModel before = track->instrument;
    const SamplerFxModel beforeFx = track->samplerFx;
    // Both halves under one Suspend, then a single entry pushed by hand: the
    // two steps are one gesture, and `Suspend` is the documented way to keep
    // the pieces off the stack while they happen.
    {
        UndoStack::Suspend quiet(m_undo);
        setTrackInstrumentPlugin(trackId, *sampler);
        const TrackModel* updated = m_project.findTrack(trackId);
        if (!updated || !updated->instrument.isLoaded()) return false;
        if (!loadSamplerSample(trackId, updated->instrument.id, filePath)) {
            // Put the slot back rather than leaving an empty sampler behind.
            if (auto* revert = m_project.findTrack(trackId)) {
                revert->instrument = before;
                revert->samplerFx = beforeFx;
                rebuildGraph();
            }
            return false;
        }
    }

    const TrackModel* after = m_project.findTrack(trackId);
    const InsertModel loaded = after ? after->instrument : InsertModel{};
    const SamplerFxModel loadedFx = after ? after->samplerFx : SamplerFxModel{};
    const std::string slotId = loaded.id;
    m_undo.push("Load Sampler with File",
                [this, trackId, before, beforeFx] {
                    if (auto* target = m_project.findTrack(trackId)) {
                        target->instrument = before;
                        target->samplerFx = beforeFx;
                        rebuildGraph();
                    }
                },
                [this, trackId, loaded, loadedFx, slotId, filePath] {
                    if (auto* target = m_project.findTrack(trackId)) {
                        target->instrument = loaded;
                        target->samplerFx = loadedFx;
                        rebuildGraph();
                        loadSamplerSampleSilently(trackId, slotId, filePath);
                    }
                });
    return true;
}

void EngineController::loadSamplerSampleSilently(const std::string& channelId,
                                                 const std::string& slotId,
                                                 const std::string& filePath) {
    if (plugins::sampler::SamplerInstance* sampler = samplerInstance(channelId, slotId)) {
        sampler->loadSample(filePath);
    }
}

void EngineController::clearSamplerSampleSilently(
    const std::string& channelId, const std::string& slotId) {
    if (plugins::sampler::SamplerInstance* sampler =
            samplerInstance(channelId, slotId)) {
        sampler->clearSample();
    }
}

void EngineController::clearSamplerSample(const std::string& channelId,
                                          const std::string& slotId) {
    if (cloudProjectBound()) {
        const TrackModel* track = m_project.findTrack(channelId);
        if (!track || track->instrument.id != slotId ||
            track->instrument.uid != "daw.sampler") {
            return;
        }
        const bool hasSample = std::any_of(
            track->instrument.assetBindings.begin(),
            track->instrument.assetBindings.end(),
            [](const PluginAssetBinding& binding) {
                return binding.key == "sample";
            });
        if (!hasSample) return;
        (void)submitSharedMutation(
            collab::RemovePluginAssetBinding{
                {collab::PluginChain::Instrument, channelId, {}}, slotId,
                "sample"},
            "Clear Sample");
        return;
    }
    plugins::sampler::SamplerInstance* sampler = samplerInstance(channelId, slotId);
    if (!sampler) return;
    const std::string previous = sampler->samplePath();
    if (previous.empty()) return;
    sampler->clearSample();

    m_undo.push("Clear Sample",
                [this, channelId, slotId, previous] {
                    loadSamplerSampleSilently(channelId, slotId, previous);
                },
                [this, channelId, slotId] { clearSamplerSample(channelId, slotId); });
}

bool EngineController::pumpPluginEvents() {
    // The control-thread turn. Clip effects still synchronize here; built-in
    // sampler pumps below only coalesce and queue their background bakes.
    flushDeferredClipSync();

    const std::uint64_t requestedGeneration =
        plugins::PluginMainThreadWork::generation();
    const bool compatibilitySweep =
        ++m_pluginCompatibilitySweepTicks >= kPluginCompatibilitySweepTicks;
    if (requestedGeneration == m_pluginMainThreadGeneration &&
        !compatibilitySweep) {
        // Steady playback lands here: no channel/clip/slot traversal and no
        // virtual calls for VST3's empty pumpMainThread implementation.
        return false;
    }
    // Capture before draining. A callback that requests another turn from
    // inside pumpMainThread advances the generation and is seen next tick.
    m_pluginMainThreadGeneration = requestedGeneration;
    // Count idle checks since the last full scan, not absolute timer ticks.
    // Otherwise real work on tick 63 would be followed by a redundant
    // compatibility sweep on the very next tick.
    m_pluginCompatibilitySweepTicks = 0;
    ++m_pluginEventScanCount;

    bool changed = false;
    bool needsRebuild = false;
    bool needsReconfigure = false;

    auto pumpSlot = [&](const std::string& channelId, InsertSlot& slot) {
        plugins::PluginEvent event;
        if (slot.node) {
            slot.node->beginMainThreadPump();

            if (slot.node->takeReloadRequested()) {
                // kReloadComponent means unload/recreate, not re-activate the
                // same COM object. Saving state must not overlap process(), so
                // briefly park rendering; the published graph then keeps the
                // old node alive until the replacement snapshot lands.
                std::vector<std::uint8_t> state;
                {
                    const engine::RealtimeEngine::RenderGate gate(m_engine);
                    if (plugins::PluginInstance* instance = slot.node->instance()) {
                        (void)instance->saveState(state);
                    }
                }
                slot.reloadState = std::move(state);
                slot.node.reset();
                needsRebuild = true;
                needsReconfigure = true;
                // Do not return: the right half of a dual-mono slot may have
                // published work in this same generation.
            } else {
                // A plugin whose latency moved (a preset load, usually) needs
                // the graph recompiled or delay compensation silently goes
                // stale.
                if (slot.node->takeLatencyChanged()) {
                    slot.node->invalidatePrepare();
                    needsRebuild = true;
                    needsReconfigure = true;
                }
                if (slot.node->takeRestartRequested()) {
                    slot.node->invalidatePrepare();
                    needsRebuild = true;
                    needsReconfigure = true;
                }

                // Plugins queue work for the main thread whether or not an
                // editor is open, and a GUI that never gets its turn freezes.
                // The sampler uses this to enqueue its latest precompute.
                if (plugins::PluginInstance* instance = slot.node->instance()) {
                    instance->pumpMainThread();
                }

                while (slot.node->popNotification(event)) {
                    if (event.kind != plugins::PluginEvent::Kind::ParamValue)
                        continue;
                    // The plugin moved this itself, in its own editor. Record
                    // it so the document matches what is being heard.
                    plugins::PluginInstance* instance = slot.node->instance();
                    if (!instance) continue;
                    const std::span<const plugins::ParameterInfo> parameters =
                        instance->parameters();
                    if (event.paramIndex >= parameters.size()) continue;
                    const std::string& parameterId =
                        parameters[event.paramIndex].id;

                    if (InsertModel* model =
                            mutableInsertSlot(channelId, slot.slotId)) {
                        bool found = false;
                        for (InsertParameter& parameter : model->parameters) {
                            if (parameter.id != parameterId) continue;
                            parameter.value = event.value;
                            found = true;
                        }
                        if (!found) {
                            model->parameters.push_back(
                                InsertParameter{parameterId, event.value});
                        }
                    }
                    writeAutomationPoint(channelId, slot.slotId, parameterId,
                                         event.value);
                    changed = true;
                }
            }
        }

        if (!slot.rightNode) return;
        slot.rightNode->beginMainThreadPump();
        if (slot.rightNode->takeReloadRequested()) {
            std::vector<std::uint8_t> state;
            {
                const engine::RealtimeEngine::RenderGate gate(m_engine);
                if (plugins::PluginInstance* instance =
                        slot.rightNode->instance()) {
                    (void)instance->saveState(state);
                }
            }
            slot.rightReloadState = std::move(state);
            slot.rightNode.reset();
            needsRebuild = true;
            needsReconfigure = true;
            return;
        }
        // Consume both latches independently. Short-circuiting here could
        // leave restart pending after this node's global wake was consumed,
        // delaying it until the compatibility sweep.
        const bool rightLatencyChanged = slot.rightNode->takeLatencyChanged();
        const bool rightRestartRequested = slot.rightNode->takeRestartRequested();
        if (rightLatencyChanged || rightRestartRequested) {
            slot.rightNode->invalidatePrepare();
            needsRebuild = true;
            needsReconfigure = true;
        }
        if (plugins::PluginInstance* instance = slot.rightNode->instance()) {
            instance->pumpMainThread();
        }
        while (slot.rightNode->popNotification(event)) {
            if (event.kind != plugins::PluginEvent::Kind::ParamValue) continue;
            plugins::PluginInstance* instance = slot.rightNode->instance();
            if (!instance) continue;
            const auto parameters = instance->parameters();
            if (event.paramIndex >= parameters.size()) continue;
            const std::string& parameterId = parameters[event.paramIndex].id;
            if (InsertModel* model = mutableInsertSlot(channelId, slot.slotId)) {
                bool found = false;
                for (InsertParameter& parameter : model->rightParameters) {
                    if (parameter.id != parameterId) continue;
                    parameter.value = event.value;
                    found = true;
                }
                if (!found) {
                    model->rightParameters.push_back(
                        InsertParameter{parameterId, event.value});
                }
            }
            changed = true;
        }
    };

    for (auto& entry : m_channels) {
        // The instrument as well as the inserts — it is a plugin slot too, and
        // one that never got its main-thread turn before this.
        for (InsertSlot& slot : entry.second.instrument) pumpSlot(entry.first, slot);
        for (InsertSlot& slot : entry.second.samplerInserts) pumpSlot(entry.first, slot);
        for (auto& [clipId, clipFx] : entry.second.clipFx) {
            (void)clipId;
            for (InsertSlot& slot : clipFx.inserts) pumpSlot(entry.first, slot);
        }
        for (InsertSlot& slot : entry.second.inserts) pumpSlot(entry.first, slot);
    }

    if (needsRebuild) {
        rebuildGraph(needsReconfigure);
        changed = true;
    }
    return changed;
}

// ── Clips ──────────────────────────────────────────────────────────────────

std::string EngineController::importAudioToNewTrack(
    const std::string& filePath, double startSeconds,
    const std::string& trackName,
    const ClipMusicalAnalysisModel& analysis) {
    if (filePath.empty()) return {};

    if (cloudProjectBound()) {
        auto request = sharedAudioRequest(filePath);
        if (!request) return {};

        std::string name = trackName;
        if (name.empty()) {
            name = platform::pathToUtf8(
                platform::pathFromUtf8(filePath).stem());
        }
        if (name.empty()) name = "Audio";

        TrackModel track;
        track.id = newUuid();
        track.kind = TrackKind::Audio;
        track.name = std::move(name);
        track.color = defaultTrackColor(track.kind);
        ClipModel clip;
        clip.id = newUuid();
        clip.kind = ClipKind::Audio;
        clip.name = platform::pathToUtf8(
            platform::pathFromUtf8(filePath).stem());
        if (clip.name.empty()) clip.name = "Audio";
        clip.startSeconds = startSeconds;
        clip.durationSeconds = double(request->frames) / request->sampleRate;
        clip.channels = int(request->channels);
        clip.color = track.color;
        clip.musicalAnalysis = analysis;
        // Empty on purpose: the track and its clip go out now, the verified
        // asset follows. See submitOptimisticSharedAudioTrack.
        clip.asset = {};
        track.clips.push_back(clip);
        const std::string afterId = m_project.tracks.empty()
            ? std::string()
            : m_project.tracks.back().id;
        return submitOptimisticSharedAudioTrack(std::move(*request),
                                                std::move(track), afterId);
    }

    const std::size_t undoStart = m_undo.depth();
    std::string name = trackName;
    if (name.empty()) {
        name = platform::pathToUtf8(platform::pathFromUtf8(filePath).stem());
    }
    if (name.empty()) name = "Audio";

    const std::string trackId = addTrack(TrackKind::Audio, name);
    if (trackId.empty()) return {};
    if (importAudio(filePath, trackId, startSeconds, analysis).empty()) {
        // Roll back the half-finished operation without adding a compensating
        // Remove Track entry. The caller should see exactly what it saw before
        // the failed import, including the undo label/depth.
        {
            UndoStack::Suspend quiet(m_undo);
            removeTrack(trackId);
        }
        m_undo.discardSince(undoStart);
        return {};
    }

    collapseUndo(undoStart, "Import Audio to New Track");
    return trackId;
}

std::string EngineController::importAudio(const std::string& filePath,
                                          const std::string& trackId,
                                          double startSeconds,
                                          const ClipMusicalAnalysisModel& analysis) {
    auto* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Audio)) return {};

    if (cloudProjectBound()) {
        auto request = sharedAudioRequest(filePath);
        if (!request) return {};

        ClipModel clip;
        clip.id = newUuid();
        clip.kind = ClipKind::Audio;
        clip.name = platform::pathToUtf8(
            platform::pathFromUtf8(filePath).stem());
        if (clip.name.empty()) clip.name = "Audio";
        clip.startSeconds = startSeconds;
        clip.durationSeconds = double(request->frames) / request->sampleRate;
        clip.channels = int(request->channels);
        clip.color = track->color;
        clip.musicalAnalysis = analysis;
        // Deliberately left empty: appendSharedClip omits clip.setAsset for an
        // empty ref, which is what makes the clip submittable before its bytes
        // exist anywhere. The asset arrives as its own operation below.
        clip.asset = {};
        const std::string afterId = track->clips.empty()
            ? std::string()
            : track->clips.back().id;
        return submitOptimisticSharedAudioClip(std::move(*request), trackId,
                                               std::move(clip), afterId,
                                               "Import Audio");
    }

    auto samples = loadSamples(filePath);
    if (!samples) return {};
    // Decode the peak envelope now, so the arrangement can draw the waveform
    // without touching the file from its paint handler.
    m_waveforms.peaks(filePath);

    ClipModel clip;
    clip.id = newUuid();
    clip.name = platform::pathToUtf8(platform::pathFromUtf8(filePath).stem());
    clip.filePath = filePath;
    clip.startSeconds = startSeconds;
    clip.durationSeconds =
        samples->sampleRate() > 0.0
            ? double(samples->frames()) / samples->sampleRate()
            : 0.0;
    clip.channels = int(samples->channels());
    clip.color = track->color;
    clip.musicalAnalysis = analysis;
    track->clips.push_back(clip);

    syncTrackClips(*track);
    updateTimelineDuration();

    const std::string clipUuid = clip.id;
    m_undo.push("Import Audio",
                [this, trackId, clipUuid] { removeClip(trackId, clipUuid); },
                [this, trackId, clip] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->clips.push_back(clip);
                        syncTrackClips(*t);
                        updateTimelineDuration();
                    }
                });
    return clipUuid;
}

void EngineController::setClipStartSeconds(const std::string& trackId,
                                           const std::string& clipId,
                                           double startSeconds) {
    const ClipStartChange change{trackId, clipId, startSeconds};
    setClipStartsSeconds(std::span<const ClipStartChange>(&change, 1));
}

void EngineController::beginClipPositionEdit() {
    // A second UI gesture means the first one lost its matching release. Flush
    // its endpoint before opening another transaction rather than leaving the
    // realtime graph indefinitely behind the document.
    if (m_clipPositionEdit.active) endClipPositionEdit();
    m_clipPositionEdit = {};
    m_clipPositionEdit.active = true;
}

void EngineController::endClipPositionEdit(const std::string& label) {
    if (!m_clipPositionEdit.active) return;
    ClipPositionEdit edit = std::move(m_clipPositionEdit);
    m_clipPositionEdit = {};

    struct PositionDelta {
        std::string clipId;
        std::string beforeTrackId;
        std::string afterTrackId;
        double beforeStartSeconds = 0.0;
        double afterStartSeconds = 0.0;
        std::size_t beforeIndex = 0;
        std::size_t afterIndex = 0;
    };

    // Resolve only the clips touched by the gesture. The history payload is a
    // pair of scalar placements per clip; in particular it never owns a
    // ClipModel (and therefore never duplicates a dense note/sample payload).
    std::unordered_set<std::string> finalOwnerIds;
    finalOwnerIds.reserve(edit.origins.size());
    for (const auto& [clipId, origin] : edit.origins) {
        (void)clipId;
        finalOwnerIds.insert(origin.afterTrackId);
    }
    std::unordered_set<std::string> foundAtEndpoint;
    foundAtEndpoint.reserve(edit.origins.size());
    for (const std::string& trackId : finalOwnerIds) {
        const TrackModel* track = m_project.findTrack(trackId);
        if (!track) continue;
        for (std::size_t index = 0; index < track->clips.size(); ++index) {
            const ClipModel& clip = track->clips[index];
            auto origin = edit.origins.find(clip.id);
            if (origin == edit.origins.end() ||
                origin->second.afterTrackId != trackId) {
                continue;
            }
            origin->second.afterStartSeconds = clip.startSeconds;
            origin->second.afterIndex = index;
            foundAtEndpoint.insert(clip.id);
        }
    }

    std::vector<PositionDelta> built;
    built.reserve(edit.origins.size());
    for (const auto& [clipId, origin] : edit.origins) {
        if (!foundAtEndpoint.contains(clipId)) continue;
        if (origin.beforeTrackId == origin.afterTrackId &&
            origin.beforeIndex == origin.afterIndex &&
            std::abs(origin.beforeStartSeconds - origin.afterStartSeconds) <
                1e-12) {
            continue;
        }
        built.push_back(PositionDelta{
            clipId, origin.beforeTrackId, origin.afterTrackId,
            origin.beforeStartSeconds, origin.afterStartSeconds,
            origin.beforeIndex, origin.afterIndex});
    }
    if (built.empty()) return;

    const auto delta =
        std::make_shared<const std::vector<PositionDelta>>(std::move(built));
    auto apply = [this, delta](bool useAfter, bool publishAudio) {
        std::unordered_map<std::string, const PositionDelta*> desired;
        desired.reserve(delta->size());
        bool durationChanged = false;
        for (const PositionDelta& change : *delta) {
            desired.emplace(change.clipId, &change);
            durationChanged |=
                std::abs(change.beforeStartSeconds -
                         change.afterStartSeconds) >= 1e-12;
        }

        std::unordered_set<std::string> audioTracks;
        std::unordered_set<std::string> midiTracks;
        std::vector<AutomationTarget> automationTargets;
        automationTargets.reserve(delta->size());
        struct PendingPlacement {
            std::size_t index = 0;
            ClipModel clip;
        };
        std::unordered_map<std::string, std::vector<PendingPlacement>> pending;
        std::unordered_map<std::string, TrackModel*> owners;
        owners.reserve(delta->size() * 2);
        for (const PositionDelta& change : *delta) {
            if (!owners.contains(change.beforeTrackId))
                owners.emplace(change.beforeTrackId,
                               m_project.findTrack(change.beforeTrackId));
            if (!owners.contains(change.afterTrackId))
                owners.emplace(change.afterTrackId,
                               m_project.findTrack(change.afterTrackId));
        }
        bool graphDirty = false;

        // Walk only owners named by the delta, moving ClipModel objects rather
        // than copying them. Moving a vector-bearing ClipModel preserves the
        // allocation of its notes, samples and lanes even for a cross-track
        // undo/redo.
        for (const auto& [ownerId, owner] : owners) {
            (void)ownerId;
            if (!owner) continue;
            TrackModel& track = *owner;
            std::size_t index = 0;
            while (index < track.clips.size()) {
                ClipModel& clip = track.clips[index];
                const auto wanted = desired.find(clip.id);
                if (wanted == desired.end()) {
                    ++index;
                    continue;
                }

                const PositionDelta& change = *wanted->second;
                const std::string sourceTrackId = track.id;
                const std::string& targetTrackId =
                    useAfter ? change.afterTrackId : change.beforeTrackId;
                const double targetStartSeconds =
                    useAfter ? change.afterStartSeconds
                             : change.beforeStartSeconds;
                const std::size_t targetIndex =
                    useAfter ? change.afterIndex : change.beforeIndex;
                const auto targetOwner = owners.find(targetTrackId);
                TrackModel* targetTrack =
                    targetOwner == owners.end() ? nullptr : targetOwner->second;
                if (!targetTrack ||
                    !trackAccepts(targetTrack->kind, clip.kind)) {
                    ++index;
                    continue;
                }

                if (clip.kind == ClipKind::Audio) {
                    audioTracks.insert(change.beforeTrackId);
                    audioTracks.insert(change.afterTrackId);
                } else if (clip.kind == ClipKind::Midi) {
                    midiTracks.insert(change.beforeTrackId);
                    midiTracks.insert(change.afterTrackId);
                } else if (clip.kind == ClipKind::Automation) {
                    if (std::find(automationTargets.begin(),
                                  automationTargets.end(),
                                  clip.automation.target) ==
                        automationTargets.end()) {
                        automationTargets.push_back(clip.automation.target);
                    }
                }
                graphDirty |= change.beforeTrackId != change.afterTrackId &&
                              !clip.inserts.empty();

                if (sourceTrackId == targetTrackId && index == targetIndex) {
                    clip.startSeconds = targetStartSeconds;
                    ++index;
                    continue;
                }

                ClipModel moved = std::move(clip);
                track.clips.erase(track.clips.begin() +
                                  static_cast<std::ptrdiff_t>(index));
                moved.startSeconds = targetStartSeconds;
                pending[targetTrackId].push_back(
                    PendingPlacement{targetIndex, std::move(moved)});
            }
        }

        for (auto& [trackId, placements] : pending) {
            const auto owner = owners.find(trackId);
            TrackModel* target =
                owner == owners.end() ? nullptr : owner->second;
            if (!target) continue;
            std::stable_sort(
                placements.begin(), placements.end(),
                [](const PendingPlacement& a, const PendingPlacement& b) {
                    return a.index < b.index;
                });

            // Merge once per target instead of repeatedly inserting into its
            // vector. Besides being linear for a large group move, this restores
            // the exact before/after paint and playback order captured by the
            // gesture even when several clips enter the same lane.
            std::vector<ClipModel> existing = std::move(target->clips);
            std::vector<ClipModel> merged;
            merged.reserve(existing.size() + placements.size());
            std::size_t existingIndex = 0;
            for (std::size_t movedIndex = 0;
                 movedIndex < placements.size(); ++movedIndex) {
                PendingPlacement& placement = placements[movedIndex];
                const std::size_t desiredIndex = std::min(
                    placement.index, existing.size() + placements.size());
                const std::size_t existingBefore =
                    desiredIndex > movedIndex ? desiredIndex - movedIndex : 0;
                while (existingIndex < existing.size() &&
                       existingIndex < existingBefore) {
                    merged.push_back(std::move(existing[existingIndex++]));
                }
                merged.push_back(std::move(placement.clip));
            }
            while (existingIndex < existing.size())
                merged.push_back(std::move(existing[existingIndex++]));
            target->clips = std::move(merged);
        }

        // A private clip chain is physically owned by its channel. Only that
        // ownership change needs graph topology work, and all such moves share
        // this one rebuild. Ordinary placements publish just their endpoints.
        if (graphDirty) {
            const audio::Result rebuilt = rebuildGraph();
            if (!rebuilt && durationChanged) updateTimelineDuration();
            return;
        }

        // Audio placements are already live during the initial drag. They need
        // republishing here only for undo/redo, or when a transient private-FX
        // crossing caused live placement publication to be deferred.
        if (publishAudio) {
            for (const std::string& trackId : audioTracks) {
                TrackModel* track = m_project.findTrack(trackId);
                if (track) syncTrackClips(*track);
            }
        }
        for (const std::string& trackId : midiTracks) {
            TrackModel* track = m_project.findTrack(trackId);
            if (!track) continue;
            syncTrackNotes(*track);
            syncTrackAutomation(*track);
        }
        for (const AutomationTarget& target : automationTargets)
            syncAutomationTarget(target);
        if (durationChanged) updateTimelineDuration();
    };

    // Geometry is already at the after endpoint; this call publishes only the
    // deferred realtime state and duration cache. A transient private-chain
    // crossing also deferred ordinary audio placements after that crossing.
    apply(true, edit.graphDirty);
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const PositionDelta& change : *delta) {
            const TrackModel* owner = m_project.findTrack(change.afterTrackId);
            std::string afterId;
            if (owner) {
                const auto found = std::find_if(
                    owner->clips.begin(), owner->clips.end(),
                    [&](const ClipModel& clip) {
                        return clip.id == change.clipId;
                    });
                if (found != owner->clips.end() && found != owner->clips.begin())
                    afterId = std::prev(found)->id;
            }
            if (change.beforeTrackId != change.afterTrackId ||
                change.beforeIndex != change.afterIndex) {
                appendCommand(batch, collab::MoveClip{
                    change.clipId, change.beforeTrackId,
                    change.afterTrackId, afterId});
            }
            if (std::abs(change.beforeStartSeconds -
                         change.afterStartSeconds) >= 1e-12) {
                appendCommand(batch, collab::SetClipProperty{
                    change.afterTrackId, change.clipId,
                    collab::ClipProperty::StartSeconds,
                    change.afterStartSeconds});
            }
        }
        if (!batch->commands.empty()) {
            const auto result = submitSharedMutation(
                collab::CommandBody{std::move(batch)}, label);
            if (result == collab::SharedMutationResult::Blocked)
                apply(false, true);
            if (result != collab::SharedMutationResult::LocalFallback)
                return;
        }
    }
    if (!label.empty()) {
        m_undo.push(label,
                    [apply] { apply(false, true); },
                    [apply] { apply(true, true); });
    }
}

void EngineController::setClipStartsSeconds(
    std::span<const ClipStartChange> changes) {
    if (cloudProjectBound() && !m_clipPositionEdit.active) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const ClipStartChange& change : changes) {
            const TrackModel* track = m_project.findTrack(change.trackId);
            const ClipModel* clip = findClip(change.trackId, change.clipId);
            if (!track || !clip) continue;
            const double next = std::max(0.0, change.startSeconds);
            const double delta = next - clip->startSeconds;
            if (std::abs(delta) < 1e-12) continue;
            appendCommand(batch, collab::SetClipProperty{
                change.trackId, change.clipId,
                collab::ClipProperty::StartSeconds, next});
            if (clip->kind != ClipKind::Pattern) continue;
            for (const TrackModel& memberTrack : m_project.tracks) {
                for (const ClipModel& member : memberTrack.clips) {
                    if (member.patternClipId != clip->id) continue;
                    appendCommand(batch, collab::SetClipProperty{
                        memberTrack.id, member.id,
                        collab::ClipProperty::StartSeconds,
                        std::max(0.0, member.startSeconds + delta)});
                }
            }
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       "Move Clip");
        }
        return;
    }
    std::unordered_set<std::string> audioTracks;
    std::unordered_set<std::string> midiTracks;
    bool automationMoved = false;
    bool changed = false;

    for (const ClipStartChange& change : changes) {
        TrackModel* track = m_project.findTrack(change.trackId);
        if (!track) continue;
        auto clipIt = std::find_if(track->clips.begin(), track->clips.end(),
                                   [&](const ClipModel& clip) {
                                       return clip.id == change.clipId;
                                   });
        if (clipIt == track->clips.end()) continue;

        ClipModel& clip = *clipIt;
        const std::size_t clipIndex =
            std::size_t(clipIt - track->clips.begin());
        const double next = std::max(0.0, change.startSeconds);
        const double delta = next - clip.startSeconds;
        if (std::abs(delta) < 1e-12) continue;
        if (m_clipPositionEdit.active) {
            auto [position, inserted] =
                m_clipPositionEdit.origins.try_emplace(
                    clip.id,
                    ClipPositionOrigin{track->id, clip.startSeconds,
                                       clipIndex, track->id,
                                       clip.startSeconds, clipIndex});
            (void)inserted;
            position->second.afterTrackId = track->id;
            position->second.afterStartSeconds = next;
            position->second.afterIndex = clipIndex;
        }
        clip.startSeconds = next;
        changed = true;

        if (clip.kind == ClipKind::Pattern) {
            for (TrackModel& memberTrack : m_project.tracks) {
                bool memberChanged = false;
                for (std::size_t memberIndex = 0;
                     memberIndex < memberTrack.clips.size(); ++memberIndex) {
                    ClipModel& member = memberTrack.clips[memberIndex];
                    if (member.patternClipId != clip.id) continue;
                    const double memberNext =
                        std::max(0.0, member.startSeconds + delta);
                    if (std::abs(memberNext - member.startSeconds) < 1e-12)
                        continue;
                    if (m_clipPositionEdit.active) {
                        auto [position, inserted] =
                            m_clipPositionEdit.origins.try_emplace(
                                member.id,
                                ClipPositionOrigin{
                                    memberTrack.id, member.startSeconds,
                                    memberIndex, memberTrack.id,
                                    member.startSeconds, memberIndex});
                        (void)inserted;
                        position->second.afterTrackId = memberTrack.id;
                        position->second.afterStartSeconds = memberNext;
                        position->second.afterIndex = memberIndex;
                    }
                    member.startSeconds = memberNext;
                    memberChanged = true;
                }
                if (!memberChanged) continue;
                midiTracks.insert(memberTrack.id);
            }
            continue;
        }

        if (clip.kind == ClipKind::Audio) audioTracks.insert(track->id);
        if (clip.kind == ClipKind::Midi) midiTracks.insert(track->id);
        if (clip.kind == ClipKind::Automation) automationMoved = true;
    }

    if (!changed) return;

    // Audio placements still follow an ordinary drag live. A private-chain
    // cross-track move is the exception: its graph is intentionally rebuilt
    // once at release, so the old owner must remain untouched in between.
    if (!m_clipPositionEdit.active || !m_clipPositionEdit.graphDirty) {
        for (const std::string& trackId : audioTracks) {
            TrackModel* track = m_project.findTrack(trackId);
            if (track) syncTrackClips(*track);
        }
    }

    if (m_clipPositionEdit.active) {
        return;
    }

    for (const std::string& trackId : midiTracks) {
        TrackModel* track = m_project.findTrack(trackId);
        if (!track) continue;
        syncTrackNotes(*track);
        syncTrackAutomation(*track);
    }
    if (automationMoved) syncAllAutomation();
    updateTimelineDuration();
}

double EngineController::clipSampleParameter(const std::string& trackId,
                                             const std::string& clipId,
                                             const std::string& id) {
    const ClipModel* clip = audioClip(trackId, clipId);
    if (!clip) return 0.0;
    const ClipSampleEditModel& s = clip->sampleEdit;
    double sourceDuration = 0.0;
    if (auto audio = loadSamples(clip->filePath); audio && audio->sampleRate() > 0.0)
        sourceDuration = double(audio->frames()) / audio->sampleRate();
    const double outputDuration = clip->durationSeconds > 0.0
                                      ? clip->durationSeconds
                                      : std::max(0.0, sourceDuration - clip->offsetSeconds) *
                                            s.stretchTime;
    if (id == "startoffset")
        return sourceDuration > 0.0 ? clip->offsetSeconds / sourceDuration : 0.0;
    if (id == "endoffset") {
        const double sourceEnd = clip->offsetSeconds +
            outputDuration / std::max(s.stretchTime, 0.01);
        return sourceDuration > 0.0 ? std::clamp(sourceEnd / sourceDuration, 0.0, 1.0)
                                    : 1.0;
    }
    if (id == "fadein")
        return outputDuration > 0.0 ? clip->fadeInSeconds / outputDuration : 0.0;
    if (id == "fadeout")
        return outputDuration > 0.0 ? clip->fadeOutSeconds / outputDuration : 0.0;
    if (id == "loop.mode") return s.loopMode;
    if (id == "loop.start") return s.loopStart;
    if (id == "loop.end") return s.loopEnd;
    if (id == "stretch.mode") return int(s.stretchMode);
    if (id == "stretch.time") return s.stretchTime;
    if (id == "stretch.pitch") return s.stretchPitch;
    if (id == "formant") return s.formant;
    if (id == "rootnote") return s.rootNote;
    if (id == "pre.boost") return s.boost;
    if (id == "pre.eq.low") return s.eqLow;
    if (id == "pre.eq.mid") return s.eqMid;
    if (id == "pre.eq.high") return s.eqHigh;
    if (id == "pre.rm.mix") return s.ringMix;
    if (id == "pre.rm.freq") return s.ringFreq;
    if (id == "pre.cut") return s.cut;
    if (id == "pre.res") return s.res;
    if (id == "pre.rev.type") return s.reverbType;
    if (id == "pre.rev") return s.reverb;
    if (id == "pre.delay") return s.stereoDelay;
    if (id == "pre.pogo") return s.pogo;
    if (id == "pre.dc") return s.removeDc ? 1.0 : 0.0;
    if (id == "pre.polarity") return s.reversePolarity ? 1.0 : 0.0;
    if (id == "pre.normalize") return s.normalize ? 1.0 : 0.0;
    if (id == "pre.fadestereo") return s.fadeStereo ? 1.0 : 0.0;
    if (id == "pre.reverse") return s.reverse ? 1.0 : 0.0;
    if (id == "pre.swap") return s.swapStereo ? 1.0 : 0.0;
    return 0.0;
}

void EngineController::setClipSampleParameter(const std::string& trackId,
                                              const std::string& clipId,
                                              const std::string& id,
                                              double value) {
    TrackModel* track = m_project.findTrack(trackId);
    ClipModel* clip = findClip(trackId, clipId);
    if (!track || !clip || clip->kind != ClipKind::Audio) return;
    ClipSampleEditModel& s = clip->sampleEdit;
    const plugins::sampler::PrecomputeSettings bakedBefore =
        clipPrecomputeSettings(s);

    double sourceDuration = 0.0;
    if (auto audio = loadSamples(clip->filePath); audio && audio->sampleRate() > 0.0)
        sourceDuration = double(audio->frames()) / audio->sampleRate();
    double outputDuration = clip->durationSeconds > 0.0
                                ? clip->durationSeconds
                                : std::max(0.0, sourceDuration - clip->offsetSeconds) *
                                      s.stretchTime;
    const double sourceEnd = clip->offsetSeconds +
        outputDuration / std::max(s.stretchTime, 0.01);

    if (id == "startoffset" && sourceDuration > 0.0) {
        const double next = std::clamp(value, 0.0, 1.0) * sourceDuration;
        clip->offsetSeconds = std::min(next, sourceEnd - kMinClipSeconds);
        clip->durationSeconds =
            std::max(kMinClipSeconds, sourceEnd - clip->offsetSeconds) * s.stretchTime;
    } else if (id == "endoffset" && sourceDuration > 0.0) {
        const double next = std::clamp(value, 0.0, 1.0) * sourceDuration;
        const double end = std::max(clip->offsetSeconds + kMinClipSeconds, next);
        clip->durationSeconds = (end - clip->offsetSeconds) * s.stretchTime;
    } else if (id == "fadein") {
        clip->fadeInSeconds = std::clamp(value, 0.0, 1.0) * outputDuration;
    } else if (id == "fadeout") {
        clip->fadeOutSeconds = std::clamp(value, 0.0, 1.0) * outputDuration;
    } else if (id == "loop.mode") s.loopMode = std::clamp(int(std::lround(value)), 0, 2);
    else if (id == "loop.start") s.loopStart = std::min(std::clamp(value, 0.0, 1.0), s.loopEnd);
    else if (id == "loop.end") s.loopEnd = std::max(std::clamp(value, 0.0, 1.0), s.loopStart);
    else if (id == "stretch.mode")
        s.stretchMode = ClipStretchMode(std::clamp(int(std::lround(value)), 0, 4));
    else if (id == "stretch.time") {
        const double next = std::clamp(value, 0.25, 4.0);
        const double sourceSpan = outputDuration / std::max(s.stretchTime, 0.01);
        s.stretchTime = next;
        clip->durationSeconds = std::max(kMinClipSeconds, sourceSpan * next);
    } else if (id == "stretch.pitch") s.stretchPitch = std::clamp(value, -24.0, 24.0);
    else if (id == "formant") s.formant = std::clamp(value, -12.0, 12.0);
    else if (id == "rootnote") s.rootNote = std::clamp(int(std::lround(value)), 0, 127);
    else if (id == "pre.boost") s.boost = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.eq.low") s.eqLow = std::clamp(value, -1.0, 1.0);
    else if (id == "pre.eq.mid") s.eqMid = std::clamp(value, -1.0, 1.0);
    else if (id == "pre.eq.high") s.eqHigh = std::clamp(value, -1.0, 1.0);
    else if (id == "pre.rm.mix") s.ringMix = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.rm.freq") s.ringFreq = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.cut") s.cut = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.res") s.res = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.rev.type") s.reverbType = std::clamp(int(std::lround(value)), 0, 1);
    else if (id == "pre.rev") s.reverb = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.delay") s.stereoDelay = std::clamp(value, 0.0, 1.0);
    else if (id == "pre.pogo") s.pogo = std::clamp(value, -1.0, 1.0);
    else if (id == "pre.dc") s.removeDc = value >= 0.5;
    else if (id == "pre.polarity") s.reversePolarity = value >= 0.5;
    else if (id == "pre.normalize") s.normalize = value >= 0.5;
    else if (id == "pre.fadestereo") s.fadeStereo = value >= 0.5;
    else if (id == "pre.reverse") s.reverse = value >= 0.5;
    else if (id == "pre.swap") s.swapStereo = value >= 0.5;
    else return;

    if (clip->durationSeconds > 0.0) {
        clip->fadeInSeconds = std::min(clip->fadeInSeconds, clip->durationSeconds);
        clip->fadeOutSeconds = std::min(clip->fadeOutSeconds, clip->durationSeconds);
    }
    // A knob that only changes how the sample is *played* republishes the clip
    // list straight away — it is a few pointers. A knob that changes what the
    // sample *is* has to re-render it, so it waits for the next control-thread
    // turn and a whole drag costs one bake instead of one per mouse move.
    // Asking the settings themselves, rather than testing the parameter id,
    // means a new precomputed knob cannot forget to opt in.
    if (clipPrecomputeSettings(s) == bakedBefore) syncTrackClips(*track);
    else deferClipSync(track->id);
    updateTimelineDuration();
}

double EngineController::snappedStretchTime(const std::string& trackId,
                                            const std::string& clipId,
                                            double wanted,
                                            double gridSeconds) const {
    constexpr double kMinStretch = 0.25;
    constexpr double kMaxStretch = 4.0;
    /// Widest the detent may be, as a fraction of the knob's whole range —
    /// a few pixels of travel, so it reads as resistance rather than a jump.
    constexpr double kMaxBandOfRange = 0.03;
    /// …and of the gap between two grid lines, so most of the way between them
    /// is always free.
    constexpr double kMaxBandOfGap = 0.35;

    wanted = std::clamp(wanted, kMinStretch, kMaxStretch);
    if (gridSeconds <= 0.0) return wanted;
    const ClipModel* clip = audioClip(trackId, clipId);
    if (!clip) return wanted;

    // How long the material is in the file. It does not move when the stretch
    // does — that is the whole point of the control — so it is what turns a
    // position on the timeline back into a value for the knob.
    const double stretch = std::max(clip->sampleEdit.stretchTime, 0.01);
    const double sourceSpan = clip->durationSeconds / stretch;
    if (!(sourceSpan > 0.0)) return wanted;

    const double end = clip->startSeconds + sourceSpan * wanted;
    const double line = std::round(end / gridSeconds) * gridSeconds;
    const double onGrid = (line - clip->startSeconds) / sourceSpan;
    if (onGrid < kMinStretch || onGrid > kMaxStretch) return wanted;

    const double gap = gridSeconds / sourceSpan;
    const double band = std::min((kMaxStretch - kMinStretch) * kMaxBandOfRange,
                                 gap * kMaxBandOfGap);
    return std::abs(onGrid - wanted) <= band ? onGrid : wanted;
}

void EngineController::commitClipSampleParameterEdit(
    const std::string& trackId, const std::string& clipId,
    const std::string& parameterId, double before, const std::string& label) {
    const double after = clipSampleParameter(trackId, clipId, parameterId);
    if (std::abs(after - before) < 1e-9) return;
    const ClipModel* current = audioClip(trackId, clipId);
    const ClipMusicalAnalysisModel analysisBefore =
        current ? current->musicalAnalysis : ClipMusicalAnalysisModel{};
    ClipMusicalAnalysisModel analysisAfter = analysisBefore;
    if (parameterId == "startoffset" || parameterId == "endoffset" ||
        parameterId == "loop.mode" || parameterId == "loop.start" ||
        parameterId == "loop.end") {
        analysisAfter = {};
    } else if (parameterId == "stretch.time") {
        analysisAfter.tempo = {};
    } else if (parameterId == "stretch.pitch") {
        analysisAfter.key = {};
    }
    if (ClipModel* edited = findClip(trackId, clipId))
        edited->musicalAnalysis = analysisAfter;
    if (cloudProjectBound() && current) {
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetClipSampleEdit{
            trackId, clipId, current->sampleEdit});
        appendCommand(batch, collab::SetClipMusicalAnalysis{
            trackId, clipId, analysisAfter});
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, label);
        if (result == collab::SharedMutationResult::Blocked) {
            setClipSampleParameter(trackId, clipId, parameterId, before);
            if (ClipModel* target = findClip(trackId, clipId))
                target->musicalAnalysis = analysisBefore;
        }
        if (result != collab::SharedMutationResult::LocalFallback) return;
    }
    const auto apply = [this, trackId, clipId, parameterId](
                           double value,
                           const ClipMusicalAnalysisModel& analysis) {
        setClipSampleParameter(trackId, clipId, parameterId, value);
        if (ClipModel* target = findClip(trackId, clipId))
            target->musicalAnalysis = analysis;
    };
    m_undo.push(label,
                [apply, before, analysisBefore] {
                    apply(before, analysisBefore);
                },
                [apply, after, analysisAfter] {
                    apply(after, analysisAfter);
                });
}

void EngineController::setClipGain(const std::string& trackId,
                                   const std::string& clipId, float gain) {
    setClipFxVolume(trackId, clipId, gain);
}

void EngineController::setClipFade(const std::string& trackId,
                                   const std::string& clipId,
                                   double fadeInSeconds, double fadeOutSeconds) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;

        double fadeIn = std::max(0.0, fadeInSeconds);
        double fadeOut = std::max(0.0, fadeOutSeconds);
        const double len = clip.durationSeconds;
        if (len > 0.0) {
            fadeIn = std::min(fadeIn, len);
            fadeOut = std::min(fadeOut, len);
            // The two ramps can't cross over each other.
            if (fadeIn + fadeOut > len) {
                if (fadeInSeconds > clip.fadeInSeconds) fadeOut = len - fadeIn;
                else fadeIn = len - fadeOut;
            }
        }
        fadeIn = std::max(0.0, fadeIn);
        fadeOut = std::max(0.0, fadeOut);
        if (clip.fadeInSeconds == fadeIn && clip.fadeOutSeconds == fadeOut)
            return;
        clip.fadeInSeconds = fadeIn;
        clip.fadeOutSeconds = fadeOut;
        syncTrackClips(*track);
        return;
    }
}

void EngineController::commitClipFadeEdit(const std::string& trackId,
                                          const std::string& clipId,
                                          double beforeIn, double beforeOut,
                                          const std::string& label) {
    const ClipModel* clip = findClip(trackId, clipId);
    if (!clip) return;
    const double afterIn = clip->fadeInSeconds;
    const double afterOut = clip->fadeOutSeconds;
    if (beforeIn == afterIn && beforeOut == afterOut) return;

    const auto shared = submitSharedMutation(
        collab::SetClipFade{trackId, clipId, afterIn, afterOut}, label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked)
            setClipFade(trackId, clipId, beforeIn, beforeOut);
        return;
    }

    auto apply = [this, trackId, clipId](double fadeIn, double fadeOut) {
        setClipFade(trackId, clipId, fadeIn, fadeOut);
    };
    m_undo.push(label,
                [apply, beforeIn, beforeOut] {
                    apply(beforeIn, beforeOut);
                },
                [apply, afterIn, afterOut] { apply(afterIn, afterOut); });
}

void EngineController::setClipFadeCurve(const std::string& trackId,
                                        const std::string& clipId,
                                        bool fadeIn, double curve) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;
        const double value = std::clamp(curve, -1.0, 1.0);
        double& current = fadeIn ? clip.fadeInCurve : clip.fadeOutCurve;
        if (current == value) return;
        current = value;
        syncTrackClips(*track);
        return;
    }
}

void EngineController::commitClipFadeCurveEdit(
    const std::string& trackId, const std::string& clipId, bool fadeIn,
    double beforeCurve, const std::string& label) {
    const ClipModel* clip = findClip(trackId, clipId);
    if (!clip) return;
    const double afterCurve = fadeIn ? clip->fadeInCurve : clip->fadeOutCurve;
    if (beforeCurve == afterCurve) return;

    const auto shared = submitSharedMutation(
        collab::SetClipFadeCurve{trackId, clipId,
                                 fadeIn ? collab::ClipEdge::In
                                        : collab::ClipEdge::Out,
                                 afterCurve},
        label);
    if (shared != collab::SharedMutationResult::LocalFallback) {
        if (shared == collab::SharedMutationResult::Blocked)
            setClipFadeCurve(trackId, clipId, fadeIn, beforeCurve);
        return;
    }

    auto apply = [this, trackId, clipId, fadeIn](double curve) {
        setClipFadeCurve(trackId, clipId, fadeIn, curve);
    };
    m_undo.push(label, [apply, beforeCurve] { apply(beforeCurve); },
                [apply, afterCurve] { apply(afterCurve); });
}

void EngineController::setClipFadeMode(const std::string& trackId,
                                       const std::string& clipId,
                                       bool fadeIn, ClipFadeMode mode) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;
        ClipFadeMode& value = fadeIn ? clip.fadeInMode : clip.fadeOutMode;
        if (value == mode) return;
        const ClipFadeMode before = value;
        const auto shared = submitSharedMutation(
            collab::SetClipFadeMode{trackId, clipId,
                                    fadeIn ? collab::ClipEdge::In
                                           : collab::ClipEdge::Out,
                                    mode},
            mode == ClipFadeMode::Tape ? "Set Tape Fade" : "Set Gain Fade");
        if (shared != collab::SharedMutationResult::LocalFallback) return;
        value = mode;
        syncTrackClips(*track);
        auto apply = [this, trackId, clipId, fadeIn](ClipFadeMode next) {
            auto* t = m_project.findTrack(trackId);
            if (!t) return;
            for (auto& c : t->clips) {
                if (c.id != clipId) continue;
                (fadeIn ? c.fadeInMode : c.fadeOutMode) = next;
                syncTrackClips(*t);
                return;
            }
        };
        m_undo.push(mode == ClipFadeMode::Tape ? "Set Tape Fade" : "Set Gain Fade",
                    [apply, before] { apply(before); },
                    [apply, mode] { apply(mode); });
        return;
    }
}

void EngineController::moveClipToTrack(const std::string& fromTrackId,
                                       const std::string& clipId,
                                       const std::string& toTrackId) {
    if (fromTrackId == toTrackId) return;
    auto* from = m_project.findTrack(fromTrackId);
    auto* to = m_project.findTrack(toTrackId);
    if (!from || !to) return;

    auto it = std::find_if(from->clips.begin(), from->clips.end(),
                           [&](const ClipModel& c) { return c.id == clipId; });
    if (it == from->clips.end()) return;
    // The destination has to take this kind of clip — audio onto an audio lane,
    // MIDI onto a MIDI/instrument lane, and nothing at all onto a folder or bus.
    if (!trackAccepts(to->kind, it->kind)) return;

    // Outside a position-edit bracket this writes straight into the document,
    // and endClipPositionEdit is what turns a lane crossing into a command.
    // Under a cloud binding an unbracketed move would therefore be a silent
    // local-only divergence; refusing is the honest answer. Every caller that
    // needs it already brackets, or now does.
    if (!m_clipPositionEdit.active && cloudProjectBound()) return;

    const std::size_t sourceIndex =
        std::size_t(it - from->clips.begin());
    const std::size_t targetIndex = to->clips.size();
    if (m_clipPositionEdit.active) {
        auto [position, inserted] =
            m_clipPositionEdit.origins.try_emplace(
                it->id,
                ClipPositionOrigin{from->id, it->startSeconds, sourceIndex,
                                   from->id, it->startSeconds, sourceIndex});
        (void)inserted;
        position->second.afterTrackId = to->id;
        position->second.afterStartSeconds = it->startSeconds;
        position->second.afterIndex = targetIndex;
    }
    const bool hasPrivateFx = !it->inserts.empty();
    const ClipKind kind = it->kind;
    // A lane crossing changes only ownership. Move the ClipModel so a dense
    // note list (or audio edit payload) keeps its allocation throughout the
    // live gesture and the delta undo/redo that follows.
    ClipModel clip = std::move(*it);
    from->clips.erase(it);
    to->clips.push_back(std::move(clip));

    if (m_clipPositionEdit.active) {
        if (hasPrivateFx) {
            m_clipPositionEdit.graphDirty = true;
        } else if (kind == ClipKind::Audio) {
            // Ordinary audio placements can continue to follow the pointer;
            // MIDI and automation have their own deferred snapshots below.
            syncTrackClips(*from);
            syncTrackClips(*to);
        }
        return;
    }

    if (hasPrivateFx) rebuildGraph();
    else {
        syncTrackClips(*from);
        syncTrackClips(*to);
    }
    // Notes and curves belong to a *channel*, and the clip just changed which
    // one it is on. Without this a MIDI clip dragged to another lane left its
    // notes playing on the old track's instrument and its automation still
    // driving the old track's plugins.
    if (kind == ClipKind::Midi) {
        syncTrackNotes(*from);
        syncTrackNotes(*to);
        syncTrackAutomation(*from);
        syncTrackAutomation(*to);
    }
    if (kind == ClipKind::Automation) syncAllAutomation();

    // Ownership alone cannot change the project endpoint, so there is no
    // duration scan here. A bracketed arrangement gesture records lane and
    // position together through its small placement delta on release.
}

void EngineController::beginClipTrimEdit(const std::string& trackId,
                                         const std::string& clipId) {
    beginClipTrimEdit({{trackId, clipId}});
}

void EngineController::beginClipTrimEdit(
    const std::vector<std::pair<std::string, std::string>>& clips) {
    if (m_clipTrimEdit.active) endClipTrimEdit();
    m_clipTrimEdit = {};
    m_clipTrimEdit.origins.reserve(clips.size());
    for (const auto& [trackId, clipId] : clips) {
        const bool duplicate = std::any_of(
            m_clipTrimEdit.origins.begin(), m_clipTrimEdit.origins.end(),
            [&](const ClipTrimOrigin& origin) {
                return origin.trackId == trackId && origin.clipId == clipId;
            });
        if (duplicate) continue;
        const ClipModel* clip = findClip(trackId, clipId);
        if (!clip) continue;

        ClipTrimOrigin origin;
        origin.trackId = trackId;
        origin.clipId = clipId;
        origin.kind = clip->kind;
        origin.beforeStartSeconds = clip->startSeconds;
        origin.beforeOffsetSeconds = clip->offsetSeconds;
        origin.beforeDurationSeconds = clip->durationSeconds;
        origin.beforeMusicalAnalysis = clip->musicalAnalysis;
        if (clip->kind == ClipKind::Automation)
            origin.beforeAutomation = clip->automation;

        // Resolving source files can touch caches. Do it once on press, never
        // for every pointer sample of the group edge drag.
        if (clip->kind == ClipKind::Audio && !clip->filePath.empty()) {
            if (auto samples = loadSamples(clip->filePath);
                samples && samples->sampleRate() > 0.0) {
                origin.sourceDurationSeconds =
                    double(samples->frames()) / samples->sampleRate();
            }
        }
        if (clip->kind == ClipKind::Pattern) {
            origin.patternMemberTrackIds.reserve(m_project.tracks.size());
            for (const TrackModel& memberTrack : m_project.tracks) {
                const bool owns = std::any_of(
                    memberTrack.clips.begin(), memberTrack.clips.end(),
                    [&](const ClipModel& member) {
                        return member.patternClipId == clipId;
                    });
                if (owns)
                    origin.patternMemberTrackIds.push_back(memberTrack.id);
            }
        }
        m_clipTrimEdit.origins.push_back(std::move(origin));
    }
    m_clipTrimEdit.active = !m_clipTrimEdit.origins.empty();
}

void EngineController::endClipTrimEdit(const std::string& label) {
    if (!m_clipTrimEdit.active) return;
    ClipTrimEdit edit = std::move(m_clipTrimEdit);
    m_clipTrimEdit = {};

    struct TrimSnapshot {
        double startSeconds = 0.0;
        double offsetSeconds = 0.0;
        double durationSeconds = 0.0;
        ClipMusicalAnalysisModel musicalAnalysis;
        ClipAutomationModel automation;
    };
    struct TrimItem {
        std::string trackId;
        std::string clipId;
        ClipKind kind = ClipKind::Audio;
        TrimSnapshot before;
        TrimSnapshot after;
        std::vector<std::string> patternMemberTrackIds;
    };
    struct TrimDelta {
        std::vector<TrimItem> items;
    };

    TrimDelta captured;
    captured.items.reserve(edit.origins.size());
    for (ClipTrimOrigin& origin : edit.origins) {
        ClipModel* clip = findClip(origin.trackId, origin.clipId);
        if (!clip) continue;
        const bool geometryChanged =
            origin.beforeStartSeconds != clip->startSeconds ||
            origin.beforeOffsetSeconds != clip->offsetSeconds ||
            origin.beforeDurationSeconds != clip->durationSeconds;
        if (!origin.dirty || !geometryChanged) continue;
        captured.items.push_back(TrimItem{
            origin.trackId, origin.clipId, origin.kind,
            TrimSnapshot{origin.beforeStartSeconds,
                         origin.beforeOffsetSeconds,
                         origin.beforeDurationSeconds,
                         std::move(origin.beforeMusicalAnalysis),
                         std::move(origin.beforeAutomation)},
            TrimSnapshot{clip->startSeconds, clip->offsetSeconds,
                         clip->durationSeconds, clip->musicalAnalysis,
                         origin.kind == ClipKind::Automation
                             ? clip->automation
                             : ClipAutomationModel{}},
            std::move(origin.patternMemberTrackIds)});
    }
    if (captured.items.empty()) return;
    auto delta = std::make_shared<const TrimDelta>(std::move(captured));

    auto publish = [this, delta] {
        for (const TrimItem& item : delta->items) {
            switch (item.kind) {
            case ClipKind::Audio: {
                if (TrackModel* track = m_project.findTrack(item.trackId))
                    syncTrackClips(*track);
                break;
            }
            case ClipKind::Midi: {
                if (TrackModel* track = m_project.findTrack(item.trackId)) {
                    syncTrackNotes(*track);
                    syncTrackAutomation(*track);
                }
                break;
            }
            case ClipKind::Automation: {
                if (const ClipModel* current =
                        findClip(item.trackId, item.clipId))
                    syncAutomationTarget(current->automation.target);
                break;
            }
            case ClipKind::Pattern:
                for (const std::string& memberTrackId :
                     item.patternMemberTrackIds) {
                    if (TrackModel* track =
                            m_project.findTrack(memberTrackId)) {
                        syncTrackNotes(*track);
                        syncTrackAutomation(*track);
                    }
                }
                break;
            }
        }
        updateTimelineDuration();
    };

    auto apply = [this, delta, publish](bool after) {
        for (const TrimItem& item : delta->items) {
            ClipModel* target = findClip(item.trackId, item.clipId);
            if (!target) continue;
            const TrimSnapshot& value = after ? item.after : item.before;
            target->startSeconds = value.startSeconds;
            target->offsetSeconds = value.offsetSeconds;
            target->durationSeconds = value.durationSeconds;
            target->musicalAnalysis = value.musicalAnalysis;
            if (item.kind == ClipKind::Automation)
                target->automation = value.automation;
        }
        publish();
    };

    // The document is already at the after endpoint. Publish it without
    // assigning any captured payload back into the live clip.
    publish();
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        ProjectModel scratch = m_project;
        for (const TrimItem& item : delta->items) {
            ClipModel* beforeClip = nullptr;
            if (TrackModel* owner = scratch.findTrack(item.trackId)) {
                const auto found = std::find_if(
                    owner->clips.begin(), owner->clips.end(),
                    [&](const ClipModel& clip) { return clip.id == item.clipId; });
                if (found != owner->clips.end()) beforeClip = &*found;
            }
            if (!beforeClip) {
                apply(false);
                return;
            }
            beforeClip->startSeconds = item.before.startSeconds;
            beforeClip->offsetSeconds = item.before.offsetSeconds;
            beforeClip->durationSeconds = item.before.durationSeconds;
            beforeClip->musicalAnalysis = item.before.musicalAnalysis;
            if (item.kind == ClipKind::Automation)
                beforeClip->automation = item.before.automation;

            if (item.before.startSeconds != item.after.startSeconds) {
                appendCommand(batch, collab::SetClipProperty{
                    item.trackId, item.clipId,
                    collab::ClipProperty::StartSeconds,
                    item.after.startSeconds});
            }
            if (item.before.offsetSeconds != item.after.offsetSeconds) {
                appendCommand(batch, collab::SetClipProperty{
                    item.trackId, item.clipId,
                    collab::ClipProperty::OffsetSeconds,
                    item.after.offsetSeconds});
            }
            if (item.before.durationSeconds != item.after.durationSeconds) {
                appendCommand(batch, collab::SetClipProperty{
                    item.trackId, item.clipId,
                    collab::ClipProperty::DurationSeconds,
                    item.after.durationSeconds});
            }
            if (item.kind == ClipKind::Audio) {
                appendCommand(batch, collab::SetClipMusicalAnalysis{
                    item.trackId, item.clipId, item.after.musicalAnalysis});
            }
            if (item.kind != ClipKind::Automation) continue;

            const ClipAutomationModel& before = item.before.automation;
            const ClipAutomationModel& after = item.after.automation;
            if (before.target != after.target) {
                appendCommand(batch, collab::SetAutomationTarget{
                    item.trackId, item.clipId, after.target});
            }
            if (before.defaultValue != after.defaultValue) {
                appendCommand(batch, collab::SetAutomationDefault{
                    item.trackId, item.clipId, after.defaultValue});
            }
            if (before.active != after.active) {
                appendCommand(batch, collab::SetAutomationActive{
                    item.trackId, item.clipId, after.active});
            }
            std::unordered_set<std::string> afterIds;
            afterIds.reserve(after.points.size());
            for (const AutomationPoint& point : after.points)
                afterIds.insert(point.id);
            for (const AutomationPoint& point : before.points) {
                if (!afterIds.contains(point.id)) {
                    appendCommand(batch, collab::DeleteAutomationPoint{
                        item.trackId, item.clipId, {}, point.id});
                }
            }
            std::unordered_map<std::string, const AutomationPoint*> beforeById;
            beforeById.reserve(before.points.size());
            for (const AutomationPoint& point : before.points)
                beforeById.emplace(point.id, &point);
            for (std::size_t index = 0; index < after.points.size(); ++index) {
                const AutomationPoint& point = after.points[index];
                const auto old = beforeById.find(point.id);
                if (old != beforeById.end() && *old->second == point) continue;
                appendCommand(batch, collab::UpsertAutomationPoint{
                    item.trackId, item.clipId, {}, point,
                    index == 0 ? std::string() : after.points[index - 1].id});
            }
        }
        if (batch->commands.empty() || !sharedBatchApplies(scratch, batch)) {
            apply(false);
            return;
        }
        const auto shared = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            label.empty() ? "Trim Clip" : label);
        if (shared == collab::SharedMutationResult::Blocked) apply(false);
        if (shared != collab::SharedMutationResult::LocalFallback) return;
    }
    if (!label.empty()) {
        m_undo.push(label, [apply] { apply(false); },
                    [apply] { apply(true); });
    }
}

void EngineController::setClipTrim(const std::string& trackId,
                                   const std::string& clipId, double startSeconds,
                                   double offsetSeconds, double durationSeconds) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;

        ClipTrimOrigin* gesture = nullptr;
        if (m_clipTrimEdit.active) {
            const auto found = std::find_if(
                m_clipTrimEdit.origins.begin(), m_clipTrimEdit.origins.end(),
                [&](const ClipTrimOrigin& origin) {
                    return origin.trackId == trackId && origin.clipId == clipId;
                });
            if (found != m_clipTrimEdit.origins.end()) gesture = &*found;
        }

        if (clip.kind == ClipKind::Pattern) {
            const double newStart = std::max(0.0, startSeconds);
            const double newDuration =
                std::max(kMinClipSeconds, durationSeconds);
            const bool changed = clip.startSeconds != newStart ||
                                 clip.durationSeconds != newDuration;
            if (!changed) return;
            clip.startSeconds = newStart;
            clip.durationSeconds = newDuration;
            if (gesture) {
                gesture->dirty |= changed;
                if (changed) {
                    for (const std::string& memberTrackId :
                         gesture->patternMemberTrackIds) {
                        bumpMidiNotesRevision(memberTrackId);
                    }
                }
                return;
            }
            // The member clips keep their source data; their audible range is
            // gated by the container in syncTrackNotes. That makes a live edge
            // drag reversible in either direction instead of deleting notes as
            // soon as the edge crosses them.
            for (TrackModel& memberTrack : m_project.tracks) {
                const bool owns = std::any_of(
                    memberTrack.clips.begin(), memberTrack.clips.end(),
                    [&](const ClipModel& member) {
                        return member.patternClipId == clipId;
                    });
                if (owns) {
                    syncTrackNotes(memberTrack);
                    syncTrackAutomation(memberTrack);
                }
            }
            updateTimelineDuration();
            return;
        }

        if (clip.kind == ClipKind::Automation) {
            // A curve has no source file to run past, so the edges are free.
            // Pulling the *head* moves the clip's origin, and the points are
            // timed from that origin — so they are rebased by the same amount
            // and the curve stays put over the arrangement while the window
            // over it changes.
            const double before =
                gesture ? gesture->beforeStartSeconds : clip.startSeconds;
            const double newStart = std::max(0.0, startSeconds);
            const double shiftBeats =
                secondsToBeats(newStart - before, m_project.tempo);
            ClipAutomationModel next =
                gesture ? gesture->beforeAutomation : clip.automation;
            const double newDuration =
                std::max(kMinClipSeconds, durationSeconds);
            const bool geometryChanged = clip.startSeconds != newStart ||
                                         clip.durationSeconds != newDuration;
            if (std::abs(shiftBeats) > 1e-9) {
                for (AutomationPoint& point : next.points)
                    point.beats -= shiftBeats;
                // What the curve held at the new head becomes the value before
                // the first surviving point, so trimming does not reveal a
                // default the user never chose.
                next.defaultValue =
                    automationValueAt(next.points, 0.0, next.defaultValue);
                std::erase_if(next.points,
                              [](const AutomationPoint& p) { return p.beats < 0.0; });
                normalizeAutomation(next.points);
            }
            const bool curveChanged =
                clip.automation.target != next.target ||
                clip.automation.defaultValue != next.defaultValue ||
                clip.automation.points != next.points;
            if (!geometryChanged && !curveChanged) return;
            clip.startSeconds = newStart;
            clip.durationSeconds = newDuration;
            clip.automation = std::move(next);
            if (gesture) {
                gesture->dirty |= geometryChanged || curveChanged;
                return;
            }
            syncAutomationTarget(clip.automation.target);
            updateTimelineDuration();
            return;
        }

        // The source's full length, so a trim can't run past the file's end.
        double sourceDuration =
            gesture ? gesture->sourceDurationSeconds : 0.0;
        if (!gesture) {
            if (auto samples = loadSamples(clip.filePath);
                samples && samples->sampleRate() > 0.0) {
                sourceDuration =
                    double(samples->frames()) / samples->sampleRate();
            }
        }

        double newOffset = std::max(0.0, offsetSeconds);
        double newStart = std::max(0.0, startSeconds);
        double newDuration = std::max(kMinClipSeconds, durationSeconds);
        if (sourceDuration > 0.0) {
            newOffset = std::min(newOffset,
                                 std::max(0.0, sourceDuration - kMinClipSeconds));
            // A stretched clip's timeline length is measured after stretching,
            // while its offset remains in source seconds.  A loop may be made
            // arbitrarily longer than the source selection; a non-looping clip
            // still stops at the physical end of the file.
            if (clip.sampleEdit.loopMode == 0) {
                const double outputAvailable =
                    (sourceDuration - newOffset) *
                    std::max(clip.sampleEdit.stretchTime, 0.01);
                newDuration = std::min(newDuration, outputAvailable);
            }
        }
        if (newDuration < kMinClipSeconds) return;

        // Live edit like the drag-move: applied immediately, not pushed onto the
        // undo stack per mouse move.
        const bool changed = clip.startSeconds != newStart ||
                             clip.offsetSeconds != newOffset ||
                             clip.durationSeconds != newDuration;
        if (!changed) return;
        clip.startSeconds = newStart;
        clip.offsetSeconds = newOffset;
        clip.durationSeconds = newDuration;
        if (gesture &&
            newStart == gesture->beforeStartSeconds &&
            newOffset == gesture->beforeOffsetSeconds &&
            newDuration == gesture->beforeDurationSeconds) {
            clip.musicalAnalysis = gesture->beforeMusicalAnalysis;
        } else {
            clip.musicalAnalysis = {};
        }
        if (gesture) {
            gesture->dirty |= changed;
            if (changed && clip.kind == ClipKind::Midi)
                bumpMidiNotesRevision(trackId);
            return;
        }
        syncTrackClips(*track);
        if (clip.kind == ClipKind::Midi) {
            syncTrackNotes(*track);
            syncTrackAutomation(*track);
        }
        updateTimelineDuration();
        return;
    }
}

namespace {

/// One clip owned by a Pattern history endpoint. Only ids recorded in the
/// delta are ever removed; unrelated clips stay in their live relative order.
struct IndexedPatternClip {
    std::string trackId;
    std::size_t index = 0;
    ClipModel clip;
};

struct PatternClipEndpointDelta {
    std::vector<IndexedPatternClip> before;
    std::vector<IndexedPatternClip> after;
    std::unordered_map<std::string, std::unordered_set<std::string>> ids;
};

void rememberPatternClip(PatternClipEndpointDelta& delta, bool after,
                         IndexedPatternClip state) {
    delta.ids[state.trackId].insert(state.clip.id);
    (after ? delta.after : delta.before).push_back(std::move(state));
}

void restorePatternClipEndpoint(ProjectModel& project,
                                const PatternClipEndpointDelta& delta,
                                bool useAfter) {
    // Never erase by a captured vector index: a later unrelated insertion may
    // occupy it. Ids are document identities; indices are only insertion hints.
    for (const auto& [trackId, ids] : delta.ids) {
        TrackModel* track = project.findTrack(trackId);
        if (!track) continue;
        std::erase_if(track->clips, [&](const ClipModel& clip) {
            return ids.contains(clip.id);
        });
    }

    const auto& endpoint = useAfter ? delta.after : delta.before;
    std::vector<const IndexedPatternClip*> ordered;
    ordered.reserve(endpoint.size());
    for (const IndexedPatternClip& state : endpoint)
        ordered.push_back(&state);
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const IndexedPatternClip* a, const IndexedPatternClip* b) {
            if (a->trackId != b->trackId) return a->trackId < b->trackId;
            return a->index < b->index;
        });
    for (const IndexedPatternClip* state : ordered) {
        TrackModel* track = project.findTrack(state->trackId);
        if (!track) continue;
        const std::size_t index = std::min(state->index, track->clips.size());
        track->clips.insert(track->clips.begin() + std::ptrdiff_t(index),
                            state->clip);
    }
}

ClipModel* findPatternDeltaClip(ProjectModel& project,
                                const std::string& trackId,
                                const std::string& clipId) {
    TrackModel* track = project.findTrack(trackId);
    if (!track) return nullptr;
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const ClipModel& clip) { return clip.id == clipId; });
    return found == track->clips.end() ? nullptr : &*found;
}

bool patternClipNeedsGraphRebuild(const ClipModel& clip) {
    // Clip-private chains are currently realised only for audio clips.
    return clip.kind == ClipKind::Audio && !clip.inserts.empty();
}

void appendUniqueTrack(std::vector<std::string>& tracks,
                       const std::string& trackId) {
    if (std::find(tracks.begin(), tracks.end(), trackId) == tracks.end())
        tracks.push_back(trackId);
}

} // namespace

std::string EngineController::splitClip(const std::string& trackId,
                                        const std::string& clipId,
                                        double atSeconds) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return {};

    if (cloudProjectBound()) {
        const auto found = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const ClipModel& clip) { return clip.id == clipId; });
        if (found == track->clips.end()) return {};
        if (found->kind == ClipKind::Pattern) {
            const ClipModel original = *found;
            const double clipEnd =
                original.startSeconds + original.durationSeconds;
            if (original.durationSeconds <= 0.0 ||
                atSeconds <= original.startSeconds + kMinClipSeconds ||
                atSeconds >= clipEnd - kMinClipSeconds) {
                return {};
            }
            for (const TrackModel& memberTrack : m_project.tracks) {
                for (const ClipModel& member : memberTrack.clips) {
                    if (member.patternClipId == clipId &&
                        member.kind != ClipKind::Midi) {
                        return {};
                    }
                }
            }

            const double leftDuration = atSeconds - original.startSeconds;
            ClipModel rightPattern = original;
            rightPattern.startSeconds = atSeconds;
            rightPattern.durationSeconds = clipEnd - atSeconds;
            rightPattern.offsetSeconds = original.offsetSeconds + leftDuration;
            rightPattern.patternClipId.clear();
            mintClipIdentities(rightPattern, {}, true);

            auto batch = std::make_shared<collab::BatchCommand>();
            appendCommand(batch, collab::SetClipProperty{
                trackId, clipId, collab::ClipProperty::DurationSeconds,
                leftDuration});
            if (!appendSharedClip(
                    batch, trackId, rightPattern,
                    track->clips.empty() ? std::string()
                                         : track->clips.back().id)) {
                return {};
            }

            std::unordered_map<std::string, std::string> anchors;
            for (const TrackModel& memberTrack : m_project.tracks) {
                anchors[memberTrack.id] = memberTrack.clips.empty()
                                              ? std::string()
                                              : memberTrack.clips.back().id;
                for (const ClipModel& member : memberTrack.clips) {
                    if (member.patternClipId != clipId) continue;
                    const double memberEnd =
                        member.startSeconds + member.durationSeconds;
                    if (memberEnd <= atSeconds) continue;
                    if (member.startSeconds >= atSeconds) {
                        appendCommand(batch, collab::SetClipPatternOwner{
                            memberTrack.id, member.id, rightPattern.id});
                        continue;
                    }

                    const double memberLeftDuration =
                        atSeconds - member.startSeconds;
                    const double cutBeats = secondsToBeats(
                        memberLeftDuration, m_project.tempo);
                    ClipModel leftMember = member;
                    ClipModel rightMember = member;
                    leftMember.durationSeconds = memberLeftDuration;
                    leftMember.notes.clear();
                    rightMember.notes.clear();
                    for (const NoteModel& note : member.notes) {
                        const double noteEnd =
                            note.startBeats + note.lengthBeats;
                        if (note.startBeats >= cutBeats) {
                            NoteModel moved = note;
                            moved.startBeats -= cutBeats;
                            rightMember.notes.push_back(std::move(moved));
                        } else if (noteEnd <= cutBeats) {
                            leftMember.notes.push_back(note);
                        } else {
                            NoteModel leftNote = note;
                            leftNote.lengthBeats =
                                cutBeats - note.startBeats;
                            if (leftNote.lengthBeats > 0.0)
                                leftMember.notes.push_back(
                                    std::move(leftNote));
                            NoteModel rightNote = note;
                            rightNote.startBeats = 0.0;
                            rightNote.lengthBeats = noteEnd - cutBeats;
                            if (rightNote.lengthBeats > 0.0)
                                rightMember.notes.push_back(
                                    std::move(rightNote));
                        }
                    }
                    for (std::size_t lane = 0; lane < member.lanes.size();
                         ++lane) {
                        leftMember.lanes[lane].points.clear();
                        rightMember.lanes[lane].points.clear();
                        for (const AutomationPoint& point :
                             member.lanes[lane].points) {
                            if (point.beats < cutBeats) {
                                leftMember.lanes[lane].points.push_back(point);
                            } else {
                                AutomationPoint moved = point;
                                moved.beats -= cutBeats;
                                rightMember.lanes[lane].points.push_back(
                                    std::move(moved));
                            }
                        }
                    }
                    rightMember.patternClipId = rightPattern.id;
                    rightMember.startSeconds = atSeconds;
                    rightMember.durationSeconds =
                        memberEnd - atSeconds;
                    rightMember.offsetSeconds = member.offsetSeconds +
                        memberLeftDuration /
                            std::max(member.sampleEdit.stretchTime, 0.01);
                    mintClipIdentities(rightMember, {}, true);

                    appendCommand(batch, collab::SetClipProperty{
                        memberTrack.id, member.id,
                        collab::ClipProperty::DurationSeconds,
                        memberLeftDuration});
                    if (!appendMidiClipContentsDiff(
                            batch, memberTrack.id, member, leftMember) ||
                        !appendSharedClip(batch, memberTrack.id, rightMember,
                                          anchors[memberTrack.id])) {
                        return {};
                    }
                    anchors[memberTrack.id] = rightMember.id;
                }
            }
            if (!sharedBatchApplies(m_project, batch)) return {};
            const auto result = submitSharedMutation(
                collab::CommandBody{std::move(batch)},
                "Split Pattern Clip");
            return result == collab::SharedMutationResult::Submitted
                       ? rightPattern.id
                       : std::string{};
        }
        const ClipModel original = *found;
        double effectiveDuration = original.durationSeconds;
        if (effectiveDuration <= 0.0 && original.asset.sampleRate > 0.0 &&
            original.asset.frames > 0) {
            effectiveDuration =
                double(original.asset.frames) / original.asset.sampleRate -
                original.offsetSeconds;
        }
        if (effectiveDuration <= 0.0) return {};
        const double clipEnd = original.startSeconds + effectiveDuration;
        if (atSeconds <= original.startSeconds + kMinClipSeconds ||
            atSeconds >= clipEnd - kMinClipSeconds) {
            return {};
        }

        const double leftDuration = atSeconds - original.startSeconds;
        ClipModel left = original;
        ClipModel right = original;
        left.durationSeconds = leftDuration;
        right.startSeconds = atSeconds;
        right.offsetSeconds = original.offsetSeconds +
            leftDuration / std::max(original.sampleEdit.stretchTime, 0.01);
        right.durationSeconds = effectiveDuration - leftDuration;
        if (original.kind == ClipKind::Audio) {
            left.musicalAnalysis = {};
            right.musicalAnalysis = {};
        }

        if (original.kind == ClipKind::Midi) {
            const double cutBeats =
                secondsToBeats(leftDuration, m_project.tempo);
            left.notes.clear();
            right.notes.clear();
            for (const NoteModel& note : original.notes) {
                const double noteEnd = note.startBeats + note.lengthBeats;
                if (note.startBeats >= cutBeats) {
                    NoteModel moved = note;
                    moved.startBeats -= cutBeats;
                    right.notes.push_back(std::move(moved));
                } else if (noteEnd <= cutBeats) {
                    left.notes.push_back(note);
                } else {
                    NoteModel leftNote = note;
                    leftNote.lengthBeats = cutBeats - note.startBeats;
                    if (leftNote.lengthBeats > 0.0)
                        left.notes.push_back(std::move(leftNote));
                    NoteModel rightNote = note;
                    rightNote.startBeats = 0.0;
                    rightNote.lengthBeats = noteEnd - cutBeats;
                    if (rightNote.lengthBeats > 0.0)
                        right.notes.push_back(std::move(rightNote));
                }
            }
            for (std::size_t lane = 0; lane < original.lanes.size(); ++lane) {
                left.lanes[lane].points.clear();
                right.lanes[lane].points.clear();
                for (const AutomationPoint& point :
                     original.lanes[lane].points) {
                    if (point.beats < cutBeats) {
                        left.lanes[lane].points.push_back(point);
                    } else {
                        AutomationPoint moved = point;
                        moved.beats -= cutBeats;
                        right.lanes[lane].points.push_back(std::move(moved));
                    }
                }
            }
        }

        if (original.kind == ClipKind::Automation) {
            const double cutBeats =
                secondsToBeats(leftDuration, m_project.tempo);
            const double atCut = automationValueAt(
                original.automation.points, cutBeats,
                original.automation.defaultValue);
            left.automation.points.clear();
            right.automation.points.clear();
            right.automation.defaultValue = atCut;
            for (const AutomationPoint& point : original.automation.points) {
                if (point.beats < cutBeats) {
                    left.automation.points.push_back(point);
                } else {
                    AutomationPoint moved = point;
                    moved.beats -= cutBeats;
                    right.automation.points.push_back(std::move(moved));
                }
            }
            AutomationPoint seam;
            seam.id = newUuid();
            seam.beats = cutBeats;
            seam.value = atCut;
            if (!left.automation.points.empty())
                seam.shape = left.automation.points.back().shape;
            left.automation.points.push_back(seam);
            AutomationPoint head = seam;
            head.id = newUuid();
            head.beats = 0.0;
            right.automation.points.insert(right.automation.points.begin(),
                                           std::move(head));
            normalizeAutomation(left.automation.points);
            normalizeAutomation(right.automation.points);
        }

        mintClipIdentities(right, {}, true);
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::DurationSeconds,
            leftDuration});
        if (original.kind == ClipKind::Audio) {
            appendCommand(batch, collab::SetClipMusicalAnalysis{
                trackId, clipId, {}});
        }
        if (original.kind == ClipKind::Midi) {
            const std::unordered_set<std::string> leftNoteIds = [&] {
                std::unordered_set<std::string> ids;
                for (const NoteModel& note : left.notes) ids.insert(note.id);
                return ids;
            }();
            for (const NoteModel& note : original.notes) {
                if (!leftNoteIds.contains(note.id)) {
                    appendCommand(batch, collab::DeleteMidiNote{
                        trackId, clipId, note.id});
                }
            }
            std::string noteAnchor;
            for (const NoteModel& note : left.notes) {
                appendCommand(batch, collab::UpsertMidiNote{
                    trackId, clipId, note, noteAnchor});
                noteAnchor = note.id;
            }
            for (std::size_t lane = 0; lane < original.lanes.size(); ++lane) {
                const std::string& laneId = original.lanes[lane].id;
                std::unordered_set<std::string> leftPointIds;
                for (const AutomationPoint& point : left.lanes[lane].points)
                    leftPointIds.insert(point.id);
                for (const AutomationPoint& point :
                     original.lanes[lane].points) {
                    if (!leftPointIds.contains(point.id)) {
                        appendCommand(batch, collab::DeleteAutomationPoint{
                            trackId, clipId, laneId, point.id});
                    }
                }
                std::string pointAnchor;
                for (const AutomationPoint& point : left.lanes[lane].points) {
                    appendCommand(batch, collab::UpsertAutomationPoint{
                        trackId, clipId, laneId, point, pointAnchor});
                    pointAnchor = point.id;
                }
            }
        }
        if (original.kind == ClipKind::Automation) {
            std::unordered_set<std::string> leftPointIds;
            for (const AutomationPoint& point : left.automation.points)
                leftPointIds.insert(point.id);
            for (const AutomationPoint& point : original.automation.points) {
                if (!leftPointIds.contains(point.id)) {
                    appendCommand(batch, collab::DeleteAutomationPoint{
                        trackId, clipId, {}, point.id});
                }
            }
            std::string pointAnchor;
            for (const AutomationPoint& point : left.automation.points) {
                appendCommand(batch, collab::UpsertAutomationPoint{
                    trackId, clipId, {}, point, pointAnchor});
                pointAnchor = point.id;
            }
        }
        if (!appendSharedClip(
                batch, trackId, right,
                track->clips.empty() ? std::string()
                                     : track->clips.back().id) ||
            !sharedBatchApplies(m_project, batch)) {
            return {};
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Split Clip");
        return result == collab::SharedMutationResult::Submitted ? right.id
                                                                 : std::string{};
    }

    auto patternIt = std::find_if(track->clips.begin(), track->clips.end(),
                                  [&](const ClipModel& clip) {
                                      return clip.id == clipId &&
                                             clip.kind == ClipKind::Pattern;
                                  });
    if (patternIt != track->clips.end()) {
        const ClipModel original = *patternIt;
        const double clipStart = original.startSeconds;
        const double clipEnd = clipStart + original.durationSeconds;
        if (original.durationSeconds <= 0.0 ||
            atSeconds <= clipStart + kMinClipSeconds ||
            atSeconds >= clipEnd - kMinClipSeconds) {
            return {};
        }

        struct PatternMembershipChange {
            std::string trackId;
            std::string clipId;
            std::string beforeOwnerId;
            std::string afterOwnerId;
        };
        struct PatternSplitDelta {
            PatternClipEndpointDelta clips;
            std::vector<PatternMembershipChange> memberships;
            std::vector<std::string> publishTracks;
            bool graphDirty = false;
        };
        auto delta = std::make_shared<PatternSplitDelta>();
        const std::size_t patternIndex =
            std::size_t(patternIt - track->clips.begin());
        rememberPatternClip(
            delta->clips, false,
            IndexedPatternClip{trackId, patternIndex, original});
        const double leftDuration = atSeconds - clipStart;

        ClipModel leftPattern = original;
        leftPattern.durationSeconds = leftDuration;
        ClipModel rightPattern = original;
        rightPattern.id = newUuid();
        rightPattern.startSeconds = atSeconds;
        rightPattern.durationSeconds = clipEnd - atSeconds;
        rightPattern.offsetSeconds = original.offsetSeconds + leftDuration;
        for (InsertModel& insert : rightPattern.inserts) insert.id = newUuid();
        *patternIt = leftPattern;
        track->clips.push_back(rightPattern);
        const std::string rightPatternId = rightPattern.id;
        rememberPatternClip(
            delta->clips, true,
            IndexedPatternClip{trackId, patternIndex, leftPattern});
        rememberPatternClip(
            delta->clips, true,
            IndexedPatternClip{trackId, track->clips.size() - 1,
                               rightPattern});

        // Split/reassign every owned child clip at the same absolute instant.
        // A note crossing the knife becomes two clipped notes, so neither half
        // leaks beyond the Pattern clip that owns it.
        for (TrackModel& memberTrack : m_project.tracks) {
            const std::size_t count = memberTrack.clips.size();
            for (std::size_t i = 0; i < count; ++i) {
                ClipModel& member = memberTrack.clips[i];
                if (member.patternClipId != clipId) continue;
                const double memberStart = member.startSeconds;
                const double memberEnd = memberStart + member.durationSeconds;
                if (memberEnd <= atSeconds) continue;
                if (memberStart >= atSeconds) {
                    delta->memberships.push_back(PatternMembershipChange{
                        memberTrack.id, member.id, clipId, rightPatternId});
                    member.patternClipId = rightPatternId;
                    appendUniqueTrack(delta->publishTracks, memberTrack.id);
                    continue;
                }

                const ClipModel memberOriginal = member;
                rememberPatternClip(
                    delta->clips, false,
                    IndexedPatternClip{memberTrack.id, i, memberOriginal});
                const double memberLeftDuration = atSeconds - memberStart;
                const double cutBeats =
                    secondsToBeats(memberLeftDuration, m_project.tempo);

                ClipModel leftMember = memberOriginal;
                ClipModel rightMember = memberOriginal;
                rightMember.id = newUuid();
                rightMember.patternClipId = rightPatternId;
                rightMember.startSeconds = atSeconds;
                rightMember.durationSeconds = memberEnd - atSeconds;
                rightMember.offsetSeconds = memberOriginal.offsetSeconds +
                    memberLeftDuration /
                        std::max(memberOriginal.sampleEdit.stretchTime, 0.01);
                for (InsertModel& insert : rightMember.inserts)
                    insert.id = newUuid();

                std::vector<NoteModel> leftNotes;
                rightMember.notes.clear();
                for (const NoteModel& note : memberOriginal.notes) {
                    const double noteEnd = note.startBeats + note.lengthBeats;
                    if (note.startBeats >= cutBeats) {
                        NoteModel moved = note;
                        moved.id = newUuid();
                        moved.startBeats -= cutBeats;
                        rightMember.notes.push_back(std::move(moved));
                    } else if (noteEnd <= cutBeats) {
                        leftNotes.push_back(note);
                    } else {
                        NoteModel left = note;
                        left.lengthBeats = cutBeats - note.startBeats;
                        if (left.lengthBeats > 0.0)
                            leftNotes.push_back(std::move(left));
                        NoteModel chased = note;
                        chased.id = newUuid();
                        chased.startBeats = 0.0;
                        chased.lengthBeats = noteEnd - cutBeats;
                        if (chased.lengthBeats > 0.0)
                            rightMember.notes.push_back(std::move(chased));
                    }
                }

                std::vector<ControllerLane> leftLanes = memberOriginal.lanes;
                rightMember.lanes = memberOriginal.lanes;
                for (std::size_t lane = 0; lane < leftLanes.size(); ++lane) {
                    leftLanes[lane].points.clear();
                    rightMember.lanes[lane].id = newUuid();
                    rightMember.lanes[lane].points.clear();
                    for (const AutomationPoint& point :
                         memberOriginal.lanes[lane].points) {
                        if (point.beats < cutBeats) {
                            leftLanes[lane].points.push_back(point);
                        } else {
                            AutomationPoint moved = point;
                            moved.beats -= cutBeats;
                            rightMember.lanes[lane].points.push_back(moved);
                        }
                    }
                }

                leftMember.durationSeconds = memberLeftDuration;
                leftMember.notes = std::move(leftNotes);
                leftMember.lanes = std::move(leftLanes);
                member = leftMember;
                const std::size_t rightIndex = memberTrack.clips.size();
                memberTrack.clips.push_back(rightMember);
                rememberPatternClip(
                    delta->clips, true,
                    IndexedPatternClip{memberTrack.id, i,
                                       std::move(leftMember)});
                rememberPatternClip(
                    delta->clips, true,
                    IndexedPatternClip{memberTrack.id, rightIndex,
                                       std::move(rightMember)});
                appendUniqueTrack(delta->publishTracks, memberTrack.id);
                delta->graphDirty |= patternClipNeedsGraphRebuild(
                    delta->clips.after.back().clip);
            }
        }

        auto publish = [this, delta] {
            if (delta->graphDirty) {
                rebuildGraph();
            } else {
                for (const std::string& memberTrackId :
                     delta->publishTracks) {
                    TrackModel* memberTrack =
                        m_project.findTrack(memberTrackId);
                    if (!memberTrack) continue;
                    syncTrackClips(*memberTrack);
                    if (trackAccepts(memberTrack->kind, ClipKind::Midi)) {
                        syncTrackNotes(*memberTrack);
                        syncTrackAutomation(*memberTrack);
                    }
                }
            }
            updateTimelineDuration();
        };
        auto apply = [this, delta, publish](bool useAfter) {
            restorePatternClipEndpoint(m_project, delta->clips, useAfter);
            for (const PatternMembershipChange& change :
                 delta->memberships) {
                ClipModel* member = findPatternDeltaClip(
                    m_project, change.trackId, change.clipId);
                if (!member) continue;
                const std::string& expected =
                    useAfter ? change.beforeOwnerId : change.afterOwnerId;
                const std::string& desired =
                    useAfter ? change.afterOwnerId : change.beforeOwnerId;
                // Do not trample a later explicit re-parenting of this clip.
                if (member->patternClipId == expected)
                    member->patternClipId = desired;
            }
            publish();
        };

        publish();
        m_undo.push("Split Pattern Clip", [apply] { apply(false); },
                    [apply] { apply(true); });
        return rightPatternId;
    }

    for (size_t i = 0; i < track->clips.size(); ++i) {
        if (track->clips[i].id != clipId) continue;
        const ClipModel original = track->clips[i];

        // Resolve the effective length: a duration of 0 means "to end of file".
        double effectiveDuration = original.durationSeconds;
        if (effectiveDuration <= 0.0) {
            if (auto samples = loadSamples(original.filePath);
                samples && samples->sampleRate() > 0.0) {
                effectiveDuration =
                    double(samples->frames()) / samples->sampleRate() -
                    original.offsetSeconds;
            }
        }
        if (effectiveDuration <= 0.0) return {};

        const double clipStart = original.startSeconds;
        const double clipEnd = clipStart + effectiveDuration;
        // The cut has to leave a viable clip on both sides.
        if (atSeconds <= clipStart + kMinClipSeconds ||
            atSeconds >= clipEnd - kMinClipSeconds) {
            return {};
        }

        const double leftDuration = atSeconds - clipStart;

        ClipModel right = original;
        right.id = newUuid();
        for (InsertModel& insert : right.inserts) insert.id = newUuid();
        right.startSeconds = atSeconds;
        right.offsetSeconds = original.offsetSeconds +
            leftDuration / std::max(original.sampleEdit.stretchTime, 0.01);
        right.durationSeconds = effectiveDuration - leftDuration;
        if (original.kind == ClipKind::Audio)
            right.musicalAnalysis = {};

        // A MIDI clip's payload is its notes, and `right` started as a whole
        // copy — without this both halves would keep every note. Notes are
        // timed from their own clip's start, so the right half's are rebased.
        std::vector<NoteModel> leftNotes = original.notes;
        std::vector<ControllerLane> leftLanes = original.lanes;
        if (original.kind == ClipKind::Midi) {
            const double cutBeats =
                secondsToBeats(leftDuration, m_project.tempo);
            right.notes.clear();
            leftNotes.clear();
            for (const auto& note : original.notes) {
                const double noteEnd = note.startBeats + note.lengthBeats;
                if (note.startBeats >= cutBeats) {
                    NoteModel moved = note;
                    moved.id = newUuid();     // the halves must not share ids
                    moved.startBeats = note.startBeats - cutBeats;
                    right.notes.push_back(std::move(moved));
                } else if (noteEnd <= cutBeats) {
                    leftNotes.push_back(note);
                } else {
                    NoteModel left = note;
                    left.lengthBeats = cutBeats - note.startBeats;
                    if (left.lengthBeats > 0.0)
                        leftNotes.push_back(std::move(left));
                    NoteModel chased = note;
                    chased.id = newUuid();
                    chased.startBeats = 0.0;
                    chased.lengthBeats = noteEnd - cutBeats;
                    if (chased.lengthBeats > 0.0)
                        right.notes.push_back(std::move(chased));
                }
            }

            right.lanes = original.lanes;
            for (std::size_t lane = 0; lane < leftLanes.size(); ++lane) {
                leftLanes[lane].points.clear();
                right.lanes[lane].id = newUuid();
                right.lanes[lane].points.clear();
                for (const AutomationPoint& point : original.lanes[lane].points) {
                    if (point.beats < cutBeats) {
                        leftLanes[lane].points.push_back(point);
                    } else {
                        AutomationPoint moved = point;
                        moved.beats -= cutBeats;
                        right.lanes[lane].points.push_back(moved);
                    }
                }
            }
        }

        // An automation clip's payload is its curve, and both halves must sound
        // exactly as the whole did. That takes a breakpoint *at* the cut on
        // each side: without one the left half ends at its last surviving
        // point and the right half starts from its default, so cutting a curve
        // would change it.
        ClipAutomationModel leftAutomation = original.automation;
        if (original.kind == ClipKind::Automation) {
            const double cutBeats = secondsToBeats(leftDuration, m_project.tempo);
            const double atCut = automationValueAt(original.automation.points,
                                                   cutBeats,
                                                   original.automation.defaultValue);
            leftAutomation.points.clear();
            right.automation = original.automation;
            right.automation.points.clear();
            // The right half opens holding the value the curve had at the knife.
            right.automation.defaultValue = atCut;

            for (const AutomationPoint& point : original.automation.points) {
                if (point.beats < cutBeats) {
                    leftAutomation.points.push_back(point);
                } else {
                    AutomationPoint moved = point;
                    moved.beats -= cutBeats;
                    right.automation.points.push_back(moved);
                }
            }
            AutomationPoint seam;
            seam.beats = cutBeats;
            seam.value = atCut;
            // The seam inherits the shape it is cutting through, so the left
            // half's last segment keeps the curve it had.
            if (!leftAutomation.points.empty())
                seam.shape = leftAutomation.points.back().shape;
            leftAutomation.points.push_back(seam);
            AutomationPoint head = seam;
            head.beats = 0.0;
            right.automation.points.insert(right.automation.points.begin(), head);
            normalizeAutomation(leftAutomation.points);
            normalizeAutomation(right.automation.points);
        }

        track->clips[i].durationSeconds = leftDuration;
        if (original.kind == ClipKind::Audio)
            track->clips[i].musicalAnalysis = {};
        track->clips[i].notes = leftNotes;
        track->clips[i].lanes = leftLanes;
        track->clips[i].automation = leftAutomation;
        track->clips.push_back(right);
        if (original.inserts.empty()) syncTrackClips(*track);
        else rebuildGraph();
        if (original.kind == ClipKind::Midi) {
            syncTrackNotes(*track);
            syncTrackAutomation(*track);
        }
        if (original.kind == ClipKind::Automation) syncAllAutomation();
        updateTimelineDuration();

        const std::string rightId = right.id;
        // Deterministic undo/redo: restore the whole clip / re-cut it, using the
        // exact before and after state captured here (not by re-running split,
        // which would mint a different id each time).
        m_undo.push(
            "Split Clip",
            [this, trackId, original, rightId] {
                if (auto* t = m_project.findTrack(trackId)) {
                    std::erase_if(t->clips, [&](const ClipModel& c) {
                        return c.id == rightId;
                    });
                    for (auto& c : t->clips) {
                        if (c.id == original.id) { c = original; break; }
                    }
                    if (original.inserts.empty()) syncTrackClips(*t);
                    else rebuildGraph();
                    if (original.kind == ClipKind::Midi) {
                        syncTrackNotes(*t);
                        syncTrackAutomation(*t);
                    }
                    if (original.kind == ClipKind::Automation) syncAllAutomation();
                    updateTimelineDuration();
                }
            },
            [this, trackId, original, right, leftDuration, leftNotes, leftLanes,
             leftAutomation] {
                if (auto* t = m_project.findTrack(trackId)) {
                    bool haveRight = false;
                    for (auto& c : t->clips) {
                        if (c.id == original.id) {
                            c.durationSeconds = leftDuration;
                            // Undo restored every note to the left half; the
                            // re-cut has to take the right half's back out.
                            c.notes = leftNotes;
                            c.lanes = leftLanes;
                            c.automation = leftAutomation;
                        }
                        if (c.id == right.id) haveRight = true;
                    }
                    if (!haveRight) t->clips.push_back(right);
                    if (original.inserts.empty()) syncTrackClips(*t);
                    else rebuildGraph();
                    if (original.kind == ClipKind::Midi) {
                        syncTrackNotes(*t);
                        syncTrackAutomation(*t);
                    }
                    if (original.kind == ClipKind::Automation) syncAllAutomation();
                    updateTimelineDuration();
                }
            });
        return rightId;
    }
    return {};
}

void EngineController::removeClip(const std::string& trackId,
                                  const std::string& clipId) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    ClipModel snapshot;
    bool found = false;
    for (const auto& clip : track->clips) {
        if (clip.id == clipId) {
            snapshot = clip;
            found = true;
        }
    }
    if (!found) return;

    if (cloudProjectBound()) {
        if (snapshot.kind != ClipKind::Pattern) {
            (void)submitSharedMutation(collab::DeleteClip{trackId, clipId},
                                       "Remove Clip");
            return;
        }
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const TrackModel& memberTrack : m_project.tracks) {
            for (const ClipModel& member : memberTrack.clips) {
                if (member.patternClipId == clipId)
                    appendCommand(batch, collab::DeleteClip{
                        memberTrack.id, member.id});
            }
        }
        appendCommand(batch, collab::DeleteClip{trackId, clipId});
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Remove Pattern Clip");
        return;
    }

    if (snapshot.kind == ClipKind::Pattern) {
        struct PatternRemoveDelta {
            PatternClipEndpointDelta clips;
            std::vector<std::string> publishTracks;
            bool graphDirty = false;
        };
        auto delta = std::make_shared<PatternRemoveDelta>();
        const auto parent = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const ClipModel& clip) { return clip.id == clipId; });
        rememberPatternClip(
            delta->clips, false,
            IndexedPatternClip{
                trackId, std::size_t(parent - track->clips.begin()), snapshot});
        for (TrackModel& memberTrack : m_project.tracks) {
            for (std::size_t index = 0; index < memberTrack.clips.size();
                 ++index) {
                const ClipModel& member = memberTrack.clips[index];
                if (member.patternClipId != clipId) continue;
                delta->graphDirty |= patternClipNeedsGraphRebuild(member);
                rememberPatternClip(
                    delta->clips, false,
                    IndexedPatternClip{memberTrack.id, index, member});
                appendUniqueTrack(delta->publishTracks, memberTrack.id);
            }
        }

        auto publish = [this, delta] {
            if (delta->graphDirty) {
                rebuildGraph();
            } else {
                for (const std::string& memberTrackId :
                     delta->publishTracks) {
                    TrackModel* memberTrack =
                        m_project.findTrack(memberTrackId);
                    if (!memberTrack) continue;
                    syncTrackClips(*memberTrack);
                    if (trackAccepts(memberTrack->kind, ClipKind::Midi)) {
                        syncTrackNotes(*memberTrack);
                        syncTrackAutomation(*memberTrack);
                    }
                }
            }
            updateTimelineDuration();
        };
        auto apply = [this, delta, publish](bool removed) {
            restorePatternClipEndpoint(m_project, delta->clips, removed);
            if (removed) {
                for (const auto& [trackId, ids] : delta->clips.ids) {
                    (void)trackId;
                    for (const std::string& id : ids)
                        m_clipSampleCache.erase(id);
                }
            }
            publish();
            if (removed) pruneDecodedSampleCache();
        };

        apply(true);
        m_undo.push("Remove Pattern Clip", [apply] { apply(false); },
                    [apply] { apply(true); });
        return;
    }

    std::erase_if(track->clips, [&](const ClipModel& c) { return c.id == clipId; });
    // A baked sample outlives nothing: the clip it belonged to is gone, and the
    // cache is keyed by clip id, so without this a long session leaks one full
    // copy of every clip that ever carried a precomputed effect.
    m_clipSampleCache.erase(clipId);
    if (snapshot.inserts.empty()) syncTrackClips(*track);
    else rebuildGraph();
    if (snapshot.kind == ClipKind::Midi) {
        syncTrackNotes(*track);
        syncTrackAutomation(*track);
    }
    if (snapshot.kind == ClipKind::Automation) syncAllAutomation();
    updateTimelineDuration();
    pruneDecodedSampleCache();

    m_undo.push("Remove Clip",
                [this, trackId, snapshot] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->clips.push_back(snapshot);
                        if (snapshot.inserts.empty()) syncTrackClips(*t);
                        else rebuildGraph();
                        if (snapshot.kind == ClipKind::Midi) {
                            syncTrackNotes(*t);
                            syncTrackAutomation(*t);
                        }
                        updateTimelineDuration();
                    }
                },
                [this, trackId, clipId] { removeClip(trackId, clipId); });
}

void EngineController::setClipMuted(const std::string& trackId,
                                    const std::string& clipId, bool muted) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    const auto target = std::find_if(track->clips.begin(), track->clips.end(),
                                     [&](const ClipModel& clip) {
                                         return clip.id == clipId;
                                     });
    if (target == track->clips.end() || target->muted == muted) return;
    if (cloudProjectBound()) {
        if (target->kind != ClipKind::Pattern) {
            (void)submitSharedMutation(
                collab::SetClipProperty{trackId, clipId,
                                        collab::ClipProperty::Muted, muted},
                muted ? "Mute Clip" : "Unmute Clip");
            return;
        }
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::Muted, muted});
        for (const TrackModel& memberTrack : m_project.tracks) {
            for (const ClipModel& member : memberTrack.clips) {
                if (member.patternClipId == clipId) {
                    appendCommand(batch, collab::SetClipProperty{
                        memberTrack.id, member.id,
                        collab::ClipProperty::Muted, muted});
                }
            }
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   muted ? "Mute Pattern Clip"
                                         : "Unmute Pattern Clip");
        return;
    }
    if (target != track->clips.end() && target->kind == ClipKind::Pattern) {
        if (target->muted == muted) return;
        struct PatternMuteChange {
            std::string trackId;
            std::string clipId;
            bool before = false;
            bool after = false;
        };
        struct PatternMuteDelta {
            std::vector<PatternMuteChange> changes;
            std::vector<std::string> publishTracks;
        };
        auto delta = std::make_shared<PatternMuteDelta>();
        delta->changes.push_back(
            PatternMuteChange{trackId, clipId, target->muted, muted});
        for (TrackModel& memberTrack : m_project.tracks) {
            for (ClipModel& member : memberTrack.clips) {
                if (member.patternClipId != clipId) continue;
                delta->changes.push_back(PatternMuteChange{
                    memberTrack.id, member.id, member.muted, muted});
                appendUniqueTrack(delta->publishTracks, memberTrack.id);
            }
        }

        auto apply = [this, delta](bool useAfter) {
            for (const PatternMuteChange& change : delta->changes) {
                if (ClipModel* clip = findPatternDeltaClip(
                        m_project, change.trackId, change.clipId)) {
                    clip->muted = useAfter ? change.after : change.before;
                }
            }
            for (const std::string& memberTrackId :
                 delta->publishTracks) {
                TrackModel* memberTrack =
                    m_project.findTrack(memberTrackId);
                if (!memberTrack) continue;
                syncTrackClips(*memberTrack);
                if (trackAccepts(memberTrack->kind, ClipKind::Midi)) {
                    syncTrackNotes(*memberTrack, false);
                    syncTrackAutomation(*memberTrack);
                }
            }
        };
        apply(true);
        m_undo.push(muted ? "Mute Pattern Clip" : "Unmute Pattern Clip",
                    [apply] { apply(false); },
                    [apply] { apply(true); });
        return;
    }
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;
        if (clip.muted == muted) return;
        clip.muted = muted;
        syncTrackClips(*track);
        if (clip.kind == ClipKind::Midi) {
            syncTrackNotes(*track);
            syncTrackAutomation(*track);
        }

        auto set = [this, trackId, clipId](bool value) {
            auto* t = m_project.findTrack(trackId);
            if (!t) return;
            for (auto& c : t->clips) {
                if (c.id != clipId) continue;
                c.muted = value;
                syncTrackClips(*t);
                if (c.kind == ClipKind::Midi) {
                    syncTrackNotes(*t);
                    syncTrackAutomation(*t);
                }
                return;
            }
        };
        m_undo.push("Mute Clip", [set, was = !muted] { set(was); },
                    [set, muted] { set(muted); });
        return;
    }
}

void EngineController::setClipName(const std::string& trackId,
                                   const std::string& clipId,
                                   const std::string& name) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return;
    for (auto& clip : track->clips) {
        if (clip.id != clipId) continue;
        if (clip.name == name) return;
        const auto shared = submitSharedMutation(
            collab::SetClipProperty{trackId, clipId,
                                    collab::ClipProperty::Name, name},
            "Rename Clip");
        if (shared != collab::SharedMutationResult::LocalFallback) return;
        const std::string before = clip.name;
        clip.name = name;

        // Display-only: nothing in the engine reads a clip's name, so unlike
        // the other clip edits this one doesn't resync the player.
        auto set = [this, trackId, clipId](const std::string& value) {
            auto* t = m_project.findTrack(trackId);
            if (!t) return;
            for (auto& c : t->clips) {
                if (c.id == clipId) { c.name = value; return; }
            }
        };
        m_undo.push("Rename Clip", [set, before] { set(before); },
                    [set, name] { set(name); });
        return;
    }
}

std::string EngineController::duplicateClip(const std::string& trackId,
                                            const std::string& clipId) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return {};

    const ClipModel* source = nullptr;
    for (const auto& clip : track->clips) {
        if (clip.id == clipId) { source = &clip; break; }
    }
    if (!source) return {};

    // Where the copy goes: butted up against the end of the original, so a
    // duplicate reads as a repeat. A stored duration of 0 means "to end of
    // file", so resolve the real length first.
    double length = source->durationSeconds;
    if (length <= 0.0 && source->asset.sampleRate > 0.0 &&
        source->asset.frames > 0) {
        length = double(source->asset.frames) / source->asset.sampleRate -
                 source->offsetSeconds;
    }
    if (length <= 0.0) {
        if (auto samples = loadSamples(source->filePath);
            samples && samples->sampleRate() > 0.0) {
            length = double(samples->frames()) / samples->sampleRate() -
                     source->offsetSeconds;
        }
    }
    if (length <= 0.0) length = kMinClipSeconds;

    return duplicateClipAt(trackId, clipId, source->startSeconds + length);
}

std::string EngineController::duplicateClipAt(const std::string& trackId,
                                              const std::string& clipId,
                                              double startSeconds) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return {};
    const ClipModel* source = nullptr;
    for (const auto& clip : track->clips) {
        if (clip.id == clipId) { source = &clip; break; }
    }
    if (!source) return {};
    if (cloudProjectBound()) {
        const double targetStart = std::max(0.0, startSeconds);
        auto batch = std::make_shared<collab::BatchCommand>();
        ClipModel parentCopy = *source;
        mintClipIdentities(parentCopy, {}, true);
        parentCopy.startSeconds = targetStart;
        if (source->kind != ClipKind::Pattern) {
            const std::string anchor = track->clips.empty()
                                           ? std::string()
                                           : track->clips.back().id;
            if (!appendSharedClip(batch, trackId, parentCopy, anchor) ||
                !sharedBatchApplies(m_project, batch)) {
                return {};
            }
            const auto result = submitSharedMutation(
                collab::CommandBody{std::move(batch)}, "Duplicate Clip");
            return result == collab::SharedMutationResult::Submitted
                       ? parentCopy.id
                       : std::string{};
        }

        if (track->kind != TrackKind::Pattern) return {};
        parentCopy.patternClipId.clear();
        parentCopy.name = source->name.empty() ? track->name : source->name;
        const double delta = targetStart - source->startSeconds;
        const std::unordered_set<std::string> descendants = [&] {
            const auto ids = subtreeOf(m_project, trackId);
            return std::unordered_set<std::string>(ids.begin(), ids.end());
        }();
        if (!appendSharedClip(
                batch, trackId, parentCopy,
                track->clips.empty() ? std::string()
                                     : track->clips.back().id)) {
            return {};
        }
        std::unordered_map<std::string, std::string> anchors;
        for (const TrackModel& memberTrack : m_project.tracks) {
            if (!descendants.contains(memberTrack.id)) continue;
            anchors[memberTrack.id] = memberTrack.clips.empty()
                                          ? std::string()
                                          : memberTrack.clips.back().id;
            for (const ClipModel& member : memberTrack.clips) {
                if (member.patternClipId != source->id ||
                    member.kind != ClipKind::Midi) {
                    continue;
                }
                ClipModel memberCopy = member;
                mintClipIdentities(memberCopy, {}, true);
                memberCopy.patternClipId = parentCopy.id;
                memberCopy.startSeconds =
                    std::max(0.0, member.startSeconds + delta);
                if (!appendSharedClip(batch, memberTrack.id, memberCopy,
                                      anchors[memberTrack.id])) {
                    return {};
                }
                anchors[memberTrack.id] = memberCopy.id;
            }
        }
        if (!sharedBatchApplies(m_project, batch)) return {};
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)},
            "Duplicate Pattern Clip");
        return result == collab::SharedMutationResult::Submitted
                   ? parentCopy.id
                   : std::string{};
    }
    if (source->kind == ClipKind::Pattern) {
        PatternClipMembers members;
        for (const TrackModel& memberTrack : m_project.tracks) {
            for (const ClipModel& member : memberTrack.clips) {
                if (member.patternClipId == source->id)
                    members.emplace_back(memberTrack.id, member);
            }
        }
        return insertPatternClipCopyImpl(trackId, *source, members,
                                         startSeconds,
                                         "Duplicate Pattern Clip");
    }
    return insertClipCopy(trackId, *source, startSeconds);
}

std::string EngineController::insertClipCopy(const std::string& trackId,
                                             const ClipModel& source,
                                             double startSeconds) {
    if (source.kind == ClipKind::Pattern) {
        PatternClipMembers members;
        for (const TrackModel& memberTrack : m_project.tracks) {
            for (const ClipModel& member : memberTrack.clips) {
                if (member.patternClipId == source.id)
                    members.emplace_back(memberTrack.id, member);
            }
        }
        return insertPatternClipCopyImpl(trackId, source, members,
                                         startSeconds,
                                         "Duplicate Pattern Clip");
    }
    auto* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, source.kind)) return {};

    if (cloudProjectBound()) {
        // Paste went straight into m_project and the legacy undo stack, which
        // is disabled under a cloud binding — so the pasted clip existed only
        // on this machine and could not be undone. duplicateClip has always
        // routed through the command path; this is the same shape.
        ClipModel copy = source;
        mintClipIdentities(copy, {}, true);
        copy.startSeconds = std::max(0.0, startSeconds);
        auto batch = std::make_shared<collab::BatchCommand>();
        if (!appendSharedClip(batch, trackId, copy,
                              track->clips.empty()
                                  ? std::string()
                                  : track->clips.back().id) ||
            !sharedBatchApplies(m_project, batch)) {
            return {};
        }
        const std::string pastedId = copy.id;
        return submitSharedMutation(collab::CommandBody{std::move(batch)},
                                    "Duplicate Clip") ==
                       collab::SharedMutationResult::Submitted
                   ? pastedId
                   : std::string{};
    }

    ClipModel copy = source;
    copy.id = newUuid();
    copy.startSeconds = std::max(0.0, startSeconds);
    for (InsertModel& insert : copy.inserts) insert.id = newUuid();
    // Notes are timed from their own clip's start, so they need no rebasing —
    // but the halves must not share ids.
    for (auto& note : copy.notes) note.id = newUuid();
    // Lanes too. They were the one thing this missed, so pasting a MIDI clip
    // produced two clips whose lanes shared ids — and the piano roll addresses
    // a lane by id. `insertPatternClipCopyImpl` has always minted these.
    for (ControllerLane& lane : copy.lanes) lane.id = newUuid();

    track->clips.push_back(copy);
    if (copy.inserts.empty()) syncTrackClips(*track);
    else rebuildGraph();
    if (copy.kind == ClipKind::Midi) {
        syncTrackNotes(*track);
        syncTrackAutomation(*track);
    }
    if (copy.kind == ClipKind::Automation) syncAllAutomation();
    updateTimelineDuration();

    const std::string newId = copy.id;
    m_undo.push("Duplicate Clip",
                [this, trackId, newId, copy] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        std::erase_if(t->clips, [&](const ClipModel& c) {
                            return c.id == newId;
                        });
                        if (copy.inserts.empty()) syncTrackClips(*t);
                        else rebuildGraph();
                        if (copy.kind == ClipKind::Midi) {
                            syncTrackNotes(*t);
                            syncTrackAutomation(*t);
                        }
                        if (copy.kind == ClipKind::Automation) syncAllAutomation();
                        updateTimelineDuration();
                    }
                },
                [this, trackId, copy] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->clips.push_back(copy);
                        if (copy.inserts.empty()) syncTrackClips(*t);
                        else rebuildGraph();
                        if (copy.kind == ClipKind::Midi) {
                            syncTrackNotes(*t);
                            syncTrackAutomation(*t);
                        }
                        if (copy.kind == ClipKind::Automation) syncAllAutomation();
                        updateTimelineDuration();
                    }
                });
    return newId;
}

std::string EngineController::insertPatternClipCopy(
    const std::string& patternTrackId, const ClipModel& source,
    const PatternClipMembers& members, double startSeconds) {
    return insertPatternClipCopyImpl(patternTrackId, source, members,
                                     startSeconds, "Paste Pattern Clip");
}

std::string EngineController::insertPatternClipCopyImpl(
    const std::string& patternTrackId, const ClipModel& source,
    const PatternClipMembers& members, double startSeconds,
    const std::string& undoLabel) {
    TrackModel* pattern = m_project.findTrack(patternTrackId);
    if (!pattern || pattern->kind != TrackKind::Pattern ||
        source.kind != ClipKind::Pattern) {
        return {};
    }

    struct PatternCopyDelta {
        PatternClipEndpointDelta clips;
        std::vector<std::string> publishTracks;
        bool graphDirty = false;
    };
    auto history = std::make_shared<PatternCopyDelta>();
    const double targetStart = std::max(0.0, startSeconds);
    const double delta = targetStart - source.startSeconds;

    auto mint = [](const ClipModel& original) {
        ClipModel copy = original;
        copy.id = newUuid();
        for (InsertModel& insert : copy.inserts) insert.id = newUuid();
        for (NoteModel& note : copy.notes) note.id = newUuid();
        for (ControllerLane& lane : copy.lanes) lane.id = newUuid();
        return copy;
    };

    ClipModel parentCopy = mint(source);
    parentCopy.patternClipId.clear();
    parentCopy.startSeconds = targetStart;
    parentCopy.name = source.name.empty() ? pattern->name : source.name;
    const std::string newPatternClipId = parentCopy.id;
    const std::size_t parentIndex = pattern->clips.size();
    pattern->clips.push_back(parentCopy);
    history->graphDirty |= patternClipNeedsGraphRebuild(parentCopy);
    rememberPatternClip(
        history->clips, true,
        IndexedPatternClip{patternTrackId, parentIndex, parentCopy});

    const std::unordered_set<std::string> descendants = [&] {
        const std::vector<std::string> ids = subtreeOf(m_project, patternTrackId);
        return std::unordered_set<std::string>(ids.begin(), ids.end());
    }();
    for (const auto& [memberTrackId, memberSource] : members) {
        if (!descendants.contains(memberTrackId) ||
            memberSource.kind != ClipKind::Midi) {
            continue;
        }
        TrackModel* memberTrack = m_project.findTrack(memberTrackId);
        if (!memberTrack ||
            !trackAccepts(memberTrack->kind, ClipKind::Midi)) {
            continue;
        }
        ClipModel memberCopy = mint(memberSource);
        memberCopy.patternClipId = newPatternClipId;
        memberCopy.startSeconds = std::max(0.0,
                                           memberSource.startSeconds + delta);
        const std::size_t memberIndex = memberTrack->clips.size();
        memberTrack->clips.push_back(memberCopy);
        history->graphDirty |= patternClipNeedsGraphRebuild(memberCopy);
        rememberPatternClip(
            history->clips, true,
            IndexedPatternClip{memberTrackId, memberIndex,
                               std::move(memberCopy)});
        appendUniqueTrack(history->publishTracks, memberTrackId);
    }

    auto publish = [this, history] {
        if (history->graphDirty) {
            rebuildGraph();
        } else {
            for (const std::string& memberTrackId :
                 history->publishTracks) {
                TrackModel* memberTrack =
                    m_project.findTrack(memberTrackId);
                if (!memberTrack) continue;
                syncTrackClips(*memberTrack);
                if (trackAccepts(memberTrack->kind, ClipKind::Midi)) {
                    syncTrackNotes(*memberTrack);
                    syncTrackAutomation(*memberTrack);
                }
            }
        }
        updateTimelineDuration();
    };
    auto apply = [this, history, publish](bool present) {
        restorePatternClipEndpoint(m_project, history->clips, present);
        if (!present) {
            for (const auto& [trackId, ids] : history->clips.ids) {
                (void)trackId;
                for (const std::string& id : ids)
                    m_clipSampleCache.erase(id);
            }
        }
        publish();
        if (!present) pruneDecodedSampleCache();
    };

    publish();
    m_undo.push(undoLabel, [apply] { apply(false); },
                [apply] { apply(true); });
    return newPatternClipId;
}

bool EngineController::setTrackInstrumentPlugin(
    const std::string& trackId, const plugins::PluginDescriptor& descriptor) {
    auto* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Midi)) return false;

    const InsertModel before = track->instrument;
    const SamplerFxModel beforeFx = track->samplerFx;
    InsertModel after;
    if (!descriptor.uid.empty()) {
        // The slot id survives a change of plugin, so the state file and any
        // automation written against this slot stay addressed to it.
        after.id = before.id.empty() ? newUuid() : before.id;
        after.bypassed = before.bypassed;
        applyDescriptor(after, descriptor);
    }

    SamplerFxModel afterFx;
    if (after.uid == "daw.sampler") {
        if (beforeFx.isOwnedBy(before) && before.uid == "daw.sampler") {
            afterFx = beforeFx;
        }
        afterFx.ownerInstrumentId = after.id;
    }

    if (cloudProjectBound()) {
        const collab::PluginLocation location{
            collab::PluginChain::Instrument, trackId, {}};
        collab::CommandBody body;
        if (before.id.empty() && after.id.empty()) return true;
        if (after.id.empty()) {
            body = collab::DeletePluginInsert{location, before.id};
        } else if (before.id.empty()) {
            if (after.format != PluginFormat::Internal ||
                after.uid != "daw.sampler") return false;
            body = collab::AddPluginInsert{location, after, {}};
        } else {
            if (before.format != PluginFormat::Internal ||
                after.format != PluginFormat::Internal ||
                after.uid != "daw.sampler") return false;
            body = collab::ReplacePluginInsert{location, before.id, after};
        }
        const auto result = submitSharedMutation(
            std::move(body), after.name.empty() ? "Remove Instrument"
                                                : "Load Instrument");
        return result == collab::SharedMutationResult::Submitted;
    }

    auto apply = [this, trackId](const InsertModel& value,
                                 const SamplerFxModel& fx) {
        if (auto* target = m_project.findTrack(trackId)) {
            target->instrument = value;
            target->samplerFx = fx;
            rebuildGraph();
        }
    };
    apply(after, afterFx);
    if (after.isLoaded() && !insertNode(trackId, after.id)) {
        // Do not leave a convincing but silent instrument slot in the model.
        // Restore the previous working instrument when initialize/activate
        // failed; the scanner's validation catches most cases, but not license
        // or machine-specific failures that happen only in the main process.
        apply(before, beforeFx);
        return false;
    }
    m_undo.push(after.name.empty() ? "Remove Instrument" : "Load Instrument",
                [apply, before, beforeFx] { apply(before, beforeFx); },
                [apply, after, afterFx] { apply(after, afterFx); });
    return true;
}

void EngineController::setTrackInstrument(const std::string& trackId,
                                           const std::string& name) {
    if (cloudProjectBound()) return;
    auto* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Midi)) return;

    const InsertModel before = track->instrument;
    const SamplerFxModel beforeFx = track->samplerFx;
    InsertModel after;
    if (!name.empty()) {
        // Keep the id across renames of the same slot, so a future host can
        // hold plugin state against it.
        after.id = before.id.empty() ? newUuid() : before.id;
        after.name = name;
        after.bypassed = before.bypassed;
    }
    if (before.name == after.name) return;
    track->instrument = after;
    track->samplerFx = {};

    const SamplerFxModel afterFx = track->samplerFx;

    m_undo.push("Set Instrument",
                [this, trackId, before, beforeFx] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->instrument = before;
                        t->samplerFx = beforeFx;
                    }
                    rebuildGraph();
                },
                [this, trackId, after, afterFx] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->instrument = after;
                        t->samplerFx = afterFx;
                    }
                    rebuildGraph();
                });
    rebuildGraph();
}

std::string EngineController::addMidiClip(const std::string& trackId,
                                          double startSeconds,
                                          double lengthSeconds) {
    auto* track = m_project.findTrack(trackId);
    if (!track || !trackAccepts(track->kind, ClipKind::Midi)) return {};

    const std::size_t undoStart = m_undo.depth();
    const double resolvedStart = std::max(0.0, startSeconds);
    double resolvedLength =
        lengthSeconds > 0.0
            ? lengthSeconds
            : beatsToSeconds(double(std::max(1, m_project.timeSigNumerator)),
                             m_project.tempo);

    if (cloudProjectBound()) {
        std::string patternTrackId;
        for (std::string parentId = track->parentId; !parentId.empty();) {
            const TrackModel* parent = m_project.findTrack(parentId);
            if (!parent) break;
            if (parent->kind == TrackKind::Pattern) {
                patternTrackId = parent->id;
                break;
            }
            parentId = parent->parentId;
        }
        auto batch = std::make_shared<collab::BatchCommand>();
        std::string patternClipId;
        if (!patternTrackId.empty()) {
            const TrackModel* pattern = m_project.findTrack(patternTrackId);
            if (pattern) {
                for (const ClipModel& candidate : pattern->clips) {
                    if (candidate.kind != ClipKind::Pattern) continue;
                    const double end = candidate.startSeconds +
                                       candidate.durationSeconds;
                    if (resolvedStart + 1e-9 < candidate.startSeconds ||
                        resolvedStart >= end - 1e-9) continue;
                    patternClipId = candidate.id;
                    if (lengthSeconds <= 0.0)
                        resolvedLength = std::max(kMinClipSeconds,
                                                  end - resolvedStart);
                    const double memberEnd = resolvedStart + resolvedLength;
                    if (memberEnd > end) {
                        appendCommand(batch, collab::SetClipProperty{
                            patternTrackId, patternClipId,
                            collab::ClipProperty::DurationSeconds,
                            memberEnd - candidate.startSeconds});
                    }
                    break;
                }
                if (patternClipId.empty()) {
                    patternClipId = newUuid();
                    appendCommand(batch, collab::AddClip{
                        patternTrackId, patternClipId, ClipKind::Pattern,
                        pattern->name, resolvedStart, resolvedLength,
                        pattern->color,
                        pattern->clips.empty() ? std::string()
                                               : pattern->clips.back().id});
                }
            }
        }
        ClipModel created;
        created.id = newUuid();
        created.name = track->name;
        created.kind = ClipKind::Midi;
        created.patternClipId = patternClipId;
        created.startSeconds = resolvedStart;
        created.durationSeconds = resolvedLength;
        created.color = track->color;
        appendCommand(batch, collab::AddClip{
            trackId, created.id, created.kind, created.name,
            created.startSeconds, created.durationSeconds, created.color,
            track->clips.empty() ? std::string() : track->clips.back().id});
        if (!patternClipId.empty()) {
            appendCommand(batch, collab::SetClipPatternOwner{
                trackId, created.id, patternClipId});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Add MIDI Clip");
        return result == collab::SharedMutationResult::Submitted ? created.id
                                                                 : std::string{};
    }

    // A MIDI source nested in a Pattern belongs to the Pattern clip covering
    // the drop point. The explicit link is what lets later arrangement edits
    // move/cut/copy the source without guessing from overlapping rectangles.
    std::string patternTrackId;
    for (std::string parentId = track->parentId; !parentId.empty();) {
        const TrackModel* parent = m_project.findTrack(parentId);
        if (!parent) break;
        if (parent->kind == TrackKind::Pattern) {
            patternTrackId = parent->id;
            break;
        }
        parentId = parent->parentId;
    }

    std::string patternClipId;
    if (!patternTrackId.empty()) {
        TrackModel* pattern = m_project.findTrack(patternTrackId);
        if (pattern) {
            for (ClipModel& candidate : pattern->clips) {
                if (candidate.kind != ClipKind::Pattern) continue;
                const double end = candidate.startSeconds +
                                   candidate.durationSeconds;
                if (resolvedStart + 1e-9 < candidate.startSeconds ||
                    resolvedStart >= end - 1e-9) {
                    continue;
                }
                patternClipId = candidate.id;
                if (lengthSeconds <= 0.0) {
                    // New sources inherit the actual Pattern instance window,
                    // including a container the user stretched to several
                    // bars, instead of silently falling back to one bar.
                    resolvedLength = std::max(
                        kMinClipSeconds, end - resolvedStart);
                }
                const double memberEnd = resolvedStart + resolvedLength;
                if (memberEnd > end) {
                    const double before = candidate.durationSeconds;
                    const double after = memberEnd - candidate.startSeconds;
                    const std::string ownerId = candidate.id;
                    auto setDuration = [this, patternTrackId,
                                        ownerId](double value) {
                        bool changed = false;
                        if (TrackModel* owner =
                                m_project.findTrack(patternTrackId)) {
                            for (ClipModel& c : owner->clips) {
                                if (c.id == ownerId) {
                                    changed = c.durationSeconds != value;
                                    c.durationSeconds = value;
                                    break;
                                }
                            }
                        }
                        if (!changed) return;
                        // The Pattern boundary gates every linked child, not
                        // merely the clip that caused the extension.
                        for (const TrackModel& memberTrack : m_project.tracks) {
                            if (!trackAccepts(memberTrack.kind, ClipKind::Midi))
                                continue;
                            const bool linked = std::any_of(
                                memberTrack.clips.begin(),
                                memberTrack.clips.end(),
                                [&](const ClipModel& member) {
                                    return member.kind == ClipKind::Midi &&
                                           member.patternClipId == ownerId;
                                });
                            if (linked) syncTrackNotes(memberTrack);
                        }
                        updateTimelineDuration();
                    };
                    setDuration(after);
                    m_undo.push("Extend Pattern Clip",
                                [setDuration, before] { setDuration(before); },
                                [setDuration, after] { setDuration(after); });
                }
                break;
            }
        }
        if (patternClipId.empty()) {
            patternClipId = addPatternClip(patternTrackId, resolvedStart,
                                           resolvedLength);
        }
    }

    ClipModel clip;
    clip.id = newUuid();
    clip.name = track->name;
    clip.kind = ClipKind::Midi;
    clip.patternClipId = patternClipId;
    clip.startSeconds = resolvedStart;
    clip.durationSeconds = resolvedLength;
    clip.color = track->color;
    track->clips.push_back(clip);

    syncTrackClips(*track);
    syncTrackNotes(*track);
    updateTimelineDuration();

    const std::string clipUuid = clip.id;
    m_undo.push("Add MIDI Clip",
                [this, trackId, clipUuid] { removeClip(trackId, clipUuid); },
                [this, trackId, clip] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->clips.push_back(clip);
                        syncTrackClips(*t);
                        syncTrackNotes(*t);
                        updateTimelineDuration();
                    }
                });
    collapseUndo(undoStart, "Add MIDI Clip");
    return clipUuid;
}

// ── MIDI notes ─────────────────────────────────────────────────────────────

namespace {
/// Find a MIDI clip by id. Returns null for an audio clip too, so every note
/// operation is a no-op on the wrong kind of clip rather than corrupting it.
ClipModel* findMidiClip(ProjectModel& project, const std::string& trackId,
                        const std::string& clipId) {
    auto* track = project.findTrack(trackId);
    if (!track) return nullptr;
    for (auto& clip : track->clips) {
        if (clip.id == clipId && clip.kind == ClipKind::Midi) return &clip;
    }
    return nullptr;
}

bool sameNotePlayback(const NoteModel& a, const NoteModel& b) {
    return a.pitch == b.pitch && a.startBeats == b.startBeats &&
           a.lengthBeats == b.lengthBeats && a.velocity == b.velocity &&
           a.muted == b.muted && a.pan == b.pan;
}

bool sameNoteGeometry(const NoteModel& a, const NoteModel& b) {
    return a.id == b.id && a.pitch == b.pitch &&
           a.startBeats == b.startBeats &&
           a.lengthBeats == b.lengthBeats;
}

bool sameNoteGeometry(const std::vector<NoteModel>& a,
                      const std::vector<NoteModel>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!sameNoteGeometry(a[i], b[i])) return false;
    }
    return true;
}

bool sameNotePlayback(const std::vector<NoteModel>& a,
                      const std::vector<NoteModel>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!sameNotePlayback(a[i], b[i])) return false;
    }
    return true;
}
} // namespace

bool EngineController::noteEditTargets(const std::string& trackId,
                                       const std::string& clipId) const {
    return m_noteEdit.active && m_noteEdit.trackId == trackId &&
           m_noteEdit.clipId == clipId;
}

void EngineController::captureNoteEditBeforeMutation(
    const std::string& trackId, const std::string& clipId,
    const ClipModel& clip) {
    if (!noteEditTargets(trackId, clipId) || m_noteEdit.structural) return;
    m_noteEdit.structuralBefore = clip.notes;
    // Property edits may precede the first structural operation in one stroke
    // (draw/move, then erase, for example). Reconstruct the true mouse-down
    // endpoint before allowing add/remove to invalidate the lazy index.
    for (const auto& [noteId, before] : m_noteEdit.beforeById) {
        const auto indexed =
            m_noteEdit.noteIndices.find(std::string_view(noteId));
        if (indexed != m_noteEdit.noteIndices.end() &&
            indexed->second < m_noteEdit.structuralBefore.size() &&
            m_noteEdit.structuralBefore[indexed->second].id == noteId) {
            m_noteEdit.structuralBefore[indexed->second] = before;
            continue;
        }
        const auto found = std::find_if(
            m_noteEdit.structuralBefore.begin(),
            m_noteEdit.structuralBefore.end(),
            [&](const NoteModel& note) { return note.id == noteId; });
        if (found != m_noteEdit.structuralBefore.end()) *found = before;
    }
    m_noteEdit.structural = true;
    m_noteEdit.noteIndices.clear();
    m_noteEdit.beforeById.clear();
    m_noteEdit.indexBuilt = false;
}

NoteModel* EngineController::indexedNoteForActiveEdit(
    const std::string& trackId, const std::string& clipId, ClipModel& clip,
    const std::string& noteId) {
    if (!noteEditTargets(trackId, clipId) || m_noteEdit.structural)
        return nullptr;
    if (!m_noteEdit.indexBuilt) {
        m_noteEdit.noteIndices.clear();
        m_noteEdit.noteIndices.reserve(clip.notes.size());
        for (std::size_t index = 0; index < clip.notes.size(); ++index) {
            const NoteModel& note = clip.notes[index];
            m_noteEdit.noteIndices.try_emplace(note.id, index);
        }
        m_noteEdit.indexBuilt = true;
        ++m_noteEditIndexBuildCount;
    }
    const auto found = m_noteEdit.noteIndices.find(std::string_view(noteId));
    if (found == m_noteEdit.noteIndices.end() ||
        found->second >= clip.notes.size()) {
        return nullptr;
    }
    NoteModel& note = clip.notes[found->second];
    return note.id == noteId ? &note : nullptr;
}

void EngineController::captureNoteDeltaBefore(const NoteModel& note) {
    if (!m_noteEdit.active || m_noteEdit.structural) return;
    m_noteEdit.beforeById.try_emplace(note.id, note);
}

void EngineController::publishOrDeferNotePlayback(
    const std::string& trackId, const std::string& clipId,
    const TrackModel& track, bool geometryChanged) {
    // The Piano Roll draws from the document, not the delayed realtime
    // snapshot. Invalidate its geometry cache on every live mutation even
    // while the expensive publication remains coalesced until mouse-up.
    if (geometryChanged) bumpMidiNotesRevision(trackId);
    if (noteEditTargets(trackId, clipId)) {
        m_noteEdit.playbackDirty = true;
        return;
    }
    syncTrackNotes(track, geometryChanged);
}

void EngineController::bumpMidiNotesRevision(const std::string& trackId) {
    ++m_midiNotesRevisionCounter;
    if (m_midiNotesRevisionCounter == 0) ++m_midiNotesRevisionCounter;
    m_midiNotesRevisions[trackId] = m_midiNotesRevisionCounter;
}

std::uint64_t EngineController::midiNotesRevision(
    const std::string& trackId) const {
    const auto found = m_midiNotesRevisions.find(trackId);
    return found == m_midiNotesRevisions.end() ? 0 : found->second;
}

std::string EngineController::addNote(const std::string& trackId,
                                      const std::string& clipId, int pitch,
                                      double startBeats, double lengthBeats,
                                      int velocity) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return {};

    NoteModel note;
    note.id = newUuid();
    note.pitch = std::clamp(pitch, kMinPitch, kMaxPitch);
    note.startBeats = std::max(0.0, startBeats);
    note.lengthBeats = std::max(kMinNoteBeats, lengthBeats);
    note.velocity = std::clamp(velocity, 1, 127);
    if (cloudProjectBound() && !noteEditTargets(trackId, clipId)) {
        const auto result = submitSharedMutation(
            collab::UpsertMidiNote{
                trackId, clipId, note,
                clip->notes.empty() ? std::string() : clip->notes.back().id},
            "Add Note");
        return result == collab::SharedMutationResult::Submitted ? note.id
                                                                 : std::string{};
    }
    captureNoteEditBeforeMutation(trackId, clipId, *clip);
    clip->notes.push_back(note);
    if (const TrackModel* track = m_project.findTrack(trackId))
        publishOrDeferNotePlayback(trackId, clipId, *track, true);

    if (noteEditTargets(trackId, clipId)) return note.id;

    // Redo replays the note captured here rather than calling addNote again —
    // a second call would mint a different uuid, and this entry's undo (which
    // captured the original id) would then be pointing at nothing.
    m_undo.push("Add Note",
                [this, trackId, clipId, id = note.id] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        std::erase_if(c->notes, [&](const NoteModel& n) {
                            return n.id == id;
                        });
                        if (const TrackModel* track =
                                m_project.findTrack(trackId)) {
                            syncTrackNotes(*track);
                        }
                    }
                },
                [this, trackId, clipId, note] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->notes.push_back(note);
                        if (const TrackModel* track =
                                m_project.findTrack(trackId)) {
                            syncTrackNotes(*track);
                        }
                    }
                });
    return note.id;
}

void EngineController::removeNote(const std::string& trackId,
                                  const std::string& clipId,
                                  const std::string& noteId) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;

    NoteModel snapshot;
    bool found = false;
    for (const auto& note : clip->notes) {
        if (note.id == noteId) {
            snapshot = note;
            found = true;
            break;
        }
    }
    if (!found) return;

    if (cloudProjectBound() && !noteEditTargets(trackId, clipId)) {
        (void)submitSharedMutation(
            collab::DeleteMidiNote{trackId, clipId, noteId}, "Remove Note");
        return;
    }

    captureNoteEditBeforeMutation(trackId, clipId, *clip);
    std::erase_if(clip->notes,
                  [&](const NoteModel& n) { return n.id == noteId; });
    if (const TrackModel* track = m_project.findTrack(trackId))
        publishOrDeferNotePlayback(trackId, clipId, *track, true);

    if (noteEditTargets(trackId, clipId)) return;

    m_undo.push("Remove Note",
                [this, trackId, clipId, snapshot] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->notes.push_back(snapshot);
                        if (const TrackModel* track =
                                m_project.findTrack(trackId)) {
                            syncTrackNotes(*track);
                        }
                    }
                },
                [this, trackId, clipId, noteId] {
                    removeNote(trackId, clipId, noteId);
                });
}

void EngineController::setNote(const std::string& trackId,
                               const std::string& clipId,
                               const std::string& noteId, int pitch,
                               double startBeats, double lengthBeats) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;
    if (noteEditTargets(trackId, clipId) && !m_noteEdit.structural) {
        NoteModel* note =
            indexedNoteForActiveEdit(trackId, clipId, *clip, noteId);
        if (!note) return;
        const int nextPitch = std::clamp(pitch, kMinPitch, kMaxPitch);
        const double nextStart = std::max(0.0, startBeats);
        const double nextLength = std::max(kMinNoteBeats, lengthBeats);
        if (note->pitch == nextPitch && note->startBeats == nextStart &&
            note->lengthBeats == nextLength) {
            return;
        }
        captureNoteDeltaBefore(*note);
        note->pitch = nextPitch;
        note->startBeats = nextStart;
        note->lengthBeats = nextLength;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, true);
        return;
    }
    for (auto& note : clip->notes) {
        if (note.id != noteId) continue;
        const int nextPitch = std::clamp(pitch, kMinPitch, kMaxPitch);
        const double nextStart = std::max(0.0, startBeats);
        // Deliberately not clamped against the clip's length: trimming a clip
        // shorter should hide the notes past its end, not delete them.
        const double nextLength = std::max(kMinNoteBeats, lengthBeats);
        if (note.pitch == nextPitch && note.startBeats == nextStart &&
            note.lengthBeats == nextLength) {
            return;
        }
        if (cloudProjectBound()) {
            NoteModel next = note;
            next.pitch = nextPitch;
            next.startBeats = nextStart;
            next.lengthBeats = nextLength;
            const std::size_t index = std::size_t(&note - clip->notes.data());
            (void)submitSharedMutation(
                collab::UpsertMidiNote{
                    trackId, clipId, next,
                    index == 0 ? std::string() : clip->notes[index - 1].id},
                "Edit Note");
            return;
        }
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        note.pitch = nextPitch;
        note.startBeats = nextStart;
        note.lengthBeats = nextLength;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, true);
        return;
    }
}

void EngineController::setNoteStates(const std::string& trackId,
                                     const std::string& clipId,
                                     std::span<const NoteModel> updates) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    const TrackModel* track = m_project.findTrack(trackId);
    if (!clip || !track || updates.empty()) return;

    if (noteEditTargets(trackId, clipId) && !m_noteEdit.structural) {
        bool changed = false;
        bool playbackChanged = false;
        bool geometryChanged = false;
        for (const NoteModel& update : updates) {
            if (update.id.empty()) continue;
            NoteModel* note = indexedNoteForActiveEdit(
                trackId, clipId, *clip, update.id);
            if (!note) continue;
            NoteModel next = update;
            next.id = note->id;
            next.pitch = std::clamp(next.pitch, kMinPitch, kMaxPitch);
            next.startBeats = std::max(0.0, next.startBeats);
            next.lengthBeats = std::max(kMinNoteBeats, next.lengthBeats);
            next.velocity = std::clamp(next.velocity, 1, 127);
            next.pan = std::clamp(next.pan, -1.0f, 1.0f);
            if (next == *note) continue;
            captureNoteDeltaBefore(*note);
            playbackChanged |= !sameNotePlayback(*note, next);
            geometryChanged |= !sameNoteGeometry(*note, next);
            // Keep the id string itself in place: the lazy index stores a
            // string_view into it and remains valid for the whole gesture.
            note->pitch = next.pitch;
            note->startBeats = next.startBeats;
            note->lengthBeats = next.lengthBeats;
            note->velocity = next.velocity;
            note->muted = next.muted;
            note->color = next.color;
            note->pan = next.pan;
            changed = true;
        }
        if (changed && playbackChanged)
            publishOrDeferNotePlayback(trackId, clipId, *track,
                                       geometryChanged);
        return;
    }

    std::unordered_map<std::string, const NoteModel*> byId;
    byId.reserve(updates.size());
    for (const NoteModel& update : updates) {
        if (!update.id.empty()) byId.insert_or_assign(update.id, &update);
    }

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (std::size_t index = 0; index < clip->notes.size(); ++index) {
            const NoteModel& note = clip->notes[index];
            const auto found = byId.find(note.id);
            if (found == byId.end()) continue;
            NoteModel next = *found->second;
            next.id = note.id;
            next.pitch = std::clamp(next.pitch, kMinPitch, kMaxPitch);
            next.startBeats = std::max(0.0, next.startBeats);
            next.lengthBeats = std::max(kMinNoteBeats, next.lengthBeats);
            next.velocity = std::clamp(next.velocity, 1, 127);
            next.pan = std::clamp(next.pan, -1.0f, 1.0f);
            if (next == note) continue;
            appendCommand(batch, collab::UpsertMidiNote{
                trackId, clipId, next,
                index == 0 ? std::string() : clip->notes[index - 1].id});
        }
        if (!batch->commands.empty()) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       "Edit Notes");
        }
        return;
    }

    bool changed = false;
    bool playbackChanged = false;
    bool geometryChanged = false;
    for (NoteModel& note : clip->notes) {
        const auto found = byId.find(note.id);
        if (found == byId.end()) continue;
        NoteModel next = *found->second;
        next.id = note.id;
        next.pitch = std::clamp(next.pitch, kMinPitch, kMaxPitch);
        next.startBeats = std::max(0.0, next.startBeats);
        next.lengthBeats = std::max(kMinNoteBeats, next.lengthBeats);
        next.velocity = std::clamp(next.velocity, 1, 127);
        next.pan = std::clamp(next.pan, -1.0f, 1.0f);
        if (next == note) continue;
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        playbackChanged |= !sameNotePlayback(note, next);
        geometryChanged |= !sameNoteGeometry(note, next);
        note = std::move(next);
        changed = true;
    }
    if (changed && playbackChanged)
        publishOrDeferNotePlayback(trackId, clipId, *track,
                                   geometryChanged);
}

void EngineController::removeNotes(const std::string& trackId,
                                   const std::string& clipId,
                                   std::span<const std::string> noteIds) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    const TrackModel* track = m_project.findTrack(trackId);
    if (!clip || !track || noteIds.empty()) return;

    std::unordered_set<std::string> ids(noteIds.begin(), noteIds.end());
    const bool removesAny = std::any_of(
        clip->notes.begin(), clip->notes.end(),
        [&](const NoteModel& note) { return ids.contains(note.id); });
    if (!removesAny) return;
    if (cloudProjectBound() && !noteEditTargets(trackId, clipId)) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const NoteModel& note : clip->notes) {
            if (ids.contains(note.id))
                appendCommand(batch, collab::DeleteMidiNote{
                    trackId, clipId, note.id});
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Remove Notes");
        return;
    }
    captureNoteEditBeforeMutation(trackId, clipId, *clip);
    std::erase_if(clip->notes, [&](const NoteModel& note) {
        return ids.contains(note.id);
    });
    publishOrDeferNotePlayback(trackId, clipId, *track, true);
}

void EngineController::setNoteVelocity(const std::string& trackId,
                                       const std::string& clipId,
                                       const std::string& noteId,
                                       int velocity) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;
    if (noteEditTargets(trackId, clipId) && !m_noteEdit.structural) {
        NoteModel* note =
            indexedNoteForActiveEdit(trackId, clipId, *clip, noteId);
        if (!note) return;
        const int next = std::clamp(velocity, 1, 127);
        if (note->velocity == next) return;
        captureNoteDeltaBefore(*note);
        note->velocity = next;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
    for (auto& note : clip->notes) {
        if (note.id != noteId) continue;
        const int next = std::clamp(velocity, 1, 127);
        if (note.velocity == next) return;
        if (cloudProjectBound()) {
            NoteModel changed = note;
            changed.velocity = next;
            const std::size_t index = std::size_t(&note - clip->notes.data());
            (void)submitSharedMutation(
                collab::UpsertMidiNote{
                    trackId, clipId, changed,
                    index == 0 ? std::string() : clip->notes[index - 1].id},
                "Set Note Velocity");
            return;
        }
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        note.velocity = next;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
}

void EngineController::setNoteMuted(const std::string& trackId,
                                    const std::string& clipId,
                                    const std::string& noteId, bool muted) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;
    if (noteEditTargets(trackId, clipId) && !m_noteEdit.structural) {
        NoteModel* note =
            indexedNoteForActiveEdit(trackId, clipId, *clip, noteId);
        if (!note || note->muted == muted) return;
        captureNoteDeltaBefore(*note);
        note->muted = muted;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
    for (auto& note : clip->notes) {
        if (note.id != noteId) continue;
        if (note.muted == muted) return;
        if (cloudProjectBound()) {
            NoteModel changed = note;
            changed.muted = muted;
            const std::size_t index = std::size_t(&note - clip->notes.data());
            (void)submitSharedMutation(
                collab::UpsertMidiNote{
                    trackId, clipId, changed,
                    index == 0 ? std::string() : clip->notes[index - 1].id},
                muted ? "Mute Note" : "Unmute Note");
            return;
        }
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        note.muted = muted;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
}

void EngineController::setNotePan(const std::string& trackId,
                                  const std::string& clipId,
                                  const std::string& noteId, float pan) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;
    if (noteEditTargets(trackId, clipId) && !m_noteEdit.structural) {
        NoteModel* note =
            indexedNoteForActiveEdit(trackId, clipId, *clip, noteId);
        if (!note) return;
        const float next = std::clamp(pan, -1.0f, 1.0f);
        if (note->pan == next) return;
        captureNoteDeltaBefore(*note);
        note->pan = next;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
    for (auto& note : clip->notes) {
        if (note.id != noteId) continue;
        const float next = std::clamp(pan, -1.0f, 1.0f);
        if (note.pan == next) return;
        if (cloudProjectBound()) {
            NoteModel changed = note;
            changed.pan = next;
            const std::size_t index = std::size_t(&note - clip->notes.data());
            (void)submitSharedMutation(
                collab::UpsertMidiNote{
                    trackId, clipId, changed,
                    index == 0 ? std::string() : clip->notes[index - 1].id},
                "Set Note Pan");
            return;
        }
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        note.pan = next;
        if (const TrackModel* track = m_project.findTrack(trackId))
            publishOrDeferNotePlayback(trackId, clipId, *track, false);
        return;
    }
}

void EngineController::beginNoteEdit(const std::string& trackId,
                                     const std::string& clipId) {
    if (m_noteEdit.active) endNoteEdit("Edit Notes");
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;
    m_noteEdit = {};
    m_noteEdit.active = true;
    m_noteEdit.trackId = trackId;
    m_noteEdit.clipId = clipId;
}

void EngineController::endNoteEdit(const std::string& label) {
    if (!m_noteEdit.active) return;
    NoteEdit edit = std::move(m_noteEdit);
    m_noteEdit = {};
    auto* clip = findMidiClip(m_project, edit.trackId, edit.clipId);
    if (!clip) return;

    if (edit.structural) {
        if (clip->notes == edit.structuralBefore) return;
        const std::vector<NoteModel> after = clip->notes;
        const bool playbackChanged =
            !sameNotePlayback(edit.structuralBefore, after);
        const bool geometryChanged =
            !sameNoteGeometry(edit.structuralBefore, after);
        if (edit.playbackDirty && playbackChanged) {
            if (const TrackModel* track = m_project.findTrack(edit.trackId))
                syncTrackNotes(*track, geometryChanged);
        }
        if (cloudProjectBound()) {
            std::unordered_map<std::string, const NoteModel*> beforeById;
            beforeById.reserve(edit.structuralBefore.size());
            for (const NoteModel& note : edit.structuralBefore)
                beforeById.emplace(note.id, &note);
            std::unordered_set<std::string> afterIds;
            afterIds.reserve(after.size());
            for (const NoteModel& note : after) afterIds.insert(note.id);
            auto batch = std::make_shared<collab::BatchCommand>();
            for (const NoteModel& note : edit.structuralBefore) {
                if (!afterIds.contains(note.id))
                    appendCommand(batch, collab::DeleteMidiNote{
                        edit.trackId, edit.clipId, note.id});
            }
            for (std::size_t index = 0; index < after.size(); ++index) {
                const NoteModel& note = after[index];
                const auto before = beforeById.find(note.id);
                if (before != beforeById.end() && *before->second == note)
                    continue;
                appendCommand(batch, collab::UpsertMidiNote{
                    edit.trackId, edit.clipId, note,
                    index == 0 ? std::string() : after[index - 1].id});
            }
            const auto result = submitSharedMutation(
                collab::CommandBody{std::move(batch)}, label);
            if (result == collab::SharedMutationResult::Blocked) {
                clip->notes = edit.structuralBefore;
                if (const TrackModel* track = m_project.findTrack(edit.trackId))
                    syncTrackNotes(*track, geometryChanged);
            }
            if (result != collab::SharedMutationResult::LocalFallback) return;
        }
        m_undo.push(
            label,
            [this, trackId = edit.trackId, clipId = edit.clipId,
             before = std::move(edit.structuralBefore), playbackChanged,
             geometryChanged] {
                if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                    c->notes = before;
                    if (playbackChanged) {
                        if (const TrackModel* track =
                                m_project.findTrack(trackId)) {
                            syncTrackNotes(*track, geometryChanged);
                        }
                    } else if (geometryChanged) {
                        bumpMidiNotesRevision(trackId);
                    }
                }
            },
            [this, trackId = edit.trackId, clipId = edit.clipId, after,
             playbackChanged, geometryChanged] {
                if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                    c->notes = after;
                    if (playbackChanged) {
                        if (const TrackModel* track =
                                m_project.findTrack(trackId)) {
                            syncTrackNotes(*track, geometryChanged);
                        }
                    } else if (geometryChanged) {
                        bumpMidiNotesRevision(trackId);
                    }
                }
            });
        return;
    }

    struct NotePropertyDelta {
        NoteModel before;
        NoteModel after;
    };
    std::vector<NotePropertyDelta> built;
    built.reserve(edit.beforeById.size());
    bool playbackChanged = false;
    bool geometryChanged = false;
    for (auto& [noteId, before] : edit.beforeById) {
        const auto indexed =
            edit.noteIndices.find(std::string_view(noteId));
        if (indexed == edit.noteIndices.end() ||
            indexed->second >= clip->notes.size()) {
            continue;
        }
        const NoteModel& after = clip->notes[indexed->second];
        if (after.id != noteId || after == before) continue;
        playbackChanged |= !sameNotePlayback(before, after);
        geometryChanged |= !sameNoteGeometry(before, after);
        built.push_back(
            NotePropertyDelta{std::move(before), after});
    }
    if (built.empty()) return;
    if (edit.playbackDirty && playbackChanged) {
        if (const TrackModel* track = m_project.findTrack(edit.trackId))
            syncTrackNotes(*track, geometryChanged);
    }

    const auto delta = std::make_shared<
        const std::vector<NotePropertyDelta>>(std::move(built));
    auto apply = [this, delta, trackId = edit.trackId,
                  clipId = edit.clipId,
                  playbackChanged, geometryChanged](bool useAfter) {
        ClipModel* target = findMidiClip(m_project, trackId, clipId);
        if (!target) return;
        std::unordered_map<std::string_view, const NoteModel*> values;
        values.reserve(delta->size());
        for (const NotePropertyDelta& change : *delta) {
            const NoteModel& value = useAfter ? change.after : change.before;
            values.emplace(value.id, &value);
        }
        for (NoteModel& note : target->notes) {
            const auto found = values.find(std::string_view(note.id));
            if (found == values.end()) continue;
            const NoteModel& value = *found->second;
            note.pitch = value.pitch;
            note.startBeats = value.startBeats;
            note.lengthBeats = value.lengthBeats;
            note.velocity = value.velocity;
            note.muted = value.muted;
            note.color = value.color;
            note.pan = value.pan;
        }
        if (playbackChanged) {
            if (const TrackModel* track = m_project.findTrack(trackId))
                syncTrackNotes(*track, geometryChanged);
        }
    };
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const NotePropertyDelta& change : *delta) {
            const auto found = std::find_if(
                clip->notes.begin(), clip->notes.end(),
                [&](const NoteModel& note) {
                    return note.id == change.after.id;
                });
            if (found == clip->notes.end()) continue;
            const std::size_t index = std::size_t(found - clip->notes.begin());
            appendCommand(batch, collab::UpsertMidiNote{
                edit.trackId, edit.clipId, change.after,
                index == 0 ? std::string() : clip->notes[index - 1].id});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, label);
        if (result == collab::SharedMutationResult::Blocked) apply(false);
        if (result != collab::SharedMutationResult::LocalFallback) return;
    }
    m_undo.push(label, [apply] { apply(false); },
                [apply] { apply(true); });
}

// ── Auditioning a file ──────────────────────────────────────────────────────

bool EngineController::previewFile(const std::string& filePath, bool loop,
                                   double pitchSemitones) {
    if (filePath.empty()) return false;
    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(filePath, decoded) || decoded.frames == 0) {
        return false;
    }
    // Deliberately not `loadSamples`: that memoises for the lifetime of the
    // project, and auditioning a folder would pin every file in it.
    auto buffer = engine::SampleBuffer::fromInterleaved(
        decoded.interleaved, engine::ChannelCount(decoded.channels),
        engine::FrameCount(decoded.frames), decoded.sampleRate);
    return previewBuffer(std::move(buffer), filePath, loop, pitchSemitones);
}

bool EngineController::previewBuffer(
    std::shared_ptr<const engine::SampleBuffer> audio,
    const std::string& sourcePath, bool loop, double pitchSemitones) {
    if (!m_preview || !audio || audio->frames() == 0) return false;
    m_previewPath = sourcePath;
    m_previewDuration = audio->sampleRate() > 0.0
                            ? double(audio->frames()) / audio->sampleRate()
                            : 0.0;
    m_preview->setLoop(loop);
    m_preview->setRate(
        std::pow(2.0, std::clamp(pitchSemitones, -36.0, 36.0) / 12.0));
    m_preview->start(std::move(audio));
    return true;
}

void EngineController::stopPreview() {
    if (m_preview) m_preview->stop();
}

void EngineController::setPreviewLoop(bool loop) {
    if (m_preview) m_preview->setLoop(loop);
}

void EngineController::setPreviewGain(float gain) {
    if (m_preview) m_preview->setGain(gain);
}

void EngineController::seekPreviewSeconds(double seconds) {
    if (!m_preview) return;
    const double rate = m_preview->sourceRate();
    if (!(rate > 0.0)) return;
    m_preview->seekFrames(std::int64_t(std::max(0.0, seconds) * rate));
}

bool EngineController::previewPlaying() const {
    return m_preview && m_preview->playing();
}

double EngineController::previewPositionSeconds() const {
    if (!m_preview) return 0.0;
    const double rate = m_preview->sourceRate();
    if (!(rate > 0.0)) return 0.0;
    return double(m_preview->positionFrames()) / rate;
}

// ── MIDI files ──────────────────────────────────────────────────────────────

std::vector<std::string> EngineController::importMidiFile(
    const std::string& filePath, const std::string& trackId, double startSeconds,
    midifile::File* outInfo) {
    const TrackModel* target = m_project.findTrack(trackId);
    if (!target || !trackAccepts(target->kind, ClipKind::Midi)) return {};

    midifile::File file;
    std::string error;
    if (!midifile::parse(filePath, file, error)) return {};
    if (outInfo) *outInfo = file;
    if (file.notes.empty()) return {};

    // Group in one pass. The previous implementation discovered the sources
    // first and then scanned every note again for each source, which made a
    // many-lane import quadratic in the number of tracks.
    std::unordered_map<int, std::vector<const midifile::Note*>> notesByTrack;
    std::vector<int> sources;
    notesByTrack.reserve(file.trackNames.size());
    sources.reserve(file.trackNames.size());
    for (const midifile::Note& note : file.notes) {
        auto [found, inserted] = notesByTrack.try_emplace(note.track);
        if (inserted) {
            sources.push_back(note.track);
        }
        found->second.push_back(&note);
    }
    // Keep the existing lane ordering: source-track order, while skipping a
    // format-1 conductor track that contains no notes.
    std::sort(sources.begin(), sources.end());

    const std::string stem =
        platform::pathToUtf8(platform::pathFromUtf8(filePath).stem());
    const double beatsPerBar =
        double(m_project.timeSigNumerator) * 4.0 /
        double(m_project.timeSigDenominator > 0 ? m_project.timeSigDenominator : 4);
    const double resolvedStart = std::max(0.0, startSeconds);
    const double longestBars = std::ceil(file.lengthBeats / beatsPerBar);
    const double longestLengthSeconds = beatsToSeconds(
        std::max(1.0, longestBars * beatsPerBar), m_project.tempo);

    // Match addMidiClip's ownership rule: the nearest Pattern ancestor owns
    // the imported clips, including when the target lives one or more plain
    // folders below that Pattern.
    std::string patternTrackId;
    for (std::string parentId = target->parentId; !parentId.empty();) {
        const TrackModel* parent = m_project.findTrack(parentId);
        if (!parent) break;
        if (parent->kind == TrackKind::Pattern) {
            patternTrackId = parent->id;
            break;
        }
        parentId = parent->parentId;
    }
    const std::string importedParentId =
        patternTrackId.empty() ? std::string() : target->parentId;
    const std::string importedOutputBusId =
        patternTrackId.empty() ? std::string()
                               : summingParent(m_project, target->id);
    const std::size_t originalTrackCount = m_project.tracks.size();
    const std::size_t targetTrackIndex = m_project.indexOf(trackId);
    const std::size_t insertedTrackIndex =
        patternTrackId.empty()
            ? originalTrackCount
            : std::min(targetTrackIndex + 1, originalTrackCount);

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        std::string patternClipId;
        if (const TrackModel* pattern = patternTrackId.empty()
                                            ? nullptr
                                            : m_project.findTrack(patternTrackId)) {
            const double memberEnd = resolvedStart + longestLengthSeconds;
            for (const ClipModel& candidate : pattern->clips) {
                if (candidate.kind != ClipKind::Pattern) continue;
                const double end =
                    candidate.startSeconds + candidate.durationSeconds;
                if (resolvedStart + 1e-9 < candidate.startSeconds ||
                    resolvedStart >= end - 1e-9) {
                    continue;
                }
                patternClipId = candidate.id;
                if (memberEnd > end) {
                    appendCommand(batch, collab::SetClipProperty{
                        patternTrackId, patternClipId,
                        collab::ClipProperty::DurationSeconds,
                        memberEnd - candidate.startSeconds});
                }
                break;
            }
            if (patternClipId.empty()) {
                ClipModel owner;
                owner.id = newUuid();
                owner.name = pattern->name;
                owner.kind = ClipKind::Pattern;
                owner.startSeconds = resolvedStart;
                owner.durationSeconds = longestLengthSeconds;
                owner.color = pattern->color;
                patternClipId = owner.id;
                if (!appendSharedClip(
                        batch, patternTrackId, owner,
                        pattern->clips.empty() ? std::string()
                                               : pattern->clips.back().id)) {
                    return {};
                }
            }
        }

        std::vector<std::string> clipIds;
        clipIds.reserve(sources.size());
        std::string trackAnchor =
            patternTrackId.empty()
                ? (m_project.tracks.empty() ? std::string()
                                            : m_project.tracks.back().id)
                : trackId;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            const int source = sources[index];
            const auto grouped = notesByTrack.find(source);
            if (grouped == notesByTrack.end() || grouped->second.empty())
                continue;

            std::vector<NoteModel> notes;
            notes.reserve(grouped->second.size());
            double lastEnd = 0.0;
            for (const midifile::Note* sourceNote : grouped->second) {
                NoteModel note;
                note.id = newUuid();
                note.pitch = std::clamp(sourceNote->pitch, 0, 127);
                note.startBeats = sourceNote->startBeats;
                note.lengthBeats = sourceNote->lengthBeats;
                note.velocity = std::clamp(sourceNote->velocity, 1, 127);
                notes.push_back(note);
                lastEnd = std::max(
                    lastEnd,
                    sourceNote->startBeats + sourceNote->lengthBeats);
            }
            const double bars = std::ceil(lastEnd / beatsPerBar);
            const double lengthBeats =
                std::max(1.0, bars * beatsPerBar);
            ClipModel clip;
            clip.id = newUuid();
            clip.kind = ClipKind::Midi;
            clip.name = index == 0 && sources.size() == 1
                            ? stem
                            : stem + " В· " + std::to_string(index + 1);
            clip.startSeconds = resolvedStart;
            clip.durationSeconds =
                beatsToSeconds(lengthBeats, m_project.tempo);
            clip.patternClipId = patternClipId;
            clip.notes = std::move(notes);
            if (index == 0) {
                clip.color = target->color;
                if (!appendSharedClip(
                        batch, trackId, clip,
                        target->clips.empty() ? std::string()
                                              : target->clips.back().id)) {
                    return {};
                }
            } else {
                TrackModel lane;
                lane.id = newUuid();
                lane.kind = TrackKind::Instrument;
                lane.name = source < int(file.trackNames.size())
                                ? file.trackNames[std::size_t(source)]
                                : std::string();
                if (lane.name.empty())
                    lane.name = stem + " " + std::to_string(index + 1);
                if (lane.name.empty()) lane.name = defaultTrackName(lane.kind);
                lane.color = defaultTrackColor(lane.kind);
                lane.parentId = importedParentId;
                lane.outputBusId = importedOutputBusId;
                clip.color = lane.color;
                lane.clips.push_back(std::move(clip));
                if (!appendSharedTrack(batch, lane, trackAnchor)) return {};
                trackAnchor = lane.id;
                clip.id = lane.clips.front().id;
            }
            clipIds.push_back(clip.id);
        }
        if (clipIds.empty() || !sharedBatchApplies(m_project, batch)) return {};
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Import MIDI File");
        return result == collab::SharedMutationResult::Submitted
                   ? clipIds
                   : std::vector<std::string>{};
    }

    // Keep only the imported delta alive for undo. Copying the whole project
    // before and after a file import made the GUI pause in an otherwise busy
    // project even when the file itself contained only a handful of notes.
    struct ImportUndoDelta {
        std::string targetTrackId;
        std::size_t targetClipIndex = 0;
        ClipModel targetClip;
        std::size_t firstAppendedTrackIndex = 0;
        std::vector<TrackModel> appendedTracks;
        std::unordered_set<std::string> appendedTrackIds;
        std::string patternTrackId;
        std::string patternClipId;
        std::size_t patternClipIndex = 0;
        ClipModel createdPatternClip;
        bool createdPatternOwner = false;
        bool extendedPatternOwner = false;
        double patternDurationBefore = 0.0;
        double patternDurationAfter = 0.0;
    };
    auto delta = std::make_shared<ImportUndoDelta>();
    delta->targetTrackId = trackId;
    delta->targetClipIndex = target->clips.size();
    delta->firstAppendedTrackIndex = insertedTrackIndex;
    delta->patternTrackId = patternTrackId;

    if (TrackModel* pattern = patternTrackId.empty()
                                  ? nullptr
                                  : m_project.findTrack(patternTrackId)) {
        const double memberEnd = resolvedStart + longestLengthSeconds;
        for (ClipModel& candidate : pattern->clips) {
            if (candidate.kind != ClipKind::Pattern) continue;
            const double end = candidate.startSeconds + candidate.durationSeconds;
            if (resolvedStart + 1e-9 < candidate.startSeconds ||
                resolvedStart >= end - 1e-9) {
                continue;
            }
            delta->patternClipId = candidate.id;
            if (memberEnd > end) {
                delta->extendedPatternOwner = true;
                delta->patternDurationBefore = candidate.durationSeconds;
                candidate.durationSeconds = memberEnd - candidate.startSeconds;
                delta->patternDurationAfter = candidate.durationSeconds;
            }
            break;
        }
        if (delta->patternClipId.empty()) {
            ClipModel owner;
            owner.id = newUuid();
            owner.name = pattern->name;
            owner.kind = ClipKind::Pattern;
            owner.startSeconds = resolvedStart;
            owner.durationSeconds = longestLengthSeconds;
            owner.color = pattern->color;
            delta->patternClipIndex = pattern->clips.size();
            delta->patternClipId = owner.id;
            delta->createdPatternClip = owner;
            delta->createdPatternOwner = true;
            pattern->clips.push_back(std::move(owner));
        }
    }
    const auto undoPatternOwner = [this, delta] {
        if (delta->patternTrackId.empty()) return;
        TrackModel* pattern = m_project.findTrack(delta->patternTrackId);
        if (!pattern) return;
        if (delta->createdPatternOwner) {
            std::erase_if(pattern->clips, [&](const ClipModel& clip) {
                return clip.id == delta->patternClipId;
            });
        } else if (delta->extendedPatternOwner) {
            for (ClipModel& clip : pattern->clips) {
                if (clip.id == delta->patternClipId) {
                    clip.durationSeconds = delta->patternDurationBefore;
                    break;
                }
            }
        }
    };
    const auto redoPatternOwner = [this, delta] {
        if (delta->patternTrackId.empty()) return;
        TrackModel* pattern = m_project.findTrack(delta->patternTrackId);
        if (!pattern) return;
        if (delta->createdPatternOwner) {
            std::erase_if(pattern->clips, [&](const ClipModel& clip) {
                return clip.id == delta->patternClipId;
            });
            const std::size_t ownerAt =
                std::min(delta->patternClipIndex, pattern->clips.size());
            pattern->clips.insert(
                pattern->clips.begin() + std::ptrdiff_t(ownerAt),
                delta->createdPatternClip);
        } else if (delta->extendedPatternOwner) {
            for (ClipModel& clip : pattern->clips) {
                if (clip.id == delta->patternClipId) {
                    clip.durationSeconds = delta->patternDurationAfter;
                    break;
                }
            }
        }
    };

    // Everything below is one gesture: the extra lanes and every clip land
    // under a single undo entry, because one drag made all of it.
    std::vector<std::string> clipIds;
    clipIds.reserve(sources.size());
    // Directly append the extra models below. Reserving first keeps the target
    // and newly appended lane addresses stable while clips are populated.
    const std::size_t extraLaneCount = sources.empty() ? 0 : sources.size() - 1;
    m_project.tracks.reserve(m_project.tracks.size() + extraLaneCount);
    delta->appendedTracks.reserve(extraLaneCount);
    delta->appendedTrackIds.reserve(extraLaneCount);
    for (size_t index = 0; index < sources.size(); ++index) {
        const int source = sources[index];
        const auto grouped = notesByTrack.find(source);
        if (grouped == notesByTrack.end() || grouped->second.empty()) continue;

        TrackModel* lane = nullptr;
        if (index > 0) {
            // Instrument, not plain MIDI: a new lane should be able to make
            // a sound without the user rebuilding it.
            std::string name =
                source < int(file.trackNames.size()) ? file.trackNames[size_t(source)]
                                                     : std::string();
            if (name.empty()) name = stem + " " + std::to_string(index + 1);
            TrackModel model;
            model.id = newUuid();
            model.kind = TrackKind::Instrument;
            model.name = name.empty() ? defaultTrackName(model.kind) : name;
            model.color = defaultTrackColor(model.kind);
            model.parentId = importedParentId;
            model.outputBusId = importedOutputBusId;
            m_project.tracks.push_back(std::move(model));
            lane = &m_project.tracks.back();
        } else {
            lane = m_project.findTrack(trackId);
        }
        if (!lane) continue;

        double lastEnd = 0.0;
        std::vector<NoteModel> notes;
        notes.reserve(grouped->second.size());
        for (const midifile::Note* sourceNote : grouped->second) {
            const midifile::Note& note = *sourceNote;
            NoteModel out;
            out.id = newUuid();
            out.pitch = std::clamp(note.pitch, 0, 127);
            out.startBeats = note.startBeats;
            out.lengthBeats = note.lengthBeats;
            out.velocity = std::clamp(note.velocity, 1, 127);
            notes.push_back(out);
            lastEnd = std::max(lastEnd, note.startBeats + note.lengthBeats);
        }

        // Round the clip up to a whole bar past its last note: a clip that
        // ends exactly on the final note-off would silence that note, since
        // syncTrackNotes cuts anything reaching the clip's end.
        const double bars = std::ceil(lastEnd / beatsPerBar);
        const double lengthBeats = std::max(1.0, bars * beatsPerBar);

        ClipModel clip;
        clip.id = newUuid();
        clip.kind = ClipKind::Midi;
        clip.patternClipId = delta->patternClipId;
        clip.name = index == 0 && sources.size() == 1
                        ? stem
                        : stem + " · " + std::to_string(index + 1);
        clip.startSeconds = resolvedStart;
        clip.durationSeconds = beatsToSeconds(lengthBeats, m_project.tempo);
        clip.color = lane->color;
        clip.notes = std::move(notes);
        clipIds.push_back(clip.id);
        lane->clips.push_back(std::move(clip));

        if (index == 0) {
            delta->targetClip = lane->clips.back();
        } else {
            delta->appendedTrackIds.insert(lane->id);
            delta->appendedTracks.push_back(*lane);
        }
    }
    if (!patternTrackId.empty() && !delta->appendedTracks.empty()) {
        // Tracks are accumulated at the end to avoid one vector shift per MIDI
        // source. One rotation then places the whole ordered block beside the
        // target inside its Pattern subtree.
        std::rotate(m_project.tracks.begin() + std::ptrdiff_t(insertedTrackIndex),
                    m_project.tracks.begin() + std::ptrdiff_t(originalTrackCount),
                    m_project.tracks.end());
    }
    if (clipIds.empty()) {
        // No engine state was published yet. This path is defensive — a
        // parsed file with notes normally always produces its first clip.
        std::erase_if(m_project.tracks, [&](const TrackModel& track) {
            return delta->appendedTrackIds.contains(track.id);
        });
        undoPatternOwner();
        return {};
    }
    // Publish every new lane, clip and note in one graph transaction. Calling
    // addTrack() in the loop used to rebuild the entire project once per lane.
    rebuildGraph();

    m_undo.push("Import MIDI File",
                [this, delta, undoPatternOwner] {
                    if (TrackModel* targetTrack =
                            m_project.findTrack(delta->targetTrackId)) {
                        std::erase_if(targetTrack->clips, [&](const ClipModel& clip) {
                            return clip.id == delta->targetClip.id;
                        });
                    }
                    std::erase_if(m_project.tracks, [&](const TrackModel& track) {
                        return delta->appendedTrackIds.contains(track.id);
                    });
                    undoPatternOwner();
                    rebuildGraph();
                },
                [this, delta, redoPatternOwner] {
                    // Undo removes all imported lanes. Erase defensively before
                    // restoring so an externally modified document cannot
                    // acquire duplicate ids on redo.
                    std::erase_if(m_project.tracks, [&](const TrackModel& track) {
                        return delta->appendedTrackIds.contains(track.id);
                    });
                    redoPatternOwner();
                    if (TrackModel* targetTrack =
                            m_project.findTrack(delta->targetTrackId)) {
                        std::erase_if(targetTrack->clips, [&](const ClipModel& clip) {
                            return clip.id == delta->targetClip.id;
                        });
                        const std::size_t clipAt = std::min(
                            delta->targetClipIndex, targetTrack->clips.size());
                        targetTrack->clips.insert(
                            targetTrack->clips.begin() + std::ptrdiff_t(clipAt),
                            delta->targetClip);
                    }
                    const std::size_t trackAt = std::min(
                        delta->firstAppendedTrackIndex, m_project.tracks.size());
                    m_project.tracks.insert(
                        m_project.tracks.begin() + std::ptrdiff_t(trackAt),
                        delta->appendedTracks.begin(), delta->appendedTracks.end());
                    rebuildGraph();
                });
    return clipIds;
}

// ── Live notes ──────────────────────────────────────────────────────────────

bool EngineController::liveNoteOn(const std::string& trackId, int pitch,
                                  int velocity) {
    return liveMidiEvent(trackId, engine::MidiEvent::kNoteOn,
                         std::clamp(pitch, 0, 127),
                         std::clamp(velocity, 1, 127));
}

bool EngineController::liveNoteOff(const std::string& trackId, int pitch) {
    return liveMidiEvent(trackId, engine::MidiEvent::kNoteOff,
                         std::clamp(pitch, 0, 127), 0);
}

bool EngineController::liveMidiEvent(const std::string& trackId, int status,
                                     int data1, int data2) {
    if (status < 0x80 || status > 0xEF || data1 < 0 || data1 > 127 ||
        data2 < 0 || data2 > 127) {
        return false;
    }
    auto found = m_channels.find(trackId);
    if (found == m_channels.end() || !found->second.midiClips) return false;
    return found->second.midiClips->sendLiveEvent(engine::MidiEvent{
        0, std::uint8_t(status), std::uint8_t(data1), std::uint8_t(data2)});
}

std::string EngineController::liveNoteTarget(const std::string& preferred) const {
    if (!preferred.empty()) {
        if (const TrackModel* track = m_project.findTrack(preferred)) {
            if (trackAccepts(track->kind, ClipKind::Midi)) return preferred;
        }
    }
    for (const TrackModel& track : m_project.tracks) {
        if (trackAccepts(track.kind, ClipKind::Midi)) return track.id;
    }
    return {};
}

// ── Controller lanes ────────────────────────────────────────────────────────

namespace {

ControllerLane* findLane(ClipModel* clip, const std::string& laneId) {
    if (!clip) return nullptr;
    for (auto& lane : clip->lanes) {
        if (lane.id == laneId) return &lane;
    }
    return nullptr;
}

bool isPluginTargetLane(const ControllerLane& lane) noexcept {
    return lane.cc < 0 && !lane.parameterId.empty();
}

} // namespace

// ── Automation clips ───────────────────────────────────────────────────────

namespace {
/// The automation clip on `trackId` with this id, or null. Kind-checked for the
/// same reason `findMidiClip` is: every entry point below assumes the payload
/// it is about to touch is the one this clip actually carries.
ClipModel* findAutomationClip(ProjectModel& project, const std::string& trackId,
                              const std::string& clipId) {
    auto* track = project.findTrack(trackId);
    if (!track) return nullptr;
    for (auto& clip : track->clips) {
        if (clip.id == clipId && clip.kind == ClipKind::Automation) return &clip;
    }
    return nullptr;
}
} // namespace

std::string EngineController::automationTargetName(
    const AutomationTarget& target) const {
    // A lane made before anything was pointed at it — the free-standing kind.
    if (target.channelId.empty()) return "Automation";
    const TrackModel* channel = m_project.findTrack(target.channelId);
    const std::string owner = channel ? channel->name : std::string("?");
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume: return owner + " Volume";
        case AutomationTargetKind::TrackPan: return owner + " Pan";
        case AutomationTargetKind::TrackMute: return owner + " Mute";
        case AutomationTargetKind::SendLevel: {
            if (channel) {
                for (const SendModel& send : channel->sends) {
                    if (send.id != target.sendId) continue;
                    const TrackModel* bus =
                        m_project.findTrack(send.destinationTrackId);
                    return owner + " → " + (bus ? bus->name : std::string("Send"));
                }
            }
            return owner + " Send";
        }
        case AutomationTargetKind::PluginParameter: break;
    }

    // A plugin parameter reads as "plugin · parameter". The slot's own name
    // survives a plugin being swapped, so it is the honest half to show even
    // when the parameter can no longer be resolved.
    std::string slotName = "Instrument";
    if (channel) {
        if (!target.slotId.empty()) {
            for (const InsertModel& insert : channel->inserts) {
                if (insert.id == target.slotId) slotName = insert.name;
            }
        } else if (!channel->instrument.name.empty()) {
            slotName = channel->instrument.name;
        }
    }
    const std::string slotId =
        target.slotId.empty() && channel ? channel->instrument.id : target.slotId;
    for (const plugins::ParameterInfo& info :
         insertParameters(target.channelId, slotId)) {
        if (info.id == target.parameterId) return slotName + " · " + info.name;
    }
    return slotName + " · " + target.parameterId;
}

const plugins::ParameterInfo* EngineController::automationParameterInfo(
    const AutomationTarget& target) const {
    if (target.kind != AutomationTargetKind::PluginParameter) return nullptr;
    const TrackModel* channel = m_project.findTrack(target.channelId);
    if (!channel) return nullptr;
    // Empty means the instrument — the spelling `ControllerLane::slotId` uses,
    // kept here so one convention covers both kinds of curve.
    const std::string slotId =
        target.slotId.empty() ? channel->instrument.id : target.slotId;
    const plugins::PluginInstance* instance =
        const_cast<EngineController*>(this)->insertInstance(target.channelId, slotId);
    if (!instance) return nullptr;
    for (const plugins::ParameterInfo& info : instance->parameters()) {
        if (info.id == target.parameterId) return &info;
    }
    return nullptr;
}

double EngineController::automationToPlain(const AutomationTarget& target,
                                           double normalized) const {
    const double t = std::clamp(normalized, 0.0, 1.0);
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
            return gainFromNormalized(t);
        case AutomationTargetKind::TrackPan:
            return t * 2.0 - 1.0;              // 0…1 → −1…+1, centre at a half
        case AutomationTargetKind::TrackMute:
            // A switch, not a level: anything past halfway is "muted", so a
            // curve drawn with a shaky hand still lands on one of two states.
            return t >= 0.5 ? 1.0 : 0.0;
        case AutomationTargetKind::SendLevel:
            return t * double(kMaxSendLevel);
        case AutomationTargetKind::PluginParameter: break;
    }
    if (const plugins::ParameterInfo* info = automationParameterInfo(target))
        return info->minValue + (info->maxValue - info->minValue) * t;
    return t;
}

std::optional<double> EngineController::automationValueAtPlayhead(
    const AutomationTarget& target) const {
    rebuildAutomationReadoutCache();
    const auto found = m_automationReadoutCurves.find(target);
    if (found == m_automationReadoutCurves.end() || !found->second.active)
        return std::nullopt;

    const engine::LevelCurve& curve = found->second;
    const auto& points = curve.points;
    if (points.empty()) return curve.defaultValue;

    const double beats = positionSeconds() * (m_project.tempo / 60.0);
    const auto right = std::upper_bound(
        points.begin(), points.end(), beats,
        [](double value, const auto& point) { return value < point.first; });
    if (right == points.begin()) return curve.defaultValue;
    if (right == points.end()) return points.back().second;

    const auto left = std::prev(right);
    const double span = right->first - left->first;
    if (!(span > 0.0)) return right->second;
    const double t = (beats - left->first) / span;
    return left->second + (right->second - left->second) * t;
}

std::size_t EngineController::AutomationTargetHash::operator()(
    const AutomationTarget& target) const noexcept {
    std::size_t seed = std::hash<unsigned>{}(
        static_cast<unsigned>(target.kind));
    const auto mix = [&seed](const std::string& value) {
        const std::size_t hash = std::hash<std::string>{}(value);
        seed ^= hash + std::size_t{0x9e3779b9} + (seed << 6U) + (seed >> 2U);
    };
    mix(target.channelId);
    mix(target.slotId);
    mix(target.parameterId);
    mix(target.sendId);
    return seed;
}

void EngineController::invalidateAutomationReadoutCache() noexcept {
    m_automationReadoutCacheDirty = true;
}

void EngineController::rebuildAutomationReadoutCache() const {
    if (!m_automationReadoutCacheDirty) return;

    m_automationReadoutCurves.clear();
    const double beatsPerSecond = m_project.tempo / 60.0;
    for (const TrackModel& lane : m_project.tracks) {
        if (!isAutomationLane(lane)) continue;
        for (const ClipModel& clip : lane.clips) {
            if (clip.kind != ClipKind::Automation || clip.muted ||
                !clip.automation.active) continue;

            const AutomationTarget& target = clip.automation.target;
            engine::LevelCurve& curve = m_automationReadoutCurves[target];
            const auto toPlain = [this, &target](double normalized) {
                return automationToPlain(target, normalized);
            };
            if (!curve.active) {
                curve.active = true;
                curve.defaultValue = toPlain(clip.automation.defaultValue);
            }
            appendCurvePoints(curve.points, clip, beatsPerSecond, toPlain);
        }
    }

    for (auto& [target, curve] : m_automationReadoutCurves) {
        (void)target;
        std::sort(curve.points.begin(), curve.points.end(),
                  [](const auto& a, const auto& b) {
                      return a.first < b.first;
                  });
    }
    m_automationReadoutCacheDirty = false;
}

std::string EngineController::automationValueText(const AutomationTarget& target,
                                                 double normalized) const {
    const double plain = automationToPlain(target, normalized);
    char buffer[64];
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
        case AutomationTargetKind::SendLevel: {
            if (plain <= 1e-5) return "-inf dB";
            std::snprintf(buffer, sizeof buffer, "%+.1f dB",
                          20.0 * std::log10(plain));
            return buffer;
        }
        case AutomationTargetKind::TrackPan: {
            if (std::abs(plain) < 0.005) return "C";
            std::snprintf(buffer, sizeof buffer, "%s%.0f",
                          plain < 0 ? "L" : "R", std::abs(plain) * 100.0);
            return buffer;
        }
        case AutomationTargetKind::TrackMute:
            return plain >= 0.5 ? "Muted" : "Open";
        case AutomationTargetKind::PluginParameter: break;
    }

    // The plugin's own words where it has any — "440 Hz", "2:1", "Bypass" —
    // falling back to the number and whatever unit it declared.
    const TrackModel* channel = m_project.findTrack(target.channelId);
    if (channel) {
        const std::string slotId =
            target.slotId.empty() ? channel->instrument.id : target.slotId;
        if (plugins::PluginInstance* instance =
                const_cast<EngineController*>(this)->insertInstance(target.channelId,
                                                                   slotId)) {
            const std::int32_t index =
                instance->parameterIndexForId(target.parameterId);
            if (index >= 0) {
                std::string text =
                    instance->parameterText(std::uint32_t(index), plain);
                if (!text.empty()) return text;
            }
        }
    }
    std::snprintf(buffer, sizeof buffer, "%.3g", plain);
    std::string text = buffer;
    if (const plugins::ParameterInfo* info = automationParameterInfo(target)) {
        if (!info->unit.empty()) text += " " + info->unit;
    }
    return text;
}

double EngineController::plainToAutomation(const AutomationTarget& target,
                                           double plain) const {
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
            return normalizedFromGain(plain);
        case AutomationTargetKind::TrackPan:
            return std::clamp((plain + 1.0) * 0.5, 0.0, 1.0);
        case AutomationTargetKind::TrackMute:
            return plain >= 0.5 ? 1.0 : 0.0;
        case AutomationTargetKind::SendLevel:
            return std::clamp(plain / double(kMaxSendLevel), 0.0, 1.0);
        case AutomationTargetKind::PluginParameter: break;
    }
    if (const plugins::ParameterInfo* info = automationParameterInfo(target)) {
        const double span = info->maxValue - info->minValue;
        if (std::abs(span) < 1e-12) return 0.0;
        return std::clamp((plain - info->minValue) / span, 0.0, 1.0);
    }
    return std::clamp(plain, 0.0, 1.0);
}

double EngineController::defaultAutomationValue(
    const AutomationTarget& target) const {
    const TrackModel* channel = m_project.findTrack(target.channelId);
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
            return channel ? normalizedFromGain(channel->volume) : 1.0;
        case AutomationTargetKind::TrackPan:
            return channel ? plainToAutomation(target, channel->pan) : 0.5;
        case AutomationTargetKind::TrackMute:
            return channel && channel->muted ? 1.0 : 0.0;
        case AutomationTargetKind::SendLevel: {
            if (!channel) return 0.5;
            for (const SendModel& send : channel->sends) {
                if (send.id == target.sendId)
                    return plainToAutomation(target, send.level);
            }
            return 0.5;
        }
        case AutomationTargetKind::PluginParameter: break;
    }
    const std::string slotId =
        target.slotId.empty() && channel ? channel->instrument.id : target.slotId;
    return plainToAutomation(
        target, insertParameter(target.channelId, slotId, target.parameterId));
}

double EngineController::automationResetValue(const AutomationTarget& target) const {
    switch (target.kind) {
        case AutomationTargetKind::TrackVolume:
            return normalizedFromGain(1.0);       // unity / 0 dB
        case AutomationTargetKind::TrackPan:
            return 0.5;                           // centre
        case AutomationTargetKind::TrackMute:
            return 0.0;                           // unmuted
        case AutomationTargetKind::SendLevel:
            return plainToAutomation(target, 0.5); // SendModel / knob default
        case AutomationTargetKind::PluginParameter:
            break;
    }
    if (const plugins::ParameterInfo* info = automationParameterInfo(target))
        return plainToAutomation(target, info->defaultValue);
    // A stale plugin target has no factory metadata left. Its captured value is
    // the only honest default available and is still preferable to arbitrary 0.
    return defaultAutomationValue(target);
}

std::string EngineController::addAutomationLane(const std::string& trackId,
                                                const AutomationTarget& target) {
    TrackModel model;
    model.id = newUuid();
    model.kind = TrackKind::Automation;
    model.name = automationTargetName(target);
    model.parentId = trackId;
    // Shorter than a channel lane: a curve needs height to be readable, but not
    // as much as a waveform, and a track with four of them still has to fit.
    // The grip strip along the top costs the curve some of that, so the lane
    // starts a little taller than the curve alone would need.
    model.height = 64.0;
    if (const TrackModel* owner = m_project.findTrack(trackId)) {
        model.color = owner->color;
    }

    const std::string laneId = model.id;
    // Placed directly under its track and everything already filed there, so a
    // second lane lands beside the first rather than above it.
    size_t at = m_project.tracks.size();
    if (!trackId.empty() && m_project.indexOf(trackId) != std::string::npos) {
        at = m_project.indexOf(trackId) + 1 + subtreeOf(m_project, trackId).size();
    }
    at = std::min(at, m_project.tracks.size());
    if (cloudProjectBound()) {
        const std::string afterId = at == 0 ? std::string()
                                             : m_project.tracks[at - 1].id;
        const auto result = submitSharedMutation(
            collab::AddTrack{model.id, model.kind, model.name, model.color,
                             model.parentId, afterId},
            "Add Automation Lane");
        return result == collab::SharedMutationResult::Submitted ? laneId
                                                                 : std::string{};
    }
    m_project.tracks.insert(m_project.tracks.begin() + std::ptrdiff_t(at),
                            std::move(model));
    // A lane nobody can see is not what asking for one means.
    if (auto* owner = m_project.findTrack(trackId))
        owner->automationExpanded = true;
    rebuildGraph();

    m_undo.push("Add Automation Lane",
                [this, laneId] { removeAutomationLane(laneId); },
                [this, trackId, target] { addAutomationLane(trackId, target); });
    return laneId;
}

std::vector<std::string> EngineController::automationLanesOf(
    const std::string& trackId) const {
    std::vector<std::string> lanes;
    for (const TrackModel& track : m_project.tracks) {
        if (isAutomationLane(track) && track.parentId == trackId)
            lanes.push_back(track.id);
    }
    return lanes;
}

void EngineController::removeAutomationLane(const std::string& laneTrackId) {
    const TrackModel* lane = m_project.findTrack(laneTrackId);
    if (!lane || !isAutomationLane(*lane)) return;
    removeTrack(laneTrackId);
}

std::string EngineController::addAutomationClip(const std::string& laneTrackId,
                                                const AutomationTarget& target,
                                                double startSeconds,
                                                double lengthSeconds) {
    auto* lane = m_project.findTrack(laneTrackId);
    if (!lane || !isAutomationLane(*lane)) return {};

    ClipModel clip;
    clip.id = newUuid();
    clip.kind = ClipKind::Automation;
    clip.startSeconds = std::max(0.0, startSeconds);
    // "As far as the content goes" — a fresh curve should cover the arrangement
    // rather than land as a stub the user has to stretch before drawing on it.
    // An empty project still gets something to draw on: eight bars.
    double length = lengthSeconds;
    if (!(length > 0.0)) {
        const double bars = beatsToSeconds(
            8.0 * double(std::max(1, m_project.timeSigNumerator)), m_project.tempo);
        length = std::max(durationSeconds() - clip.startSeconds, bars);
    }
    clip.durationSeconds = length;
    clip.color = lane->color;
    clip.automation.target = target;
    clip.automation.defaultValue = defaultAutomationValue(target);
    const double endBeats = secondsToBeats(length, m_project.tempo);
    clip.automation.points = {
        AutomationPoint{0.0, clip.automation.defaultValue,
                        AutomationSegment::Linear, 0.0, newUuid()},
        AutomationPoint{endBeats, clip.automation.defaultValue,
                        AutomationSegment::Linear, 0.0, newUuid()},
    };
    clip.name = automationTargetName(target);

    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::AddClip{
            laneTrackId, clip.id, clip.kind, clip.name, clip.startSeconds,
            clip.durationSeconds, clip.color,
            lane->clips.empty() ? std::string() : lane->clips.back().id});
        appendCommand(batch, collab::SetAutomationTarget{
            laneTrackId, clip.id, clip.automation.target});
        appendCommand(batch, collab::SetAutomationDefault{
            laneTrackId, clip.id, clip.automation.defaultValue});
        for (std::size_t index = 0; index < clip.automation.points.size();
             ++index) {
            appendCommand(batch, collab::UpsertAutomationPoint{
                laneTrackId, clip.id, {}, clip.automation.points[index],
                index == 0 ? std::string()
                           : clip.automation.points[index - 1].id});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Add Automation Clip");
        return result == collab::SharedMutationResult::Submitted ? clip.id
                                                                 : std::string{};
    }

    const ClipModel created = clip;
    lane->clips.push_back(std::move(clip));
    syncAllAutomation();
    updateTimelineDuration();

    const std::string clipId = created.id;
    m_undo.push("Add Automation Clip",
                [this, laneTrackId, clipId] { removeClip(laneTrackId, clipId); },
                [this, laneTrackId, created] {
                    if (auto* l = m_project.findTrack(laneTrackId)) {
                        l->clips.push_back(created);
                        syncAllAutomation();
                        updateTimelineDuration();
                    }
                });
    return clipId;
}

std::pair<std::string, std::string> EngineController::findAutomation(
    const AutomationTarget& target) const {
    for (const TrackModel& track : m_project.tracks) {
        if (!isAutomationLane(track)) continue;
        for (const ClipModel& clip : track.clips) {
            if (clip.kind == ClipKind::Automation && clip.automation.target == target)
                return {track.id, clip.id};
        }
    }
    return {};
}

std::pair<std::string, std::string> EngineController::ensureAutomation(
    const AutomationTarget& target) {
    if (auto found = findAutomation(target); !found.first.empty()) {
        // Already automated. Opening the owner is the whole of the work: the
        // lane exists, and the only reason to ask again is to look at it.
        if (const TrackModel* lane = m_project.findTrack(found.first)) {
            if (auto* owner = m_project.findTrack(lane->parentId))
                owner->automationExpanded = true;
        }
        return found;
    }
    // A lane of its own rather than another clip on whatever lane is already
    // there: the lane wears the target's name, and two curves sharing one row
    // is a thing the user arranges deliberately, not a default.
    const std::size_t mark = undoDepth();
    const std::string lane = addAutomationLane(target.channelId, target);
    if (lane.empty()) return {};
    const std::string clip = addAutomationClip(lane, target, 0.0);
    collapseUndo(mark, "Automate " + automationTargetName(target));
    return {lane, clip};
}

void EngineController::setAutomationTarget(const std::string& trackId,
                                           const std::string& clipId,
                                           const AutomationTarget& target) {
    auto* clip = findAutomationClip(m_project, trackId, clipId);
    if (!clip || clip->automation.target == target) return;
    const AutomationTarget before = clip->automation.target;
    const std::string beforeName = clip->name;
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCommand(batch, collab::SetAutomationTarget{
            trackId, clipId, target});
        appendCommand(batch, collab::SetClipProperty{
            trackId, clipId, collab::ClipProperty::Name,
            automationTargetName(target)});
        if (!clip->automation.active) {
            const double value = defaultAutomationValue(target);
            appendCommand(batch, collab::SetAutomationDefault{
                trackId, clipId, value});
            for (std::size_t index = 0;
                 index < clip->automation.points.size(); ++index) {
                AutomationPoint point = clip->automation.points[index];
                point.value = value;
                appendCommand(batch, collab::UpsertAutomationPoint{
                    trackId, clipId, {}, point,
                    index == 0 ? std::string()
                               : clip->automation.points[index - 1].id});
            }
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Retarget Automation");
        return;
    }
    clip->automation.target = target;
    clip->name = automationTargetName(target);
    if (!clip->automation.active) {
        const double value = defaultAutomationValue(target);
        clip->automation.defaultValue = value;
        for (AutomationPoint& point : clip->automation.points)
            point.value = value;
    }
    // Both endpoints are republished in the same control-thread turn: the old
    // target must stop moving as the new one starts, but unrelated channels do
    // not need their immutable automation snapshots rebuilt.
    syncAutomationTarget(before);
    if (target != before) syncAutomationTarget(target);

    m_undo.push("Retarget Automation",
                [this, trackId, clipId, before] {
                    setAutomationTarget(trackId, clipId, before);
                },
                [this, trackId, clipId, target] {
                    setAutomationTarget(trackId, clipId, target);
                });
    (void)beforeName;
}

void EngineController::setAutomationPoints(const std::string& trackId,
                                           const std::string& clipId,
                                           std::vector<AutomationPoint> points,
                                           bool active) {
    auto* clip = findAutomationClip(m_project, trackId, clipId);
    if (!clip) return;
    normalizeAutomation(points);
    // Re-sending the untouched endpoints is not a user edit.
    if (points == clip->automation.points && active && !clip->automation.active)
        return;
    if (points == clip->automation.points && active == clip->automation.active)
        return;
    clip->automation.points = std::move(points);
    clip->automation.active = active;
    // The lane can live anywhere, but its target names the one channel whose
    // compiled snapshot changed. A live point drag should not rebuild every
    // plugin and fader curve in the project on every mouse event.
    syncAutomationTarget(clip->automation.target);
}

void EngineController::commitAutomationEdit(const std::string& trackId,
                                            const std::string& clipId,
                                            std::vector<AutomationPoint> before,
                                            const std::string& label,
                                            bool activeBefore) {
    auto* clip = findAutomationClip(m_project, trackId, clipId);
    if (!clip) return;
    normalizeAutomation(before);
    std::vector<AutomationPoint> after = clip->automation.points;
    const bool activeAfter = clip->automation.active;
    if (after == before && activeAfter == activeBefore) return;
    if (after == before) {
        setAutomationPoints(trackId, clipId, before, activeBefore);
        return;
    }

    if (cloudProjectBound()) {
        std::unordered_set<std::string> afterIds;
        for (const AutomationPoint& point : after) afterIds.insert(point.id);
        std::unordered_map<std::string, const AutomationPoint*> beforeById;
        for (const AutomationPoint& point : before)
            beforeById.emplace(point.id, &point);
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const AutomationPoint& point : before) {
            if (!afterIds.contains(point.id))
                appendCommand(batch, collab::DeleteAutomationPoint{
                    trackId, clipId, {}, point.id});
        }
        for (std::size_t index = 0; index < after.size(); ++index) {
            const AutomationPoint& point = after[index];
            const auto old = beforeById.find(point.id);
            if (old != beforeById.end() && *old->second == point) continue;
            appendCommand(batch, collab::UpsertAutomationPoint{
                trackId, clipId, {}, point,
                index == 0 ? std::string() : after[index - 1].id});
        }
        if (activeAfter != activeBefore)
            appendCommand(batch, collab::SetAutomationActive{
                trackId, clipId, activeAfter});
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, label);
        if (result == collab::SharedMutationResult::Blocked)
            setAutomationPoints(trackId, clipId, before, activeBefore);
        if (result != collab::SharedMutationResult::LocalFallback) return;
    }

    m_undo.push(label,
                [this, trackId, clipId, before, activeBefore] {
                    setAutomationPoints(trackId, clipId, before, activeBefore);
                },
                [this, trackId, clipId, after, activeAfter] {
                    setAutomationPoints(trackId, clipId, after, activeAfter);
                });
}

std::string EngineController::addControllerLane(const std::string& trackId,
                                                const std::string& clipId,
                                                const std::string& name, int cc) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return {};

    ControllerLane lane;
    lane.id = newUuid();
    lane.name = name.empty() ? "CC" : name;
    lane.cc = cc;
    if (cloudProjectBound()) {
        collab::ControllerLaneTarget target;
        target.cc = lane.cc;
        target.parameterId = lane.parameterId;
        target.slotId = lane.slotId;
        const auto result = submitSharedMutation(
            collab::AddControllerLane{
                trackId, clipId, lane.id, lane.name, target,
                lane.defaultValue,
                clip->lanes.empty() ? std::string() : clip->lanes.back().id},
            "Add Controller Lane");
        return result == collab::SharedMutationResult::Submitted ? lane.id
                                                                 : std::string{};
    }
    clip->lanes.push_back(lane);

    m_undo.push("Add Controller Lane",
                [this, trackId, clipId, id = lane.id] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        std::erase_if(c->lanes, [&](const ControllerLane& l) {
                            return l.id == id;
                        });
                    }
                },
                [this, trackId, clipId, lane] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->lanes.push_back(lane);
                    }
                });
    return lane.id;
}

void EngineController::removeControllerLane(const std::string& trackId,
                                            const std::string& clipId,
                                            const std::string& laneId) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    auto* lane = findLane(clip, laneId);
    if (!lane) return;
    const ControllerLane snapshot = *lane;
    const bool pluginTarget = isPluginTargetLane(snapshot);

    if (cloudProjectBound()) {
        (void)submitSharedMutation(
            collab::DeleteControllerLane{trackId, clipId, laneId},
            "Remove Controller Lane");
        return;
    }

    std::erase_if(clip->lanes,
                  [&](const ControllerLane& l) { return l.id == laneId; });
    if (pluginTarget) {
        if (TrackModel* track = m_project.findTrack(trackId))
            syncTrackAutomation(*track);
    }

    m_undo.push("Remove Controller Lane",
                [this, trackId, clipId, snapshot, pluginTarget] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->lanes.push_back(snapshot);
                        if (pluginTarget) {
                            if (TrackModel* track = m_project.findTrack(trackId))
                                syncTrackAutomation(*track);
                        }
                    }
                },
                [this, trackId, clipId, laneId, pluginTarget] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        std::erase_if(c->lanes, [&](const ControllerLane& l) {
                            return l.id == laneId;
                        });
                        if (pluginTarget) {
                            if (TrackModel* track = m_project.findTrack(trackId))
                                syncTrackAutomation(*track);
                        }
                    }
                });
}

void EngineController::setLaneTarget(const std::string& trackId,
                                     const std::string& clipId,
                                     const std::string& laneId,
                                     const std::string& slotId,
                                     const std::string& parameterId) {
    auto* lane = findLane(findMidiClip(m_project, trackId, clipId), laneId);
    if (!lane) return;

    const ControllerLane before = *lane;
    const collab::ControllerLaneTarget next{
        parameterId.empty() ? 1 : -1, parameterId, slotId};
    const auto shared = submitSharedMutation(
        collab::SetControllerLaneTarget{trackId, clipId, laneId, next},
        "Retarget Lane");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    lane->parameterId = parameterId;
    lane->slotId = slotId;
    // −1 is what marks a lane as addressing a plugin rather than a controller;
    // clearing the parameter puts it back on CC 1 rather than leaving it
    // pointing at nothing.
    lane->cc = parameterId.empty() ? 1 : -1;
    const ControllerLane after = *lane;

    auto apply = [this, trackId, clipId, laneId](const ControllerLane& value) {
        if (auto* target = findLane(findMidiClip(m_project, trackId, clipId), laneId)) {
            const bool affectsPlugin =
                isPluginTargetLane(*target) || isPluginTargetLane(value);
            target->parameterId = value.parameterId;
            target->slotId = value.slotId;
            target->cc = value.cc;
            if (affectsPlugin) {
                if (TrackModel* track = m_project.findTrack(trackId))
                    syncTrackAutomation(*track);
            }
        }
    };
    if (isPluginTargetLane(before) || isPluginTargetLane(after)) {
        if (TrackModel* track = m_project.findTrack(trackId))
            syncTrackAutomation(*track);
    }
    m_undo.push("Retarget Lane", [apply, before] { apply(before); },
                [apply, after] { apply(after); });
}

void EngineController::setLanePoints(const std::string& trackId,
                                     const std::string& clipId,
                                     const std::string& laneId,
                                     std::vector<AutomationPoint> points) {
    auto* lane = findLane(findMidiClip(m_project, trackId, clipId), laneId);
    if (!lane) return;
    normalizeAutomation(points);
    lane->points = std::move(points);
    // Ordinary CC lanes are document-only until MIDI CC routing is supported;
    // rebuilding every loaded plugin's immutable curve set for them cannot
    // change playback and turns a pencil stroke into repeated full-track work.
    if (isPluginTargetLane(*lane)) {
        if (TrackModel* track = m_project.findTrack(trackId))
            syncTrackAutomation(*track);
    }
}

void EngineController::commitLaneEdit(const std::string& trackId,
                                      const std::string& clipId,
                                      const std::string& laneId,
                                      std::vector<AutomationPoint> before,
                                      const std::string& label) {
    auto* lane = findLane(findMidiClip(m_project, trackId, clipId), laneId);
    if (!lane) return;
    normalizeAutomation(before);
    if (before == lane->points) return;   // nothing actually moved

    if (cloudProjectBound()) {
        const std::vector<AutomationPoint> after = lane->points;
        std::unordered_set<std::string> afterIds;
        for (const AutomationPoint& point : after) afterIds.insert(point.id);
        std::unordered_map<std::string, const AutomationPoint*> beforeById;
        for (const AutomationPoint& point : before)
            beforeById.emplace(point.id, &point);
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const AutomationPoint& point : before) {
            if (!afterIds.contains(point.id))
                appendCommand(batch, collab::DeleteAutomationPoint{
                    trackId, clipId, laneId, point.id});
        }
        for (std::size_t index = 0; index < after.size(); ++index) {
            const AutomationPoint& point = after[index];
            const auto old = beforeById.find(point.id);
            if (old != beforeById.end() && *old->second == point) continue;
            appendCommand(batch, collab::UpsertAutomationPoint{
                trackId, clipId, laneId, point,
                index == 0 ? std::string() : after[index - 1].id});
        }
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, label);
        if (result == collab::SharedMutationResult::Blocked) {
            lane->points = before;
            if (isPluginTargetLane(*lane)) {
                if (TrackModel* track = m_project.findTrack(trackId))
                    syncTrackAutomation(*track);
            }
        }
        if (result != collab::SharedMutationResult::LocalFallback) return;
    }

    m_undo.push(label,
                [this, trackId, clipId, laneId, before] {
                    if (auto* l = findLane(findMidiClip(m_project, trackId, clipId),
                                           laneId)) {
                        l->points = before;
                        if (isPluginTargetLane(*l)) {
                            if (TrackModel* track = m_project.findTrack(trackId))
                                syncTrackAutomation(*track);
                        }
                    }
                },
                [this, trackId, clipId, laneId, after = lane->points] {
                    if (auto* l = findLane(findMidiClip(m_project, trackId, clipId),
                                           laneId)) {
                        l->points = after;
                        if (isPluginTargetLane(*l)) {
                            if (TrackModel* track = m_project.findTrack(trackId))
                                syncTrackAutomation(*track);
                        }
                    }
                });
}

void EngineController::setClipNotes(const std::string& trackId,
                                    const std::string& clipId,
                                    std::vector<NoteModel> notes,
                                    const std::string& label) {
    auto* clip = findMidiClip(m_project, trackId, clipId);
    if (!clip) return;

    // Clamp here rather than in every caller: the tools are pure functions over
    // note vectors and have no business knowing the document's legal ranges.
    for (auto& note : notes) {
        if (note.id.empty()) note.id = newUuid();
        note.pitch = std::clamp(note.pitch, kMinPitch, kMaxPitch);
        note.startBeats = std::max(0.0, note.startBeats);
        note.lengthBeats = std::max(kMinNoteBeats, note.lengthBeats);
        note.velocity = std::clamp(note.velocity, 1, 127);
        note.pan = std::clamp(note.pan, -1.0f, 1.0f);
    }

    if (clip->notes == notes) return;
    const bool playbackChanged = !sameNotePlayback(clip->notes, notes);
    const bool geometryChanged = !sameNoteGeometry(clip->notes, notes);

    if (noteEditTargets(trackId, clipId)) {
        captureNoteEditBeforeMutation(trackId, clipId, *clip);
        clip->notes = std::move(notes);
        if (playbackChanged) {
            if (const TrackModel* track = m_project.findTrack(trackId))
                publishOrDeferNotePlayback(trackId, clipId, *track,
                                           geometryChanged);
        } else if (geometryChanged) {
            bumpMidiNotesRevision(trackId);
        }
        return;
    }

    if (cloudProjectBound()) {
        std::unordered_set<std::string> afterIds;
        for (const NoteModel& note : notes) afterIds.insert(note.id);
        std::unordered_map<std::string, const NoteModel*> beforeById;
        for (const NoteModel& note : clip->notes)
            beforeById.emplace(note.id, &note);
        auto batch = std::make_shared<collab::BatchCommand>();
        for (const NoteModel& note : clip->notes) {
            if (!afterIds.contains(note.id))
                appendCommand(batch, collab::DeleteMidiNote{
                    trackId, clipId, note.id});
        }
        for (std::size_t index = 0; index < notes.size(); ++index) {
            const NoteModel& note = notes[index];
            const auto before = beforeById.find(note.id);
            if (before != beforeById.end() && *before->second == note) continue;
            appendCommand(batch, collab::UpsertMidiNote{
                trackId, clipId, note,
                index == 0 ? std::string() : notes[index - 1].id});
        }
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)}, label);
        return;
    }

    std::vector<NoteModel> before = clip->notes;
    clip->notes = notes;
    if (playbackChanged) {
        if (const TrackModel* track = m_project.findTrack(trackId))
            syncTrackNotes(*track, geometryChanged);
    } else if (geometryChanged) {
        bumpMidiNotesRevision(trackId);
    }

    // Both halves replay a captured vector by value. Anything that recomputed
    // the transform on redo would mint fresh uuids and orphan this entry's undo,
    // the same trap `addNote` documents.
    m_undo.push(label,
                [this, trackId, clipId, before = std::move(before),
                 playbackChanged, geometryChanged] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->notes = before;
                        if (playbackChanged) {
                            if (const TrackModel* track =
                                    m_project.findTrack(trackId)) {
                                syncTrackNotes(*track, geometryChanged);
                            }
                        } else if (geometryChanged) {
                            bumpMidiNotesRevision(trackId);
                        }
                    }
                },
                [this, trackId, clipId, after = std::move(notes),
                 playbackChanged, geometryChanged] {
                    if (auto* c = findMidiClip(m_project, trackId, clipId)) {
                        c->notes = after;
                        if (playbackChanged) {
                            if (const TrackModel* track =
                                    m_project.findTrack(trackId)) {
                                syncTrackNotes(*track, geometryChanged);
                            }
                        } else if (geometryChanged) {
                            bumpMidiNotesRevision(trackId);
                        }
                    }
                });
}

// ── Master / metering ──────────────────────────────────────────────────────

void EngineController::setMasterVolume(float volume) {
    const float before = m_project.masterVolume;
    const float requested = std::clamp(volume, 0.0f, 2.0f);
    if (requested == before) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::MasterVolume,
                                 double(requested)},
        "Set Master Volume");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    setMasterVolumeLive(volume);
    const float after = m_project.masterVolume;
    if (after == before) return;
    m_undo.push("Set Master Volume",
                [this, before] { setMasterVolumeLive(before); },
                [this, after] { setMasterVolumeLive(after); });
}

void EngineController::setMasterPan(float pan) {
    const float before = m_project.masterPan;
    const float requested = std::clamp(pan, -1.0f, 1.0f);
    if (requested == before) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::MasterPan,
                                 double(requested)},
        "Set Master Pan");
    if (shared != collab::SharedMutationResult::LocalFallback) return;
    setMasterPanLive(pan);
    const float after = m_project.masterPan;
    if (after == before) return;
    m_undo.push("Set Master Pan",
                [this, before] { setMasterPanLive(before); },
                [this, after] { setMasterPanLive(after); });
}

void EngineController::setMasterVolumeLive(float volume) {
    const float applied = std::clamp(volume, 0.0f, 2.0f);
    if (m_project.masterVolume == applied) return;
    m_project.masterVolume = applied;
    if (m_masterFader) m_masterFader->setGain(applied);
}

void EngineController::commitMasterVolumeEdit(float before,
                                              const std::string& label) {
    const float after = m_project.masterVolume;
    if (after == before) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::MasterVolume,
                                 double(after)},
        label);
    if (shared == collab::SharedMutationResult::Blocked) {
        setMasterVolumeLive(before);
        return;
    }
    if (shared == collab::SharedMutationResult::Submitted) return;
    m_undo.push(label, [this, before] { setMasterVolumeLive(before); },
                [this, after] { setMasterVolumeLive(after); });
}

void EngineController::setMasterPanLive(float pan) {
    const float applied = std::clamp(pan, -1.0f, 1.0f);
    if (m_project.masterPan == applied) return;
    m_project.masterPan = applied;
    if (m_masterFader) m_masterFader->setPan(applied);
}

void EngineController::commitMasterPanEdit(float before,
                                           const std::string& label) {
    const float after = m_project.masterPan;
    if (after == before) return;
    const auto shared = submitSharedMutation(
        collab::SetProjectScalar{collab::ProjectScalar::MasterPan,
                                 double(after)},
        label);
    if (shared == collab::SharedMutationResult::Blocked) {
        setMasterPanLive(before);
        return;
    }
    if (shared == collab::SharedMutationResult::Submitted) return;
    m_undo.push(label, [this, before] { setMasterPanLive(before); },
                [this, after] { setMasterPanLive(after); });
}

float EngineController::trackPeak(const std::string& trackId) const {
    auto found = m_channels.find(trackId);
    if (found == m_channels.end() || !found->second.meter) return 0.0f;
    const engine::MeterNode& meter = *found->second.meter;
    return std::max(meter.peakLeft(), meter.peakRight());
}

float EngineController::trackRms(const std::string& trackId) const {
    // The meter node publishes peak only; RMS follows when the meter grows a
    // proper ballistics stage.
    return trackPeak(trackId) * 0.707f;
}

float EngineController::samplerFxPeakLeft(const std::string& trackId) const {
    const auto found = m_channels.find(trackId);
    return found == m_channels.end() || !found->second.samplerMeter
               ? 0.0f
               : found->second.samplerMeter->peakLeft();
}

float EngineController::samplerFxPeakRight(const std::string& trackId) const {
    const auto found = m_channels.find(trackId);
    return found == m_channels.end() || !found->second.samplerMeter
               ? 0.0f
               : found->second.samplerMeter->peakRight();
}

float EngineController::clipFxPeakLeft(const std::string& trackId,
                                        const std::string& clipId) const {
    const auto channel = m_channels.find(trackId);
    if (channel == m_channels.end()) return 0.0f;
    const auto clip = channel->second.clipFx.find(clipId);
    return clip == channel->second.clipFx.end() || !clip->second.meter
               ? 0.0f
               : clip->second.meter->peakLeft();
}

float EngineController::clipFxPeakRight(const std::string& trackId,
                                         const std::string& clipId) const {
    const auto channel = m_channels.find(trackId);
    if (channel == m_channels.end()) return 0.0f;
    const auto clip = channel->second.clipFx.find(clipId);
    return clip == channel->second.clipFx.end() || !clip->second.meter
               ? 0.0f
               : clip->second.meter->peakRight();
}

float EngineController::masterPeak() const {
    return std::max(m_engine.masterPeakLeft(), m_engine.masterPeakRight());
}
float EngineController::masterRms() const { return masterPeak() * 0.707f; }
float EngineController::masterPeakLeft() const { return m_engine.masterPeakLeft(); }
float EngineController::masterPeakRight() const { return m_engine.masterPeakRight(); }
engine::RealtimeEngine::MasterSpectrum EngineController::masterSpectrum() const {
    return m_engine.masterSpectrum();
}
void EngineController::addMasterSpectrumConsumer() noexcept {
    m_engine.addMasterSpectrumConsumer();
}
void EngineController::removeMasterSpectrumConsumer() noexcept {
    m_engine.removeMasterSpectrumConsumer();
}
float EngineController::dspLoad() const { return m_engine.dspLoad(); }

// ── Recording ──────────────────────────────────────────────────────────────

void EngineController::publishRecorders() {
    auto list = std::make_shared<RecorderList>();
    list->reserve(m_captures.size());
    for (const auto& capture : m_captures) {
        if (capture.recorder) list->push_back(capture.recorder);
    }
    m_activeRecorders.publish(std::shared_ptr<const RecorderList>(list));
}

void EngineController::setRecordingPrefs(const RecordingPrefs& prefs) {
    m_recording = prefs;
    m_recording.compCrossfadeMs = std::clamp(m_recording.compCrossfadeMs, 0.0, 20.0);
}

void EngineController::setTrackRecordMode(const std::string& trackId,
                                          TrackRecordMode mode) {
    auto* track = m_project.findTrack(trackId);
    if (!track || track->recordMode == mode) return;
    const TrackRecordMode previous = track->recordMode;
    track->recordMode = mode;

    m_undo.push("Track Recording Mode",
                [this, trackId, previous] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->recordMode = previous;
                    }
                },
                [this, trackId, mode] {
                    if (auto* t = m_project.findTrack(trackId)) {
                        t->recordMode = mode;
                    }
                });
}

RecordMode EngineController::effectiveRecordMode(const std::string& trackId) const {
    RecordMode mode = m_recording.mode;
    if (const auto* track = m_project.findTrack(trackId)) {
        if (track->recordMode == TrackRecordMode::Overwrite) {
            mode = RecordMode::Overwrite;
        } else if (track->recordMode == TrackRecordMode::Layers) {
            mode = RecordMode::Layers;
        }
    }
    if (m_recording.inverted) {
        mode = mode == RecordMode::Layers ? RecordMode::Overwrite
                                          : RecordMode::Layers;
    }
    return mode;
}

EngineController::FrozenRecordingSemantics
EngineController::frozenRecordingSemantics(const std::string& trackId) const {
    FrozenRecordingSemantics semantics;
    semantics.mode = effectiveRecordMode(trackId);
    semantics.loopEnabled = isLoopEnabled();
    semantics.loopStartSeconds = loopStartSeconds();
    semantics.loopEndSeconds = loopEndSeconds();
    semantics.loopCreatesTakes = m_recording.loopCreatesTakes;
    semantics.trimTakesToRegion = m_recording.trimTakesToRegion;
    semantics.autoExpandAfterRecord = m_recording.autoExpandAfterRecord;
    semantics.compCrossfadeMs = m_recording.compCrossfadeMs;
    semantics.autoMonitorOnRecord = m_recording.autoMonitorOnRecord;
    semantics.monitorStopPolicy = m_recording.monitorStopPolicy;
    return semantics;
}

bool EngineController::startRecording(const std::string& trackId) {
    return startRecordingTracks({trackId});
}

bool EngineController::startRecordingTracks(
    const std::vector<std::string>& trackIds) {
    return startRecordingTracksImpl(trackIds, false);
}

bool EngineController::canStartRecordingTracksExactly(
    const std::vector<std::string>& trackIds) const {
    if (isRecording() || trackIds.empty()) return false;
    std::unordered_set<std::string> unique;
    unique.reserve(trackIds.size());
    for (const std::string& trackId : trackIds) {
        if (trackId.empty() || !unique.insert(trackId).second) return false;
        const TrackModel* track = m_project.findTrack(trackId);
        if (!track || !isRecordable(*track)) return false;
    }
    return true;
}

bool EngineController::startRecordingTracksExactly(
    const std::vector<std::string>& trackIds) {
    return startRecordingTracksImpl(trackIds, true);
}

bool EngineController::startRecordingTracksImpl(
    const std::vector<std::string>& trackIds, bool requireEveryTarget) {
    // Production V1 deliberately keeps recording local-only.  This boundary
    // also protects non-UI callers and stale shortcuts.
    if (cloudProjectBound()) return false;
    if (isRecording() || trackIds.empty()) return false;
    if (requireEveryTarget && !canStartRecordingTracksExactly(trackIds))
        return false;
    stopPreview();   // never into a take

    // Restart returns a new take to the run anchor just like Play. Resume keeps
    // the ordinary punch-in behaviour at the current playhead.
    applyTransportStartPolicy();
    const double startSeconds = toSeconds(m_engine.transport().position());

    std::error_code ec;
    fs::create_directories(m_recordDir, ec);

    std::vector<Capture> prepared;
    prepared.reserve(trackIds.size());

    const auto rollbackExactStart = [&] {
        bool localStateChanged = false;
        for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
            if (it->recorder && it->recorder->isRecording())
                it->recorder->stopRecording();
            TrackModel* track = m_project.findTrack(it->trackId);
            if (!track) continue;
            localStateChanged = localStateChanged ||
                                track->armed != it->armedBefore ||
                                track->monitor != it->monitorBefore ||
                                track->monitorAuto != it->monitorAutoBefore;
            track->armed = it->armedBefore;
            track->monitor = it->monitorBefore;
            track->monitorAuto = it->monitorAutoBefore;
        }
        prepared.clear();
        if (localStateChanged) rebuildGraph();
    };

    for (const std::string& trackId : trackIds) {
        auto* track = m_project.findTrack(trackId);
        if (!track || !isRecordable(*track)) {
            if (requireEveryTarget) {
                rollbackExactStart();
                return false;
            }
            continue;
        }

        Capture capture;
        capture.trackId = trackId;
        capture.startSeconds = startSeconds;
        capture.semantics = frozenRecordingSemantics(trackId);
        capture.monitorManaged = capture.semantics.autoMonitorOnRecord;
        // "Before" means before the *whole* gesture: a count-in has already
        // opened the monitor by now, and restoring to that would leave it on
        // for good.
        capture.monitorBefore = track->monitor;
        for (const auto& [id, monitor] : m_countInMonitorBefore) {
            if (id == trackId) {
                capture.monitorBefore = monitor;
                capture.monitorManaged = true;
            }
        }
        capture.armedBefore = track->armed;
        capture.monitorAutoBefore = track->monitorAuto;

        // Exactly as wide as the input the track is pointed at. Capturing a
        // pair from a mono source wrote a file whose right channel was whatever
        // happened to be on the next hardware input — silence, usually — and
        // that file plays out of one speaker on any track that is not folded to
        // mono.
        const std::uint32_t captureChannels =
            std::clamp(track->inputChannelCount, 1u, 2u);
        capture.recorder = std::make_shared<audio::AudioRecorder>();
        if (!capture.recorder->initialize(m_sampleRate, captureChannels)) {
            if (requireEveryTarget) {
                rollbackExactStart();
                return false;
            }
            continue;
        }
        // `setRecordPath` names the *directory* a capture goes in; the recorder
        // mints the file name itself and reports it back through the session.
        capture.recorder->setRecordPath(m_recordDir);
        capture.recorder->setInputChannels(
            audio::ChannelCount(track->inputChannel),
            audio::ChannelCount(captureChannels));
        if (!capture.recorder->startRecording(0, m_engine.transport().position())) {
            if (requireEveryTarget) {
                rollbackExactStart();
                return false;
            }
            continue;
        }
        capture.path = capture.recorder->session().filePath;

        // Arming is implied by recording onto the track — the user picked it,
        // which is the whole intent the arm button expresses.
        track->armed = true;
        if (capture.semantics.autoMonitorOnRecord) applySmartMonitoring(*track);

        prepared.push_back(std::move(capture));
    }

    if (prepared.empty()) return false;
    if (requireEveryTarget && prepared.size() != trackIds.size()) {
        rollbackExactStart();
        return false;
    }

    for (Capture& capture : prepared) {
        m_recordingTracks.push_back(capture.trackId);
        m_captures.push_back(std::move(capture));
    }

    // Whatever was counting in has landed here.
    m_countInTracks.clear();
    m_countInBeatsLeft = 0;
    m_countInToNextBeat = 0.0;
    m_countInRequiresExactTargets = false;
    m_countInMonitorBefore.clear();

    publishRecorders();
    rebuildGraph();            // arming and monitoring both grow input nodes
    m_engine.transport().startRecording();
    return true;
}

namespace {
/// One beat in seconds at `tempo`, with a sane fallback for a broken project.
double beatLength(double tempo) { return tempo > 0.0 ? 60.0 / tempo : 0.5; }
}  // namespace

bool EngineController::armCountIn(const std::vector<std::string>& trackIds,
                                  int beats) {
    return armCountInImpl(trackIds, beats, false);
}

bool EngineController::armCountInExactly(
    const std::vector<std::string>& trackIds, int beats) {
    return armCountInImpl(trackIds, beats, true);
}

bool EngineController::armCountInImpl(
    const std::vector<std::string>& trackIds, int beats,
    bool requireEveryTarget) {
    if (cloudProjectBound()) return false;
    if (isRecording() || trackIds.empty()) return false;
    if (requireEveryTarget && !canStartRecordingTracksExactly(trackIds))
        return false;
    if (beats <= 0) {
        return requireEveryTarget ? startRecordingTracksExactly(trackIds)
                                  : startRecordingTracks(trackIds);
    }

    // Nothing is captured yet, so refuse now rather than after three clicks
    // have already been counted at the user.
    if (!requireEveryTarget) {
        bool recordable = false;
        for (const std::string& id : trackIds) {
            const auto* track = m_project.findTrack(id);
            if (track && isRecordable(*track)) {
                recordable = true;
                break;
            }
        }
        if (!recordable) return false;
    }

    m_countInTracks = trackIds;
    m_countInBeatsLeft = beats;
    m_countInToNextBeat = beatLength(m_project.tempo);
    m_countInRequiresExactTargets = requireEveryTarget;

    // Monitoring opens now, not when the take starts: the point of counting a
    // player in is that they play *on* the first beat, which they can only do
    // if they can hear themselves during the count. With automatic monitoring
    // switched off the count-in leaves it alone, like everything else does.
    m_countInMonitorBefore.clear();
    bool monitorChanged = false;
    if (m_recording.autoMonitorOnRecord) {
        for (const std::string& id : trackIds) {
            auto* track = m_project.findTrack(id);
            if (!track || !isRecordable(*track)) continue;
            const bool before = track->monitor;
            m_countInMonitorBefore.emplace_back(id, before);
            applySmartMonitoring(*track);
            if (track->monitor != before) monitorChanged = true;
        }
    }
    if (monitorChanged) rebuildGraph();

    // The clicks are the metronome's job and come whether it is switched on or
    // not: a count-in nobody can hear is not a count-in.
    if (m_metronome) m_metronome->requestCountIn(beats);
    return true;
}

bool EngineController::tickCountIn(double deltaSeconds) {
    if (cloudProjectBound()) {
        cancelCountIn();
        return false;
    }
    if (m_countInBeatsLeft <= 0) return false;
    m_countInToNextBeat -= std::max(0.0, deltaSeconds);
    while (m_countInToNextBeat <= 0.0 && m_countInBeatsLeft > 0) {
        --m_countInBeatsLeft;
        m_countInToNextBeat += beatLength(m_project.tempo);
    }
    if (m_countInBeatsLeft > 0) return false;

    // The last beat has been counted: the take starts on the downbeat that
    // follows it, which is now.
    const std::vector<std::string> tracks = std::move(m_countInTracks);
    const bool requireEveryTarget = m_countInRequiresExactTargets;
    m_countInTracks.clear();
    m_countInToNextBeat = 0.0;
    const bool started = requireEveryTarget
        ? startRecordingTracksExactly(tracks)
        : startRecordingTracks(tracks);
    if (!started) {
        // The exact path may fail while opening one of the WAVs after its
        // count-in already opened monitoring. Restore that local state just as
        // an explicit count-in cancellation would.
        bool monitorChanged = false;
        for (const auto& [id, monitor] : m_countInMonitorBefore) {
            auto* track = m_project.findTrack(id);
            if (!track || !track->monitorAuto || track->monitor == monitor)
                continue;
            track->monitor = monitor;
            monitorChanged = true;
        }
        m_countInMonitorBefore.clear();
        m_countInRequiresExactTargets = false;
        if (monitorChanged) rebuildGraph();
    }
    return true;
}

void EngineController::cancelCountIn() {
    if (m_countInBeatsLeft <= 0 && m_countInTracks.empty()) return;
    m_countInTracks.clear();
    m_countInBeatsLeft = 0;
    m_countInToNextBeat = 0.0;
    m_countInRequiresExactTargets = false;

    // Nothing was recorded, so the monitors go back exactly as they were —
    // except on a track the user grabbed by hand mid-count, which is theirs now.
    bool monitorChanged = false;
    for (const auto& [id, monitor] : m_countInMonitorBefore) {
        auto* track = m_project.findTrack(id);
        if (!track || !track->monitorAuto || track->monitor == monitor) continue;
        track->monitor = monitor;
        monitorChanged = true;
    }
    m_countInMonitorBefore.clear();
    if (monitorChanged) rebuildGraph();

    if (m_metronome) m_metronome->requestCountIn(0);
}

bool EngineController::isCountingIn() const { return m_countInBeatsLeft > 0; }

double EngineController::countInRemainingSeconds() const {
    if (m_countInBeatsLeft <= 0) return 0.0;
    return std::max(0.0, m_countInToNextBeat + double(m_countInBeatsLeft - 1) *
                                                   beatLength(m_project.tempo));
}

int EngineController::countInBeatsRemaining() const { return m_countInBeatsLeft; }

namespace {
/// The clip a run of recording punches into: the first audio clip on the track
/// that the run overlaps. Templated so the running preview can ask a const
/// track the same question the landing asks a mutable one — one rule, so the
/// take is drawn into the clip it will actually join.
template <typename Clips, typename LengthOf>
auto* punchTarget(Clips& clips, double runStart, double runEnd, LengthOf lengthOf) {
    using Clip = std::conditional_t<std::is_const_v<Clips>, const ClipModel, ClipModel>;
    Clip* target = nullptr;
    for (Clip& clip : clips) {
        if (clip.kind != ClipKind::Audio) continue;
        const double start = clip.startSeconds;
        const double end = start + lengthOf(clip);
        if (runStart < end && runEnd > start) {
            target = &clip;
            break;
        }
    }
    return target;
}

/// Buckets an envelope may hold before it is halved. At the default step that
/// is a little over three minutes of take before the resolution drops.
constexpr size_t kMaxEnvelopeBuckets = 8192;
}  // namespace

void EngineController::pumpRecordingEnvelopes() {
    if (m_captures.empty()) return;

    for (auto& capture : m_captures) {
        const auto* track = m_project.findTrack(capture.trackId);
        if (!track || !capture.recorder) continue;
        if (capture.seededSeconds >= 0.0) continue;   // an offline harness owns it
        // Both sides of a stereo pair, so a mono source on either one still
        // draws. This is the same meter the mixer reads, sampled per frame.
        const float peak = std::clamp(std::max(inputPeak(track->inputChannel),
                                               inputPeak(track->inputChannel + 1)),
                                      0.0f, 1.0f);

        // The recorder's own frame count, not the wall clock and not the
        // playhead: it is the clock the finished file is measured on, it never
        // wraps at the loop end, and drawing against it is what stops the shape
        // from creeping under the write head.
        const double captured =
            m_sampleRate > 0.0
                ? double(capture.recorder->recordedFrames()) / m_sampleRate
                : 0.0;

        // Bucket `i` is [i·step, (i+1)·step) of recorded time, so a sample's
        // position is a property of *when it was captured* rather than of how
        // many frames happened to be drawn before it.
        size_t wanted =
            size_t(captured / std::max(0.001, capture.envelopeStepSeconds)) + 1;
        while (wanted > kMaxEnvelopeBuckets) {
            // Halving pairs the buckets up and doubles the step, which keeps
            // every remaining bucket's start time exactly what it was.
            std::vector<float> thinned;
            thinned.reserve(capture.envelope.size() / 2 + 1);
            for (size_t i = 0; i < capture.envelope.size(); i += 2) {
                thinned.push_back(std::max(capture.envelope[i],
                                           i + 1 < capture.envelope.size()
                                               ? capture.envelope[i + 1]
                                               : 0.0f));
            }
            capture.envelope = std::move(thinned);
            capture.envelopeStepSeconds *= 2.0;
            wanted =
                size_t(captured / std::max(0.001, capture.envelopeStepSeconds)) + 1;
        }

        // Buckets the tick skipped over get this frame's peak too: the meter is
        // itself a peak over the blocks since the last read, so it describes the
        // gap as well as the instant.
        while (capture.envelope.size() < wanted) capture.envelope.push_back(peak);
        if (!capture.envelope.empty()) {
            // The newest bucket is still filling; hold its loudest sample.
            capture.envelope.back() = std::max(capture.envelope.back(), peak);
        }
    }
}

std::vector<RecordingSpan> EngineController::capturePasses(
    const FrozenRecordingSemantics& semantics, double startSeconds,
    double capturedLength) {
    std::vector<RecordingSpan> passes;
    if (capturedLength <= 0.0) return passes;

    // File time 0 is the punch point: the count-in happens before any recorder
    // exists, so nothing has to be trimmed off the front.
    std::vector<double> boundaries;
    const double loopLength =
        semantics.loopEndSeconds - semantics.loopStartSeconds;
    if (semantics.loopEnabled && loopLength > 0.0 &&
        startSeconds >= semantics.loopStartSeconds &&
        startSeconds < semantics.loopEndSeconds) {
        double cursor = semantics.loopEndSeconds - startSeconds;
        while (cursor < capturedLength) {
            boundaries.push_back(cursor);
            cursor += loopLength;
        }
    }
    if (boundaries.empty()) {
        passes.push_back({startSeconds, startSeconds + capturedLength, 0.0});
    } else {
        double from = 0.0;
        double at = startSeconds;
        for (const double boundary : boundaries) {
            passes.push_back({at, at + (boundary - from), from});
            from = boundary;
            at = semantics.loopStartSeconds;   // every pass after the first
        }
        // The run is still inside (or was stopped in) the last pass.
        if (capturedLength - from > 0.0) {
            passes.push_back({at, at + (capturedLength - from), from});
        }
    }
    return passes;
}

void EngineController::seedRecordingForShot(
    const std::string& trackId, double seconds,
    const std::function<float(double)>& level) {
    for (auto& capture : m_captures) {
        if (capture.trackId != trackId) continue;
        capture.seededSeconds = std::max(0.0, seconds);
        capture.envelope.clear();
        const size_t buckets =
            size_t(capture.seededSeconds / capture.envelopeStepSeconds) + 1;
        capture.envelope.reserve(buckets);
        for (size_t i = 0; i < buckets; ++i) {
            const double at = double(i) * capture.envelopeStepSeconds;
            capture.envelope.push_back(
                std::clamp(level ? level(at) : 0.6f, 0.0f, 1.0f));
        }

        // This is an explicitly offline/test-only hook. Feed the recorder too,
        // so Stop exercises the same closed WAV metadata and landing path as a
        // device callback instead of manufacturing a duration that the file
        // does not actually contain.
        if (!capture.recorder || !capture.recorder->isRecording() ||
            m_sampleRate <= 0.0) {
            continue;
        }
        const TrackModel* track = m_project.findTrack(trackId);
        const std::uint32_t neededChannels = track
            ? std::max<std::uint32_t>(
                  1, track->inputChannel +
                         std::clamp(track->inputChannelCount, 1u, 2u))
            : 1;
        const std::uint64_t targetFrames = std::uint64_t(
            std::llround(capture.seededSeconds * m_sampleRate));
        constexpr audio::BufferSize kSeedBlock = 512;
        audio::AudioBuffer input(neededChannels, kSeedBlock);
        int stalledWrites = 0;
        while (std::uint64_t(capture.recorder->recordedFrames()) <
                   targetFrames &&
               stalledWrites < 5000) {
            const std::uint64_t before =
                std::uint64_t(capture.recorder->recordedFrames());
            const audio::BufferSize frames = audio::BufferSize(
                std::min<std::uint64_t>(kSeedBlock, targetFrames - before));
            for (std::uint32_t channel = 0; channel < neededChannels;
                 ++channel) {
                float* destination = input.getChannel(channel);
                for (audio::BufferSize frame = 0; frame < frames; ++frame) {
                    const double at = double(before + frame) / m_sampleRate;
                    destination[frame] = std::clamp(
                        level ? level(at) : 0.6f, 0.0f, 1.0f);
                }
            }
            capture.recorder->process(input, frames);
            if (std::uint64_t(capture.recorder->recordedFrames()) == before) {
                ++stalledWrites;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                stalledWrites = 0;
            }
        }
    }
}

RecordingPreview EngineController::recordingPreview(
    const std::string& trackId) {
    RecordingPreview preview;
    const Capture* capture = nullptr;
    for (const auto& c : m_captures) {
        if (c.trackId == trackId) capture = &c;
    }
    if (!capture || !capture->recorder) return preview;
    TrackModel* track = m_project.findTrack(trackId);
    if (!track) return preview;

    preview.active = true;
    preview.trackId = trackId;
    preview.envelope = capture->envelope;
    preview.envelopeStepSeconds = capture->envelopeStepSeconds;
    preview.capturedSeconds =
        capture->seededSeconds >= 0.0 ? capture->seededSeconds
        : m_sampleRate > 0.0
            ? double(capture->recorder->recordedFrames()) / m_sampleRate
            : 0.0;
    preview.color = track->color;
    preview.name =
        platform::pathToUtf8(platform::pathFromUtf8(capture->path).stem());

    preview.spans = capturePasses(capture->semantics, capture->startSeconds,
                                  preview.capturedSeconds);
    // Nothing yet: still show where the take begins, so the punch point is
    // visible from the instant record is pressed.
    if (preview.spans.empty()) {
        preview.spans.push_back({capture->startSeconds, capture->startSeconds, 0.0});
    }
    // With loop takes off the passes overwrite each other, so only the newest
    // one will survive — and only the newest one is drawn.
    if (!capture->semantics.loopCreatesTakes && preview.spans.size() > 1) {
        preview.spans.erase(preview.spans.begin(), preview.spans.end() - 1);
    }

    // The write head belongs against the playhead. The capture clock decides
    // where the *waveform* goes — it is the file's own time, and drawing the
    // shape against anything else is what made it creep — but the recorder is
    // a block or so behind the transport, and an edge that trails the cursor by
    // a visible sliver reads as the take falling behind. So the newest pass is
    // stretched to the cursor. It also keeps the take visible on a device with
    // no input at all, where nothing is being captured to measure.
    RecordingSpan& head = preview.spans.back();
    const double position = m_engine.transport().presentationPositionSeconds();
    if (position > head.endSeconds && position >= head.startSeconds) {
        head.endSeconds = position;
    }

    if (capture->semantics.mode != RecordMode::Layers) return preview;

    // Layer recording: a punch-in lands *inside* the clip it was recorded over,
    // as the next take in that clip's stack — so it is drawn in that take's
    // colour, under that take's name, rather than as a clip of its own.
    double runStart = preview.spans.front().startSeconds;
    double runEnd = runStart;
    for (const RecordingSpan& span : preview.spans) {
        runStart = std::min(runStart, span.startSeconds);
        runEnd = std::max(runEnd, span.endSeconds);
    }
    const ClipModel* target =
        punchTarget(std::as_const(track->clips), runStart, runEnd,
                    [this](const ClipModel& c) { return effectiveClipLength(c); });
    if (!target) return preview;

    preview.layered = true;
    preview.targetClipId = target->id;
    // Landing promotes a plain clip's own material to Take 1 first, so the take
    // being recorded is number 2 even though the stack is still empty.
    const size_t index = target->takes.empty() ? 1 : target->takes.size();
    preview.takeIndex = int(index);
    preview.color = takeColor(track->color, index);
    preview.name = "Take " + std::to_string(index + 1);
    return preview;
}

void EngineController::applySmartMonitoring(TrackModel& track) {
    // Two tracks listening to the same physical input means hearing the source
    // twice — comb filtering, and CPU spent to produce it. So the monitor only
    // opens when nothing else is already carrying this input.
    bool alreadyMonitored = false;
    for (const auto& other : m_project.tracks) {
        if (other.id == track.id) continue;
        if (!carriesAudio(other)) continue;
        if (other.inputChannel != track.inputChannel) continue;
        if (other.monitor) {
            alreadyMonitored = true;
            break;
        }
    }

    const bool wanted = !alreadyMonitored;
    if (track.monitor == wanted) {
        // Nothing to change, but the track is still under automatic control —
        // that is what the "A" mark next to the button reports.
        track.monitorAuto = true;
        return;
    }
    track.monitor = wanted;
    track.monitorAuto = true;
}

bool EngineController::isRecording() const {
    for (const auto& capture : m_captures) {
        if (capture.recorder && capture.recorder->isRecording()) return true;
    }
    return false;
}

void EngineController::clearTrackRange(TrackModel& track, double from, double to) {
    if (to <= from) return;
    std::vector<ClipModel> kept;
    kept.reserve(track.clips.size() + 2);

    for (const ClipModel& clip : track.clips) {
        const double clipStart = clip.startSeconds;
        const double clipEnd = clipStart + effectiveClipLength(const_cast<ClipModel&>(clip));
        if (clipEnd <= from || clipStart >= to) {
            kept.push_back(clip);          // untouched
            continue;
        }
        if (clipStart >= from && clipEnd <= to) continue;   // fully covered

        // A head and/or a tail survives. Both are non-destructive trims of the
        // same source, so the piece outside the range keeps playing exactly
        // what it played before.
        if (clipStart < from) {
            ClipModel head = clip;
            head.durationSeconds = from - clipStart;
            head.fadeOutSeconds = std::min(head.fadeOutSeconds, head.durationSeconds);
            if (isLayered(head)) normalizeComp(head);
            kept.push_back(std::move(head));
        }
        if (clipEnd > to) {
            ClipModel tail = clip;
            tail.id = newUuid();
            const double shift = to - clipStart;
            tail.startSeconds = to;
            tail.offsetSeconds = clip.offsetSeconds + shift;
            tail.durationSeconds = clipEnd - to;
            tail.fadeInSeconds = std::min(tail.fadeInSeconds, tail.durationSeconds);
            // A layered clip's takes and comp are clip-relative, so cutting the
            // head off means sliding the whole comp back by the same amount.
            for (auto& take : tail.takes) take.clipOffsetSeconds -= shift;
            for (auto& segment : tail.comp) {
                segment.startSeconds -= shift;
                segment.endSeconds -= shift;
            }
            if (isLayered(tail)) normalizeComp(tail);
            kept.push_back(std::move(tail));
        }
    }
    track.clips = std::move(kept);
}

void EngineController::landCapture(
    TrackModel& track, const FinalizedRecordingTrack& recording) {
    if (recording.passes.empty()) return;

    const std::vector<RecordingSpan>& passes = recording.passes;
    const RecordMode mode = recording.semantics.mode;
    const std::string& path = recording.closedWavPath;
    const int channels = int(recording.channels);

    const std::string fileName =
        platform::pathToUtf8(platform::pathFromUtf8(path).stem());
    m_waveforms.peaks(path);

    if (mode == RecordMode::Overwrite) {
        // Overwrite is flat: each pass replaces whatever occupied its stretch of
        // timeline, so a later pass erases the earlier one exactly as it erases
        // material that was already there.
        for (const RecordingSpan& pass : passes) {
            const double length = pass.endSeconds - pass.startSeconds;
            if (length <= 0.0) continue;
            clearTrackRange(track, pass.startSeconds, pass.endSeconds);

            ClipModel clip;
            clip.id = newUuid();
            clip.name = fileName;
            clip.filePath = path;
            clip.startSeconds = pass.startSeconds;
            clip.durationSeconds = length;
            clip.offsetSeconds = pass.captureOffsetSeconds;
            clip.channels = channels;
            clip.color = track.color;
            track.clips.push_back(std::move(clip));
        }
        return;
    }

    // ── Layer recording ──
    //
    // Everything from this run belongs to one container clip, so five loop
    // passes are five takes inside one clip rather than five clips.
    double runStart = passes.front().startSeconds;
    double runEnd = runStart;
    for (const RecordingSpan& pass : passes) {
        runStart = std::min(runStart, pass.startSeconds);
        runEnd = std::max(runEnd, pass.endSeconds);
    }

    // A punch-in lands in the clip it was recorded over, which is what keeps the
    // earlier performance instead of replacing it.
    ClipModel* target =
        punchTarget(track.clips, runStart, runEnd,
                    [this](const ClipModel& c) { return effectiveClipLength(c); });

    if (!target) {
        ClipModel clip;
        clip.id = newUuid();
        clip.name = fileName;
        clip.startSeconds = runStart;
        clip.durationSeconds = runEnd - runStart;
        clip.channels = channels;
        clip.color = track.color;
        track.clips.push_back(std::move(clip));
        target = &track.clips.back();
    } else {
        // The material already in the clip becomes Take 1, so nothing that was
        // audible before the punch-in is lost.
        promoteToTake(*target);
        const double end = target->startSeconds + effectiveClipLength(*target);
        if (runEnd > end) target->durationSeconds = runEnd - target->startSeconds;
        if (runStart < target->startSeconds) {
            // Recording began before the clip did: grow the head, sliding the
            // existing takes and comp forward by the same amount.
            const double shift = target->startSeconds - runStart;
            target->startSeconds = runStart;
            target->durationSeconds += shift;
            target->offsetSeconds = std::max(0.0, target->offsetSeconds - shift);
            for (auto& take : target->takes) take.clipOffsetSeconds += shift;
            for (auto& segment : target->comp) {
                segment.startSeconds += shift;
                segment.endSeconds += shift;
            }
        }
    }

    // The preference is frozen when recording starts. Store it on the shared
    // container so later preference changes and other clients cannot alter the
    // sound of already-landed comp seams.
    target->compCrossfadeMs =
        std::clamp(recording.semantics.compCrossfadeMs, 0.0, 20.0);

    for (const RecordingSpan& pass : passes) {
        const double length = pass.endSeconds - pass.startSeconds;
        if (length <= 0.0) continue;

        TakeModel take;
        take.id = newUuid();
        take.name = "Take " + std::to_string(target->takes.size() + 1);
        take.filePath = path;
        take.offsetSeconds = pass.captureOffsetSeconds;
        take.lengthSeconds = length;
        take.clipOffsetSeconds = pass.startSeconds - target->startSeconds;
        take.channels = channels;
        take.color = takeColor(track.color, target->takes.size());
        const std::string takeId = take.id;
        target->takes.push_back(std::move(take));

        // The newest take is the one you want to hear — over its own region when
        // takes are trimmed to what was recorded, over the whole clip otherwise
        // (the renderer clamps to available material, so the rest is silence).
        if (recording.semantics.trimTakesToRegion) {
            const double from = pass.startSeconds - target->startSeconds;
            setCompRange(*target, takeId, from, from + length);
        } else {
            selectWholeTake(*target, takeId);
        }
    }

    if (recording.semantics.autoExpandAfterRecord) target->expanded = true;
}

EngineController::FinalizedRecordingRun
EngineController::finalizeRecordingCapture() {
    FinalizedRecordingRun run;
    if (m_captures.empty()) return run;

    // Take the transport out of record but leave it rolling, so punching out
    // does not also stop playback.
    if (m_engine.transport().isRecording()) m_engine.transport().play();

    std::vector<Capture> captures = std::move(m_captures);
    m_captures.clear();
    m_recordingTracks.clear();
    publishRecorders();          // the audio thread stops tapping them here
    run.tracks.reserve(captures.size());

    for (Capture& capture : captures) {
        FinalizedRecordingTrack recording;
        recording.trackId = capture.trackId;
        recording.closedWavPath = capture.path;
        recording.startSeconds = capture.startSeconds;
        recording.semantics = capture.semantics;

        if (capture.recorder) {
            if (capture.recorder->isRecording())
                capture.recorder->stopRecording();
            // `session().filePath` survives stop: only its transient state flag
            // returns to Idle. It is the authority for the name minted by the
            // recorder, while `capture.path` remains a defensive fallback.
            const audio::RecordingSession session = capture.recorder->session();
            if (!session.filePath.empty())
                recording.closedWavPath = session.filePath;
            recording.sampleRate = session.sampleRate;
            recording.channels = session.channelCount;
            recording.capturedFrames = session.capturedFrames > 0
                ? std::uint64_t(session.capturedFrames)
                : 0;
            recording.writtenFrames = session.writtenFrames > 0
                ? std::uint64_t(session.writtenFrames)
                : 0;
            recording.droppedFrames = session.droppedFrames;
            recording.fileWriteSucceeded = session.fileWriteSucceeded;
            recording.frames = recording.writtenFrames;
        }

        TrackModel* track = m_project.findTrack(capture.trackId);
        if (track) {
            // Monitoring/arm/input are local session state even though the
            // legacy runtime stores them on TrackModel. Restore only what this
            // capture owned, using the policy frozen when it began.
            if (capture.monitorManaged) {
                switch (capture.semantics.monitorStopPolicy) {
                    case MonitorStopPolicy::KeepOn: break;
                    case MonitorStopPolicy::ReturnToPrevious:
                        if (track->monitorAuto)
                            track->monitor = capture.monitorBefore;
                        break;
                    case MonitorStopPolicy::AutoDisable:
                        if (track->monitorAuto) track->monitor = false;
                        break;
                }
            }
            track->armed = capture.armedBefore;
        }

        // A cloud Stop needs metadata, not a decoded copy of the whole take.
        // Loading here retained every recovery-only WAV in the project sample
        // cache and could exhaust memory before its recovery sidecar was written.
        // The shared probe opens only the container header and also gives this
        // path a non-throwing readability check. A failed final flush/header
        // does not erase either the raw path or the writer-confirmed prefix:
        // when the prefix still probes, it remains available for recovery.
        if (!recording.closedWavPath.empty()) {
            audio::platform::AudioFileInfo info;
            if (audio::platform::probeAudioFile(recording.closedWavPath, info) &&
                info.sampleRate > 0.0 && info.frames > 0 && info.channels > 0) {
                recording.audioReadable = true;
                recording.frames = std::uint64_t(info.frames);
                recording.sampleRate = info.sampleRate;
                recording.channels = std::uint32_t(info.channels);
                recording.durationSeconds = info.durationSeconds();
            }
        }

        recording.passes = capturePasses(
            recording.semantics, recording.startSeconds,
            recording.durationSeconds);
        // Stopping the instant it started leaves no document material, but the
        // closed WAV still belongs in the finalized run for crash recovery.
        std::erase_if(recording.passes, [](const RecordingSpan& pass) {
            return pass.endSeconds - pass.startSeconds <= 0.01;
        });
        if (!recording.semantics.loopCreatesTakes &&
            recording.passes.size() > 1) {
            recording.passes.erase(recording.passes.begin(),
                                   recording.passes.end() - 1);
        }
        run.tracks.push_back(std::move(recording));
    }

    // Recorder taps have gone and automatic arm/monitor state was restored.
    // This rebuild is purely a local engine projection; the shared document is
    // byte-for-byte untouched by capture finalization.
    rebuildGraph();
    return run;
}

std::string EngineController::stopRecording() {
    FinalizedRecordingRun run = finalizeRecordingCapture();
    if (run.empty()) return {};

    struct Landing {
        std::string trackId;
        std::vector<ClipModel> before;
        std::vector<ClipModel> after;
    };
    std::vector<Landing> landings;
    std::string firstPath;

    for (const FinalizedRecordingTrack& recording : run.tracks) {
        TrackModel* track = m_project.findTrack(recording.trackId);
        if (!track || !recording.audioReadable) continue;

        Landing landing;
        landing.trackId = recording.trackId;
        landing.before = track->clips;
        landCapture(*track, recording);
        landing.after = track->clips;
        landings.push_back(std::move(landing));

        if (firstPath.empty()) firstPath = recording.closedWavPath;
    }

    // Snapshot-and-restore rather than inverse operations: one recording can
    // create a clip, trim two others and add five takes, and undoing that as a
    // single step is exactly what the user means by "undo the recording".
    auto restore = [this](const std::vector<Landing>& to,
                          bool useAfter) {
        for (const Landing& landing : to) {
            if (auto* t = m_project.findTrack(landing.trackId)) {
                t->clips = useAfter ? landing.after : landing.before;
                syncTrackClips(*t);
            }
        }
        updateTimelineDuration();
    };
    if (!landings.empty()) {
        m_undo.push("Record",
                    [restore, landings] { restore(landings, false); },
                    [restore, landings] { restore(landings, true); });
    }

    rebuildGraph();
    return firstPath;
}

// ── Takes and comping ──────────────────────────────────────────────────────

ClipModel* EngineController::findClip(const std::string& trackId,
                                      const std::string& clipId) {
    auto* track = m_project.findTrack(trackId);
    if (!track) return nullptr;
    for (auto& clip : track->clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

void EngineController::syncClipOwner(const std::string& trackId) {
    if (auto* track = m_project.findTrack(trackId)) syncTrackClips(*track);
    updateTimelineDuration();
}

void EngineController::setClipExpanded(const std::string& trackId,
                                       const std::string& clipId, bool expanded) {
    // View state, so it is persisted but never goes on the undo stack: undo is
    // for the music, not for which panels are open.
    if (auto* clip = findClip(trackId, clipId)) clip->expanded = expanded;
}

void EngineController::beginCompEdit(const std::string& trackId,
                                     const std::string& clipId) {
    if (m_compEdit.active) return;
    const ClipModel* clip = findClip(trackId, clipId);
    if (!clip) return;
    m_compEdit = {true, trackId, clipId, clip->comp, clip->comp};
}

void EngineController::endCompEdit() {
    if (!m_compEdit.active) return;
    CompEdit edit = std::move(m_compEdit);
    m_compEdit = {};

    const ClipModel* clip = findClip(edit.trackId, edit.clipId);
    if (!clip) return;
    std::vector<CompSegment> after =
        cloudProjectBound() ? std::move(edit.after) : clip->comp;
    if (after.size() == edit.before.size()) {
        bool same = true;
        for (size_t i = 0; i < after.size() && same; ++i) {
            same = after[i].takeId == edit.before[i].takeId &&
                   after[i].startSeconds == edit.before[i].startSeconds &&
                   after[i].endSeconds == edit.before[i].endSeconds;
        }
        if (same) return;       // the stroke changed nothing
    }

    const std::string trackId = edit.trackId;
    const std::string clipId = edit.clipId;
    auto apply = [this, trackId, clipId](const std::vector<CompSegment>& comp) {
        if (auto* c = findClip(trackId, clipId)) {
            c->comp = comp;
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCompDiff(batch, trackId, clipId, edit.before, after);
        if (batch->commands.empty() ||
            !sharedBatchApplies(m_project, batch)) return;
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   "Comp");
        return;
    }
    m_undo.push("Comp", [apply, before = std::move(edit.before)] { apply(before); },
                [apply, after = std::move(after)] { apply(after); });
}

void EngineController::setCompSegment(const std::string& trackId,
                                      const std::string& clipId,
                                      const std::string& takeId,
                                      double fromSeconds, double toSeconds) {
    auto* clip = findClip(trackId, clipId);
    if (!clip) return;
    if (cloudProjectBound()) {
        ClipModel draft = *clip;
        const bool inGesture = m_compEdit.active &&
                               m_compEdit.trackId == trackId &&
                               m_compEdit.clipId == clipId;
        if (inGesture) draft.comp = m_compEdit.after;
        const std::vector<CompSegment> before = draft.comp;
        setCompRange(draft, takeId, fromSeconds, toSeconds);
        if (inGesture) {
            m_compEdit.after = std::move(draft.comp);
            return;
        }
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCompDiff(batch, trackId, clipId, before, draft.comp);
        if (!batch->commands.empty() && sharedBatchApplies(m_project, batch)) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       "Comp");
        }
        return;
    }
    setCompRange(*clip, takeId, fromSeconds, toSeconds);
    syncClipOwner(trackId);
}

void EngineController::selectTake(const std::string& trackId,
                                  const std::string& clipId,
                                  const std::string& takeId) {
    auto* clip = findClip(trackId, clipId);
    if (!clip || !findTake(*clip, takeId)) return;
    std::vector<CompSegment> before = clip->comp;
    if (cloudProjectBound()) {
        ClipModel draft = *clip;
        selectWholeTake(draft, takeId);
        auto batch = std::make_shared<collab::BatchCommand>();
        appendCompDiff(batch, trackId, clipId, before, draft.comp);
        if (!batch->commands.empty() && sharedBatchApplies(m_project, batch)) {
            (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                       "Select Take");
        }
        return;
    }
    selectWholeTake(*clip, takeId);
    std::vector<CompSegment> after = clip->comp;
    syncClipOwner(trackId);

    auto apply = [this, trackId, clipId](const std::vector<CompSegment>& comp) {
        if (auto* c = findClip(trackId, clipId)) {
            c->comp = comp;
            syncClipOwner(trackId);
        }
    };
    m_undo.push("Select Take", [apply, before = std::move(before)] { apply(before); },
                [apply, after = std::move(after)] { apply(after); });
}

namespace {

/// True when two takes are the same take, field for field. Used to decide
/// whether a take edit is worth an undo entry.
bool sameTake(const TakeModel& a, const TakeModel& b) {
    return a.name == b.name && a.color == b.color && a.muted == b.muted &&
           a.gain == b.gain;
}

bool matchesTakeCompBaseline(const ClipModel* current,
                             const std::vector<TakeModel>& takes,
                             const std::vector<CompSegment>& comp) {
    if (!current || current->takes.size() != takes.size() ||
        current->comp.size() != comp.size()) {
        return false;
    }
    for (std::size_t index = 0; index < takes.size(); ++index) {
        if (current->takes[index].id != takes[index].id) return false;
    }
    for (std::size_t index = 0; index < comp.size(); ++index) {
        const CompSegment& actual = current->comp[index];
        const CompSegment& expected = comp[index];
        if (actual.id != expected.id || actual.takeId != expected.takeId ||
            actual.startSeconds != expected.startSeconds ||
            actual.endSeconds != expected.endSeconds) {
            return false;
        }
    }
    return true;
}

void appendCompDiff(const std::shared_ptr<collab::BatchCommand>& batch,
                    const std::string& trackId, const std::string& clipId,
                    const std::vector<CompSegment>& before,
                    const std::vector<CompSegment>& after) {
    std::unordered_set<std::string> afterIds;
    for (const CompSegment& segment : after) afterIds.insert(segment.id);
    std::unordered_map<std::string, const CompSegment*> beforeById;
    for (const CompSegment& segment : before)
        beforeById.emplace(segment.id, &segment);
    for (const CompSegment& segment : before) {
        if (!afterIds.contains(segment.id))
            appendCommand(batch, collab::DeleteCompSegment{
                trackId, clipId, segment.id});
    }

    // A swipe can insert a new segment before a retained right-hand fragment.
    // Shrink/move retained fragments first, using their current surviving
    // predecessor, so no intermediate child overlaps the old geometry. The
    // final pass below then inserts new entities and establishes final order.
    std::string survivingAnchor;
    for (const CompSegment& current : before) {
        if (!afterIds.contains(current.id)) continue;
        const auto replacement = std::find_if(
            after.begin(), after.end(), [&](const CompSegment& segment) {
                return segment.id == current.id;
            });
        if (replacement != after.end() &&
            (replacement->takeId != current.takeId ||
             replacement->startSeconds != current.startSeconds ||
             replacement->endSeconds != current.endSeconds)) {
            appendCommand(batch, collab::UpsertCompSegment{
                trackId, clipId, *replacement, survivingAnchor});
        }
        survivingAnchor = current.id;
    }

    for (std::size_t index = 0; index < after.size(); ++index) {
        const CompSegment& segment = after[index];
        const auto old = beforeById.find(segment.id);
        const bool existing = old != beforeById.end();
        if (existing) {
            const std::string finalAnchor =
                index == 0 ? std::string() : after[index - 1].id;
            const auto currentIndex = std::find_if(
                before.begin(), before.end(), [&](const CompSegment& value) {
                    return value.id == segment.id;
                });
            std::string oldAnchor;
            if (currentIndex != before.end()) {
                for (auto at = before.begin(); at != currentIndex; ++at) {
                    if (afterIds.contains(at->id)) oldAnchor = at->id;
                }
            }
            const bool changed = old->second->takeId != segment.takeId ||
                                 old->second->startSeconds !=
                                     segment.startSeconds ||
                                 old->second->endSeconds != segment.endSeconds;
            if (!changed && oldAnchor == finalAnchor) continue;
        }
        appendCommand(batch, collab::UpsertCompSegment{
            trackId, clipId, segment,
            index == 0 ? std::string() : after[index - 1].id});
    }
}

/// Position of a take in its clip, or npos.
size_t takeIndex(const ClipModel& clip, const std::string& takeId) {
    for (size_t i = 0; i < clip.takes.size(); ++i) {
        if (clip.takes[i].id == takeId) return i;
    }
    return std::string::npos;
}

} // namespace

/// The one place a take property edit goes through: snapshot, mutate, push.
/// Every setter below is a discrete click, so each is its own undo entry.
template <typename Mutate>
void EngineController::editTake(const char* label, const std::string& trackId,
                                const std::string& clipId,
                                const std::string& takeId, Mutate mutate) {
    auto* clip = findClip(trackId, clipId);
    if (!clip) return;
    TakeModel* take = findTake(*clip, takeId);
    if (!take) return;

    const TakeModel before = *take;
    TakeModel after = before;
    mutate(after);
    if (sameTake(before, after)) return;
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        if (before.name != after.name)
            appendCommand(batch, collab::SetTakeProperty{
                trackId, clipId, takeId, collab::TakeProperty::Name,
                after.name});
        if (before.color != after.color)
            appendCommand(batch, collab::SetTakeProperty{
                trackId, clipId, takeId, collab::TakeProperty::Color,
                std::int64_t(after.color)});
        if (before.muted != after.muted)
            appendCommand(batch, collab::SetTakeProperty{
                trackId, clipId, takeId, collab::TakeProperty::Muted,
                after.muted});
        if (before.gain != after.gain)
            appendCommand(batch, collab::SetTakeProperty{
                trackId, clipId, takeId, collab::TakeProperty::Gain,
                double(after.gain)});
        (void)submitSharedMutation(collab::CommandBody{std::move(batch)},
                                   label);
        return;
    }
    *take = after;
    syncClipOwner(trackId);

    auto apply = [this, trackId, clipId, takeId](const TakeModel& state) {
        auto* c = findClip(trackId, clipId);
        if (!c) return;
        if (TakeModel* t = findTake(*c, takeId)) {
            *t = state;
            syncClipOwner(trackId);
        }
    };
    m_undo.push(label, [apply, before] { apply(before); },
                [apply, after] { apply(after); });
}

void EngineController::setTakeName(const std::string& trackId,
                                   const std::string& clipId,
                                   const std::string& takeId,
                                   const std::string& name) {
    editTake("Rename Take", trackId, clipId, takeId,
             [&](TakeModel& take) { take.name = name; });
}

void EngineController::setTakeColor(const std::string& trackId,
                                    const std::string& clipId,
                                    const std::string& takeId, uint32_t color) {
    editTake("Recolor Take", trackId, clipId, takeId,
             [&](TakeModel& take) { take.color = color & 0xFFFFFFu; });
}

void EngineController::setTakeMuted(const std::string& trackId,
                                    const std::string& clipId,
                                    const std::string& takeId, bool muted) {
    editTake("Mute Take", trackId, clipId, takeId,
             [&](TakeModel& take) { take.muted = muted; });
}

void EngineController::setTakeGain(const std::string& trackId,
                                   const std::string& clipId,
                                   const std::string& takeId, float gain) {
    editTake("Take Gain", trackId, clipId, takeId,
             [&](TakeModel& take) { take.gain = std::clamp(gain, 0.0f, 4.0f); });
}

void EngineController::setSoloTake(const std::string& trackId,
                                   const std::string& clipId,
                                   const std::string& takeId) {
    if (m_soloTakeId == takeId && m_soloClipId == clipId) return;
    const std::string previousTrack = m_soloTakeId.empty() ? std::string{} : trackId;
    m_soloTakeId = takeId;
    m_soloClipId = takeId.empty() ? std::string{} : clipId;
    // Re-emit the clip so the graph plays the soloed take instead of the comp.
    // The engine swaps in the new clip list atomically, so this is safe mid-play.
    syncClipOwner(trackId);
    if (!previousTrack.empty() && previousTrack != trackId) syncClipOwner(previousTrack);
}

std::string EngineController::duplicateTake(const std::string& trackId,
                                            const std::string& clipId,
                                            const std::string& takeId) {
    auto* clip = findClip(trackId, clipId);
    if (!clip) return {};
    const size_t source = takeIndex(*clip, takeId);
    if (source == std::string::npos) return {};

    TakeModel copy = clip->takes[source];
    copy.id = newUuid();
    copy.name = clip->takes[source].name + " copy";
    const size_t at = source + 1;
    const std::string newId = copy.id;
    const TakeModel state = copy;
    if (cloudProjectBound()) {
        copy.filePath.clear();
        copy.notes.clear();
        if (copy.asset.empty()) return {};
        const auto result = submitSharedMutation(
            collab::AddTake{
                trackId, clipId, copy,
                source < clip->takes.size() ? clip->takes[source].id
                                            : std::string()},
            "Duplicate Take");
        return result == collab::SharedMutationResult::Submitted ? newId
                                                                 : std::string{};
    }
    clip->takes.insert(clip->takes.begin() + long(at), std::move(copy));
    syncClipOwner(trackId);

    auto remove = [this, trackId, clipId, newId] {
        if (auto* c = findClip(trackId, clipId)) {
            std::erase_if(c->takes, [&](const TakeModel& t) { return t.id == newId; });
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    auto insert = [this, trackId, clipId, at, state] {
        if (auto* c = findClip(trackId, clipId)) {
            c->takes.insert(c->takes.begin() + long(std::min(at, c->takes.size())),
                            state);
            syncClipOwner(trackId);
        }
    };
    m_undo.push("Duplicate Take", remove, insert);
    return newId;
}

std::string EngineController::addTakeFromFile(const std::string& trackId,
                                              const std::string& clipId,
                                              const std::string& filePath,
                                              double clipOffsetSeconds,
    const std::string& name) {
    auto* clip = findClip(trackId, clipId);
    if (!clip) return {};

    if (cloudProjectBound()) {
        if (clip->kind != ClipKind::Audio) return {};
        auto request = sharedAudioRequest(filePath);
        if (!request) return {};

        const std::vector<TakeModel> takesBefore = clip->takes;
        const std::vector<CompSegment> compBefore = clip->comp;
        ClipModel draft = *clip;
        if (draft.takes.empty()) {
            if (draft.asset.assetId.empty() ||
                !completeSharedAudioAsset(draft.asset, draft.asset)) {
                return {};
            }
            TakeModel base;
            base.id = newUuid();
            base.name = "Take 1";
            base.offsetSeconds = draft.offsetSeconds;
            base.lengthSeconds = draft.durationSeconds;
            base.channels = draft.channels;
            base.color = draft.color;
            base.asset = draft.asset;
            draft.takes.push_back(std::move(base));
            selectWholeTake(draft, draft.takes.front().id);
        }

        TakeModel take;
        take.id = newUuid();
        take.name = name.empty()
            ? "Take " + std::to_string(draft.takes.size() + 1)
            : name;
        take.lengthSeconds = double(request->frames) / request->sampleRate;
        take.clipOffsetSeconds = std::max(0.0, clipOffsetSeconds);
        take.channels = int(request->channels);
        take.color = takeColor(draft.color, draft.takes.size());
        take.asset = expectedSharedAudioAsset(*request);
        const std::string takeId = take.id;
        const double from = take.clipOffsetSeconds;
        const double to = from + take.lengthSeconds;
        draft.takes.push_back(std::move(take));
        setCompRange(draft, takeId, from, to);
        std::vector<TakeModel> takesAfter = std::move(draft.takes);
        const std::vector<CompSegment> compAfter = std::move(draft.comp);

        PendingSharedAssetMutation pending;
        pending.expected = expectedSharedAudioAsset(*request);
        pending.complete =
            [trackId, clipId, takesBefore,
             takesAfter = std::move(takesAfter), compBefore,
             compAfter](EngineController& controller,
                        const AssetRef& verifiedAsset) mutable {
                if (!matchesTakeCompBaseline(
                        controller.findClip(trackId, clipId), takesBefore,
                        compBefore)) {
                    return collab::SharedMutationResult::Blocked;
                }
                auto readyTake = std::find_if(
                    takesAfter.begin(), takesAfter.end(),
                    [&](const TakeModel& candidate) {
                        return candidate.asset.assetId ==
                               verifiedAsset.assetId;
                    });
                if (readyTake == takesAfter.end())
                    return collab::SharedMutationResult::Blocked;
                readyTake->asset = verifiedAsset;

                auto batch = std::make_shared<collab::BatchCommand>();
                std::unordered_set<std::string> existing;
                for (const TakeModel& before : takesBefore)
                    existing.insert(before.id);
                std::string anchor = takesBefore.empty()
                    ? std::string()
                    : takesBefore.back().id;
                for (const TakeModel& candidate : takesAfter) {
                    if (existing.contains(candidate.id)) continue;
                    TakeModel added = candidate;
                    added.filePath.clear();
                    appendCommand(batch, collab::AddTake{
                        trackId, clipId, std::move(added), anchor});
                    anchor = candidate.id;
                }
                appendCompDiff(batch, trackId, clipId, compBefore,
                               compAfter);
                if (!sharedBatchApplies(controller.m_project, batch))
                    return collab::SharedMutationResult::Blocked;
                return controller.submitSharedMutation(
                    collab::CommandBody{std::move(batch)}, "Add Take");
            };
        return prepareSharedAssetMutation(std::move(*request),
                                          std::move(pending)) ==
                       collab::SharedMutationResult::Submitted
                   ? takeId
                   : std::string{};
    }

    auto samples = loadSamples(filePath);
    if (!samples) return {};
    // Decoded here rather than from a paint handler, same as importAudio.
    m_waveforms.peaks(filePath);

    // Snapshotted before the promotion below, so undo takes the clip all the way
    // back to the plain one-layer shape rather than leaving Take 1 behind.
    const std::vector<TakeModel> takesBefore = clip->takes;
    const std::vector<CompSegment> compBefore = clip->comp;

    // The material already in the clip becomes Take 1 — the punch-in rule, so
    // layering never costs the performance that was there.
    promoteToTake(*clip);

    TakeModel take;
    take.id = newUuid();
    take.name = name.empty() ? "Take " + std::to_string(clip->takes.size() + 1)
                             : name;
    take.filePath = filePath;
    take.lengthSeconds = samples->sampleRate() > 0.0
                             ? double(samples->frames()) / samples->sampleRate()
                             : 0.0;
    take.clipOffsetSeconds = std::max(0.0, clipOffsetSeconds);
    take.channels = int(samples->channels());
    take.color = takeColor(clip->color, clip->takes.size());
    const std::string takeId = take.id;
    const double from = take.clipOffsetSeconds;
    const double to = from + take.lengthSeconds;

    clip->takes.push_back(std::move(take));
    // Active over what it covers: the newest layer is the one you want to hear,
    // and outside its stretch the earlier takes keep playing.
    setCompRange(*clip, takeId, from, to);
    const std::vector<TakeModel> takesAfter = clip->takes;
    const std::vector<CompSegment> compAfter = clip->comp;
    syncClipOwner(trackId);

    auto apply = [this, trackId, clipId](const std::vector<TakeModel>& takes,
                                        const std::vector<CompSegment>& comp) {
        if (auto* c = findClip(trackId, clipId)) {
            c->takes = takes;
            c->comp = comp;
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    m_undo.push("Add Take",
                [apply, takesBefore, compBefore] { apply(takesBefore, compBefore); },
                [apply, takesAfter, compAfter] { apply(takesAfter, compAfter); });
    return takeId;
}

void EngineController::removeTake(const std::string& trackId,
                                  const std::string& clipId,
                                  const std::string& takeId, bool deleteFile) {
    auto* clip = findClip(trackId, clipId);
    if (!clip) return;
    const size_t at = takeIndex(*clip, takeId);
    if (at == std::string::npos) return;
    const TakeModel take = clip->takes[at];
    if (cloudProjectBound()) {
        (void)submitSharedMutation(
            collab::DeleteTake{trackId, clipId, takeId}, "Delete Take");
        return;
    }

    // Deleting a take's file is only safe when this was the last reference to
    // it: loop recording points every pass at the same capture.
    std::string fileToDelete;
    if (deleteFile && !take.filePath.empty()) {
        size_t references = 0;
        for (const auto& track : m_project.tracks) {
            for (const auto& other : track.clips) {
                if (other.filePath == take.filePath) ++references;
                for (const auto& t : other.takes) {
                    if (t.filePath == take.filePath) ++references;
                }
            }
        }
        if (references <= 1) fileToDelete = take.filePath;
    }

    std::vector<CompSegment> comp = clip->comp;
    clip->takes.erase(clip->takes.begin() + long(at));
    normalizeComp(*clip);          // drops the segments that named it
    if (m_soloTakeId == takeId) setSoloTake(trackId, clipId, {});
    syncClipOwner(trackId);

    if (!fileToDelete.empty()) {
        std::error_code ec;
        fs::remove(fileToDelete, ec);
    }

    // Undo restores the document; a file already erased from disk is gone, and
    // the take comes back pointing at a missing source (drawn as such).
    auto undo = [this, trackId, clipId, at, take, comp] {
        if (auto* c = findClip(trackId, clipId)) {
            c->takes.insert(c->takes.begin() + long(std::min(at, c->takes.size())),
                            take);
            c->comp = comp;
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    auto redo = [this, trackId, clipId, takeId] {
        if (auto* c = findClip(trackId, clipId)) {
            std::erase_if(c->takes, [&](const TakeModel& t) { return t.id == takeId; });
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    m_undo.push("Delete Take", undo, redo);
}

void EngineController::moveTake(const std::string& trackId,
                                const std::string& clipId,
                                const std::string& takeId, size_t targetIndex) {
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->takes.empty()) return;
    size_t from = clip->takes.size();
    for (size_t i = 0; i < clip->takes.size(); ++i) {
        if (clip->takes[i].id == takeId) from = i;
    }
    if (from == clip->takes.size()) return;
    const size_t to = std::min(targetIndex, clip->takes.size() - 1);
    if (from == to) return;
    if (cloudProjectBound()) {
        std::vector<std::string> remaining;
        remaining.reserve(clip->takes.size() - 1);
        for (const TakeModel& take : clip->takes) {
            if (take.id != takeId) remaining.push_back(take.id);
        }
        const size_t insertAt = std::min(to, remaining.size());
        const std::string afterId =
            insertAt == 0 ? std::string() : remaining[insertAt - 1];
        (void)submitSharedMutation(
            collab::MoveTake{trackId, clipId, takeId, afterId}, "Move Take");
        return;
    }

    // Display order only: nothing in the comp or the graph reads the position,
    // so there is no re-sync and no undo entry worth spending.
    TakeModel take = std::move(clip->takes[from]);
    clip->takes.erase(clip->takes.begin() + long(from));
    clip->takes.insert(clip->takes.begin() + long(to), std::move(take));
}

std::string EngineController::flattenComp(const std::string& trackId,
                                          const std::string& clipId,
                                          bool recordUndo) {
    const bool shared = cloudProjectBound();
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->comp.empty()) return {};
    if (clip->kind != ClipKind::Audio) return {};   // MIDI bake is a note merge

    // Render through the very same node that plays the comp, so what is baked is
    // what was heard — crossfades, take gains and all. Anything else would be a
    // second implementation of the comp renderer, free to disagree with the
    // first.
    engine::ClipPlayerNode::ClipList list;
    const PlacementSpan span = emitClipPlacements(*clip, list);
    if (span.count == 0) return {};

    // Placements are timeline-absolute; the bake is clip-relative, so the whole
    // list shifts back to the clip's start.
    const engine::SamplePos origin = toSamples(clip->startSeconds);
    for (auto& placement : list) {
        placement.startSample =
            placement.startSample >= origin ? placement.startSample - origin : 0;
    }
    const engine::SamplePos total = span.endSample > origin ? span.endSample - origin : 0;
    if (total == 0) return {};

    engine::ClipPlayerNode player("Flatten");
    player.setClips(std::make_shared<const engine::ClipPlayerNode::ClipList>(
        std::move(list)));
    player.prepare({m_sampleRate, engine::FrameCount(m_bufferSize), 2, true});

    audio::AudioBuffer baked(2, audio::BufferSize(total));
    std::vector<float> scratch(size_t(m_bufferSize) * 2, 0.0f);
    float* channels[2] = {scratch.data(), scratch.data() + m_bufferSize};

    for (engine::SamplePos at = 0; at < total;) {
        const engine::FrameCount frames = engine::FrameCount(
            std::min<engine::SamplePos>(m_bufferSize, total - at));
        engine::AudioBlock output(channels, 2, frames);
        engine::ProcessContext context;
        context.output = output;
        context.frames = frames;
        context.timelinePosition = at;
        context.sampleRate = m_sampleRate;
        context.playing = true;
        context.offline = true;
        player.process(context);
        for (audio::ChannelCount ch = 0; ch < 2; ++ch) {
            std::copy(channels[ch], channels[ch] + frames,
                      baked.getChannel(ch) + at);
        }
        at += frames;
    }

    const std::string path = platform::pathToUtf8(
        platform::pathFromUtf8(m_recordDir) /
        ("comp-" + newUuid() + ".wav"));
    std::error_code ec;
    fs::create_directories(platform::pathFromUtf8(m_recordDir), ec);
    if (!m_recorder->writeWAVFile(path, baked, m_sampleRate)) return {};
    m_waveforms.peaks(path);

    if (shared) {
        auto request = sharedAudioRequest(path);
        if (!request) {
            std::error_code removeError;
            fs::remove(platform::pathFromUtf8(path), removeError);
            return {};
        }

        TakeModel take;
        take.id = newUuid();
        take.name = "Comp " + std::to_string(clip->takes.size() + 1);
        take.lengthSeconds = double(request->frames) / request->sampleRate;
        take.channels = int(request->channels);
        take.color = clip->color;
        take.asset = expectedSharedAudioAsset(*request);
        const std::string newId = take.id;
        const std::vector<TakeModel> takesBefore = clip->takes;
        const std::vector<CompSegment> compBefore = clip->comp;
        ClipModel draft = *clip;
        draft.takes.push_back(take);
        selectWholeTake(draft, newId);
        const std::vector<CompSegment> compAfter = std::move(draft.comp);
        const std::string takeAnchor = takesBefore.empty()
            ? std::string()
            : takesBefore.back().id;

        PendingSharedAssetMutation pending;
        pending.expected = take.asset;
        pending.cleanupPath = path;
        pending.complete =
            [trackId, clipId, take = std::move(take), takeAnchor,
             takesBefore, compBefore,
             compAfter](EngineController& controller,
                        const AssetRef& verifiedAsset) mutable {
                if (!matchesTakeCompBaseline(
                        controller.findClip(trackId, clipId), takesBefore,
                        compBefore)) {
                    return collab::SharedMutationResult::Blocked;
                }
                take.asset = verifiedAsset;
                take.filePath.clear();
                auto batch = std::make_shared<collab::BatchCommand>();
                appendCommand(batch, collab::AddTake{
                    trackId, clipId, take, takeAnchor});
                appendCompDiff(batch, trackId, clipId, compBefore,
                               compAfter);
                if (!sharedBatchApplies(controller.m_project, batch))
                    return collab::SharedMutationResult::Blocked;
                return controller.submitSharedMutation(
                    collab::CommandBody{std::move(batch)}, "Flatten Comp");
            };
        return prepareSharedAssetMutation(std::move(*request),
                                          std::move(pending)) ==
                       collab::SharedMutationResult::Submitted
                   ? newId
                   : std::string{};
    }

    TakeModel take;
    take.id = newUuid();
    take.name = "Comp " + std::to_string(clip->takes.size() + 1);
    take.filePath = path;
    take.lengthSeconds = double(total) / m_sampleRate;
    take.channels = 2;
    take.color = clip->color;
    const std::string newId = take.id;

    const std::vector<TakeModel> takesBefore = clip->takes;
    const std::vector<CompSegment> compBefore = clip->comp;
    clip->takes.push_back(std::move(take));
    // The bake becomes the active take: it *is* the comp, so what plays does not
    // change, and further comping now happens on top of a single flat layer.
    selectWholeTake(*clip, newId);
    const std::vector<TakeModel> takesAfter = clip->takes;
    const std::vector<CompSegment> compAfter = clip->comp;
    syncClipOwner(trackId);

    auto apply = [this, trackId, clipId](const std::vector<TakeModel>& takes,
                                        const std::vector<CompSegment>& comp) {
        if (auto* c = findClip(trackId, clipId)) {
            c->takes = takes;
            c->comp = comp;
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    if (recordUndo) {
        m_undo.push("Flatten Comp",
                    [apply, takesBefore, compBefore] { apply(takesBefore, compBefore); },
                    [apply, takesAfter, compAfter] { apply(takesAfter, compAfter); });
    }
    return newId;
}

void EngineController::commitComp(const std::string& trackId,
                                  const std::string& clipId) {
    // A non-trivial commit first renders an asset. Until that verified-asset
    // pipeline exists, block every cloud entry consistently before even the
    // single-whole-take shortcut can mutate the local projection.
    if (cloudProjectBound()) return;
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->takes.empty()) return;

    // Baking first means the surviving material really is the assembled comp,
    // not just whichever take happened to be under the playhead. On a comp that
    // is already one whole take there is nothing to render, so that take stays.
    std::string keepId;
    if (clip->comp.size() == 1 &&
        clip->comp.front().startSeconds <= 0.0 &&
        clip->comp.front().endSeconds >= effectiveClipLength(*clip) - 0.001) {
        keepId = clip->comp.front().takeId;
    } else {
        keepId = flattenComp(trackId, clipId, /*recordUndo=*/false);
        if (keepId.empty()) return;
        clip = findClip(trackId, clipId);
        if (!clip) return;
    }
    const TakeModel* keep = findTake(*clip, keepId);
    if (!keep) return;

    const ClipModel before = *clip;

    // Committing dissolves the container. Keeping the winning attempt as a lone
    // take would leave the clip expandable, with one layer under it and a comp
    // map describing itself — an editing state the user has just said they are
    // finished with. So the take's material becomes the clip's own and the
    // layers are gone: what plays is identical, what is left is a plain clip.
    ClipModel after = *clip;
    after.takes.clear();
    after.comp.clear();
    after.expanded = false;
    after.gain = before.gain * keep->gain;
    if (before.kind == ClipKind::Audio) {
        after.filePath = keep->filePath;
        after.offsetSeconds = keep->offsetSeconds;
        if (keep->channels > 0) after.channels = keep->channels;
    } else {
        after.notes = keep->notes;
    }
    // A take need not span the whole clip — a trimmed punch-in sits inside it —
    // and a plain clip has no way to say that, so the clip's own geometry moves
    // to where the material actually is.
    const double insetSeconds =
        std::clamp(keep->clipOffsetSeconds, 0.0, before.durationSeconds);
    double length = keep->lengthSeconds > 0.0 ? keep->lengthSeconds
                                              : before.durationSeconds - insetSeconds;
    length = std::clamp(length, 0.01, std::max(0.01, before.durationSeconds - insetSeconds));
    after.startSeconds = before.startSeconds + insetSeconds;
    after.durationSeconds = length;

    *clip = after;
    // The files of the discarded attempts are deliberately left on disk: this
    // command is destructive enough already, and "Delete Unused Takes" is the
    // explicit way to reclaim the space.
    if (m_soloClipId == clipId) setSoloTake({}, {}, {});
    syncClipOwner(trackId);

    auto apply = [this, trackId, clipId](const ClipModel& state) {
        if (auto* c = findClip(trackId, clipId)) {
            *c = state;
            normalizeComp(*c);
            syncClipOwner(trackId);
        }
    };
    m_undo.push("Commit Comp", [apply, before] { apply(before); },
                [apply, after] { apply(after); });
}

size_t EngineController::deleteUnusedTakes(bool deleteFiles) {
    if (cloudProjectBound()) {
        auto batch = std::make_shared<collab::BatchCommand>();
        std::size_t removed = 0;
        for (const TrackModel& track : m_project.tracks) {
            for (const ClipModel& clip : track.clips) {
                for (const TakeModel& take : clip.takes) {
                    const bool used = std::any_of(
                        clip.comp.begin(), clip.comp.end(),
                        [&](const CompSegment& segment) {
                            return segment.takeId == take.id;
                        });
                    if (used) continue;
                    appendCommand(batch, collab::DeleteTake{
                        track.id, clip.id, take.id});
                    ++removed;
                }
            }
        }
        if (removed == 0 || !sharedBatchApplies(m_project, batch)) return 0;
        const auto result = submitSharedMutation(
            collab::CommandBody{std::move(batch)}, "Delete Unused Takes");
        return result == collab::SharedMutationResult::Submitted ? removed : 0;
    }
    // Which files the project still needs after the sweep, so a take that shares
    // a loop capture with a kept take cannot take the file down with it.
    std::set<std::string> keptFiles;
    std::vector<std::string> candidates;
    for (const auto& track : m_project.tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.filePath.empty()) keptFiles.insert(clip.filePath);
            for (const auto& take : clip.takes) {
                const bool used = std::any_of(
                    clip.comp.begin(), clip.comp.end(),
                    [&](const CompSegment& s) { return s.takeId == take.id; });
                if (used) {
                    keptFiles.insert(take.filePath);
                } else if (!take.filePath.empty()) {
                    candidates.push_back(take.filePath);
                }
            }
        }
    }

    size_t removed = 0;
    struct Snapshot {
        std::string trackId;
        std::string clipId;
        std::vector<TakeModel> before;
        std::vector<TakeModel> after;
    };
    std::vector<Snapshot> touched;
    for (auto& track : m_project.tracks) {
        for (auto& clip : track.clips) {
            std::vector<std::string> unused;
            for (const auto& take : clip.takes) {
                const bool used = std::any_of(
                    clip.comp.begin(), clip.comp.end(),
                    [&](const CompSegment& s) { return s.takeId == take.id; });
                if (!used) unused.push_back(take.id);
            }
            if (unused.empty()) continue;
            Snapshot snapshot{track.id, clip.id, clip.takes, {}};
            for (const auto& id : unused) {
                std::erase_if(clip.takes,
                              [&](const TakeModel& t) { return t.id == id; });
                ++removed;
            }
            normalizeComp(clip);
            snapshot.after = clip.takes;
            touched.push_back(std::move(snapshot));
        }
    }
    if (removed == 0) return 0;

    if (deleteFiles) {
        for (const auto& path : candidates) {
            if (keptFiles.count(path)) continue;
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
    for (const auto& snapshot : touched) syncClipOwner(snapshot.trackId);

    // One undo entry for the whole sweep: the user ran one command. Files
    // already erased do not come back, and their takes return as missing media.
    auto apply = [this, touched](bool useAfter) {
        for (const auto& snapshot : touched) {
            if (auto* c = findClip(snapshot.trackId, snapshot.clipId)) {
                c->takes = useAfter ? snapshot.after : snapshot.before;
                normalizeComp(*c);
                syncClipOwner(snapshot.trackId);
            }
        }
    };
    m_undo.push("Delete Unused Takes", [apply] { apply(false); },
                [apply] { apply(true); });
    return removed;
}

size_t EngineController::cropToComp(const std::string& trackId,
                                    const std::string& clipId) {
    // Cropping creates replacement files; cloud mutation must wait for the
    // upload/verify planner that can attach durable AssetRefs atomically.
    if (cloudProjectBound()) return 0;
    auto* clip = findClip(trackId, clipId);
    if (!clip || clip->comp.empty()) return 0;

    // The stretch of each take the comp actually plays, in take-file seconds.
    struct Range { double from = 0.0; double to = 0.0; };
    std::map<std::string, Range> used;
    for (const CompSegment& segment : clip->comp) {
        const TakeModel* take = findTake(*clip, segment.takeId);
        if (!take) continue;
        const double from =
            take->offsetSeconds + (segment.startSeconds - take->clipOffsetSeconds);
        const double to =
            take->offsetSeconds + (segment.endSeconds - take->clipOffsetSeconds);
        auto [it, fresh] = used.try_emplace(segment.takeId, Range{from, to});
        if (!fresh) {
            it->second.from = std::min(it->second.from, from);
            it->second.to = std::max(it->second.to, to);
        }
    }

    // Crossfades read past a segment's edge, so the crop keeps a fade's worth of
    // margin — cropping flush would silence the seams it was meant to preserve.
    const double margin =
        std::clamp(clip->compCrossfadeMs, 0.0, 20.0) / 1000.0;

    size_t rewritten = 0;
    std::error_code ec;
    fs::create_directories(m_recordDir, ec);
    for (auto& take : clip->takes) {
        auto it = used.find(take.id);
        if (it == used.end() || take.filePath.empty()) continue;
        auto samples = loadSamples(take.filePath);
        if (!samples || samples->sampleRate() <= 0.0) continue;

        const double sourceLength = double(samples->frames()) / samples->sampleRate();
        const double from = std::max(0.0, it->second.from - margin);
        const double to = std::min(sourceLength, it->second.to + margin);
        if (to - from <= 0.0) continue;
        // Nothing to reclaim: the comp already plays essentially the whole file.
        if (from <= 0.001 && to >= sourceLength - 0.001) continue;

        const auto first = engine::SamplePos(from * samples->sampleRate());
        const auto count = audio::BufferSize((to - from) * samples->sampleRate());
        const auto channels = audio::ChannelCount(
            std::max<engine::ChannelCount>(1, samples->channels()));
        audio::AudioBuffer cropped(channels, count);
        for (audio::ChannelCount ch = 0; ch < channels; ++ch) {
            const float* source = samples->channel(engine::ChannelCount(ch)) + first;
            std::copy(source, source + count, cropped.getChannel(ch));
        }

        // Written beside the original rather than over it: another clip may share
        // this capture, and an in-place rewrite would cut its material away.
        const std::string path = platform::pathToUtf8(
            platform::pathFromUtf8(m_recordDir) /
            ("crop-" + newUuid() + ".wav"));
        if (!m_recorder->writeWAVFile(path, cropped, samples->sampleRate())) continue;
        m_waveforms.peaks(path);

        take.filePath = path;
        take.clipOffsetSeconds += from - take.offsetSeconds;
        take.offsetSeconds = 0.0;
        take.lengthSeconds = to - from;
        ++rewritten;
    }
    if (rewritten > 0) {
        normalizeComp(*clip);
        syncClipOwner(trackId);
    }
    return rewritten;
}

// ── Offline export ─────────────────────────────────────────────────────────

void EngineController::undo() {
    if (cloudProjectBound()) return;
    m_undo.undo();
}

void EngineController::redo() {
    if (cloudProjectBound()) return;
    m_undo.redo();
}

audio::Result EngineController::analyzeChannel(const std::string& channelId,
                                              double fromSeconds,
                                              double toSeconds,
                                              analysis::Metrics& out) {
    const bool master = channelId == kMasterChannelId;
    if (!master && !m_project.findTrack(channelId))
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "no such channel");

    const double end = toSeconds > fromSeconds ? toSeconds : durationSeconds();
    const double start = std::max(0.0, fromSeconds);
    if (end <= start)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "nothing to measure in that range");

    // Soloing is how one channel is heard on its own, and the graph already
    // knows how to do it. Restored below, and invisible to undo throughout.
    std::vector<std::pair<std::string, bool>> solos;
    if (!master) {
        UndoStack::Suspend quiet(m_undo);
        for (const TrackModel& track : m_project.tracks)
            solos.emplace_back(track.id, track.soloed);
        for (const auto& [id, _] : solos) setTrackSoloed(id, id == channelId);
    }

    const engine::SamplePos from = toSamples(start);
    const engine::SamplePos to = toSamples(end);
    flushDeferredClipSync();
    analysis::MetricsAccumulator accumulator(m_sampleRate, 2);
    auto status = m_engine.renderOffline(
        from, to, m_bufferSize,
        [&](const engine::AudioBlock& block, engine::FrameCount frames) {
            const float* channels[2] = {block.data(0), block.data(1)};
            accumulator.add(channels, frames);
            return true;
        });

    if (!master) {
        UndoStack::Suspend quiet(m_undo);
        for (const auto& [id, wasSoloed] : solos) setTrackSoloed(id, wasSoloed);
    }
    if (!status)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   std::string(engine::describe(status.error())));

    out = accumulator.result();
    return audio::Result::ok();
}

audio::Result EngineController::analyzeSampleFile(const std::string& filePath,
                                                  analysis::SampleTraits& out) {
    // Decoded here and dropped: measuring a folder of candidates must not pin
    // every one of them in memory, the same rule the audition follows.
    audio::platform::DecodedAudio decoded;
    auto result = audio::platform::decodeAudioFile(filePath, decoded);
    if (!result.isOk()) return result;
    if (decoded.frames == 0 || decoded.channels == 0)
        return audio::Result::fail(audio::EngineError::UnsupportedFormat,
                                   "the file decoded to nothing");

    // The decoder hands back interleaved samples and the analysis wants planar,
    // so this is the one copy in the path.
    const std::size_t channels = decoded.channels;
    std::vector<std::vector<float>> planar(channels,
                                           std::vector<float>(decoded.frames));
    for (std::size_t f = 0; f < decoded.frames; ++f)
        for (std::size_t ch = 0; ch < channels; ++ch)
            planar[ch][f] = decoded.interleaved[f * channels + ch];

    std::vector<const float*> pointers;
    for (const auto& channel : planar) pointers.push_back(channel.data());
    out = analysis::describe(pointers.data(), int(pointers.size()),
                             decoded.frames, decoded.sampleRate);
    return audio::Result::ok();
}

audio::Result EngineController::exportMixdown(const std::string& outputPath,
                                              bool normalize) {
    // An export is the one place where "one tick late" is not good enough.
    flushDeferredClipSync();
    flushSamplerPrecompute();
    const double duration = durationSeconds();
    if (duration <= 0.0) {
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                   "nothing to export");
    }

    const engine::SamplePos total = toSamples(duration);
    float gain = 1.0f;
    if (normalize) {
        float peak = 0.0f;
        auto peakStatus = m_engine.renderOffline(
            0, total, m_bufferSize,
            [&](const engine::AudioBlock& block, engine::FrameCount frames) {
                for (engine::ChannelCount channel = 0; channel < 2; ++channel) {
                    const float* data = block.data(channel);
                    for (engine::FrameCount frame = 0; frame < frames; ++frame) {
                        peak = std::max(peak, std::fabs(data[frame]));
                    }
                }
                return true;
            });
        if (!peakStatus) {
            return audio::Result::fail(
                audio::EngineError::InvalidArgument,
                std::string(engine::describe(peakStatus.error())));
        }
        if (peak > 0.0001f) gain = 0.99f / peak;
    }

    audio::platform::AudioFileWriter writer;
    audio::Result ioStatus = writer.open(outputPath, m_sampleRate, 2,
                                         std::uint64_t(total));
    if (!ioStatus) return ioStatus;

    // Stream each rendered block directly to disk. Memory use is now bounded
    // by one engine block regardless of project duration; normalization uses a
    // deterministic peak pass rather than retaining the whole mix.
    auto renderStatus = m_engine.renderOffline(
        0, total, m_bufferSize,
        [&](const engine::AudioBlock& block, engine::FrameCount frames) {
            if (!ioStatus) return false;
            const float* channels[2] = {block.data(0), block.data(1)};
            ioStatus = writer.write(channels, frames, gain);
            return bool(ioStatus);
        });
    const audio::Result closeStatus = writer.close();
    if (!renderStatus || !ioStatus || !closeStatus) {
        std::error_code removeError;
        fs::remove(outputPath, removeError);
        if (!renderStatus) {
            return audio::Result::fail(
                audio::EngineError::InvalidArgument,
                std::string(engine::describe(renderStatus.error())));
        }
        if (!ioStatus) return ioStatus;
        return closeStatus;
    }

    // Host parameter events queued before the render are consumed by its first
    // block. Coalesced edits can contain older values before the latest one;
    // settle that final generation before returning to the caller.
    flushSamplerPrecompute();
    return audio::Result::ok();
}

// ── Devices ────────────────────────────────────────────────────────────────

std::vector<audio::DeviceInfo> EngineController::enumerateOutputDevices() {
    return m_devices->enumerateOutputDevices();
}
std::vector<audio::DeviceInfo> EngineController::enumerateInputDevices() {
    return m_devices->enumerateInputDevices();
}
std::string EngineController::currentOutputDeviceUid() {
    return m_devices->getCurrentOutputDevice().uid;
}
std::string EngineController::currentInputDeviceUid() {
    return m_devices->getCurrentInputDevice().uid;
}
audio::DeviceInfo EngineController::currentInputDeviceInfo() const {
    return m_devices->getCurrentInputDevice();
}
audio::AudioDeviceConfig EngineController::audioConfiguration() const {
    auto config = m_devices->configuration();
    if (!m_deviceOpen) {
        config.sampleRate = m_sampleRate;
        config.bufferSize = m_bufferSize;
    }
    return config;
}

audio::Result EngineController::applyAudioConfiguration(
    const audio::AudioDeviceConfig& config) {
    if (!m_liveDeviceAllowed) {
        const bool rateChanged = std::abs(config.sampleRate - m_sampleRate) > 0.01;
        m_bufferSize = config.bufferSize;
        if (rateChanged)
            applyRenderSampleRate(config.sampleRate);
        else
            (void)m_engine.prepare(m_sampleRate, m_bufferSize, 2);
        m_project.sampleRate = m_sampleRate;
        return rebuildGraph();
    }

    // A newly opened stream may start immediately. Keep it silent until the
    // graph has been resized for its actual block size and sample rate.
    m_devices->setAudioCallback(nullptr);
    auto applied = m_devices->applyConfiguration(config);
    if (!applied) {
        m_devices->setAudioCallback(m_callback.get());
        m_deviceOpen = m_devices->isRunning();
        return applied;
    }

    const double settledRate = m_devices->sampleRate();
    const uint32_t settledBuffer = m_devices->bufferSize();
    const bool rateChanged = std::abs(settledRate - m_sampleRate) > 0.01;
    m_bufferSize = settledBuffer;
    if (rateChanged) {
        applyRenderSampleRate(settledRate);
    } else {
        (void)m_engine.prepare(m_sampleRate, m_bufferSize, 2);
        m_recorder->initialize(m_sampleRate, 2);
    }
    m_project.sampleRate = m_sampleRate;
    auto rebuilt = rebuildGraph();

    m_devices->setAudioCallback(m_callback.get());
    audio::Result started = audio::Result::ok();
    if (!m_devices->isRunning()) started = m_devices->start();
    m_deviceOpen = bool(started) && m_devices->isRunning();
    if (!started) return started;
    return rebuilt;
}

audio::Result EngineController::setOutputDevice(const std::string& uid) {
    auto config = audioConfiguration();
    config.outputDeviceUid = uid;
    config.outputChannelSelectors.clear();
    return applyAudioConfiguration(config);
}
audio::Result EngineController::setInputDevice(const std::string& uid) {
    auto config = audioConfiguration();
    config.inputEnabled = !uid.empty();
    config.inputDeviceUid = uid;
    config.inputChannelSelectors.clear();
    return applyAudioConfiguration(config);
}

audio::Result EngineController::probeDevice(const std::string& uid,
                                            bool wantInput,
                                            audio::DeviceInfo& out) {
    return m_devices->probeDevice(uid, wantInput, out);
}

audio::Result EngineController::showDeviceControlPanel(const std::string& uid,
                                                       void* nativeWindow) {
    m_devices->setAudioCallback(nullptr);
    auto shown = m_devices->showControlPanel(uid, nativeWindow);
    if (shown) {
        const double settledRate = m_devices->sampleRate();
        m_bufferSize = m_devices->bufferSize();
        if (std::abs(settledRate - m_sampleRate) > 0.01)
            applyRenderSampleRate(settledRate);
        else
            (void)m_engine.prepare(m_sampleRate, m_bufferSize, 2);
        m_project.sampleRate = m_sampleRate;
        (void)rebuildGraph();
    }
    m_devices->setAudioCallback(m_callback.get());
    m_deviceOpen = m_devices->isRunning();
    return shown;
}

audio::Result EngineController::setSampleRateHz(double hz) {
    auto config = audioConfiguration();
    config.sampleRate = hz;
    return applyAudioConfiguration(config);
}

audio::Result EngineController::setBufferSizeFrames(uint32_t frames) {
    auto config = audioConfiguration();
    config.bufferSize = frames;
    return applyAudioConfiguration(config);
}

} // namespace daw
