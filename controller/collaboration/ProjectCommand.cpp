#include "collaboration/ProjectCommand.hpp"

#include <cctype>
#include <set>
#include <type_traits>

namespace daw::collab {

std::string projectScalarName(ProjectScalar field) {
    switch (field) {
        case ProjectScalar::Name: return "name";
        case ProjectScalar::Tempo: return "tempo";
        case ProjectScalar::AiInstructions: return "aiInstructions";
        case ProjectScalar::RenderSampleRate: return "renderSampleRate";
        case ProjectScalar::MasterVolume: return "masterVolume";
        case ProjectScalar::MasterPan: return "masterPan";
    }
    return "name";
}

bool projectScalarFromName(const std::string& name, ProjectScalar& out) {
    if (name == "name") out = ProjectScalar::Name;
    else if (name == "tempo") out = ProjectScalar::Tempo;
    else if (name == "aiInstructions") out = ProjectScalar::AiInstructions;
    else if (name == "renderSampleRate") out = ProjectScalar::RenderSampleRate;
    else if (name == "masterVolume") out = ProjectScalar::MasterVolume;
    else if (name == "masterPan") out = ProjectScalar::MasterPan;
    else return false;
    return true;
}

std::string trackPropertyName(TrackProperty property) {
    switch (property) {
        case TrackProperty::Name: return "name";
        case TrackProperty::Color: return "color";
        case TrackProperty::Volume: return "volume";
        case TrackProperty::Pan: return "pan";
        case TrackProperty::Muted: return "muted";
        case TrackProperty::Mono: return "mono";
        case TrackProperty::Summing: return "summing";
    }
    return "name";
}

bool trackPropertyFromName(const std::string& name, TrackProperty& out) {
    if (name == "name") out = TrackProperty::Name;
    else if (name == "color") out = TrackProperty::Color;
    else if (name == "volume") out = TrackProperty::Volume;
    else if (name == "pan") out = TrackProperty::Pan;
    else if (name == "muted") out = TrackProperty::Muted;
    else if (name == "mono") out = TrackProperty::Mono;
    else if (name == "summing") out = TrackProperty::Summing;
    else return false;
    return true;
}

std::string clipPropertyName(ClipProperty property) {
    switch (property) {
        case ClipProperty::Name: return "name";
        case ClipProperty::StartSeconds: return "startSeconds";
        case ClipProperty::DurationSeconds: return "durationSeconds";
        case ClipProperty::OffsetSeconds: return "offsetSeconds";
        case ClipProperty::Gain: return "gain";
        case ClipProperty::Pan: return "pan";
        case ClipProperty::Muted: return "muted";
        case ClipProperty::Color: return "color";
        case ClipProperty::CompCrossfadeMs: return "compCrossfadeMs";
    }
    return "name";
}

bool clipPropertyFromName(const std::string& name, ClipProperty& out) {
    if (name == "name") out = ClipProperty::Name;
    else if (name == "startSeconds") out = ClipProperty::StartSeconds;
    else if (name == "durationSeconds") out = ClipProperty::DurationSeconds;
    else if (name == "offsetSeconds") out = ClipProperty::OffsetSeconds;
    else if (name == "gain") out = ClipProperty::Gain;
    else if (name == "pan") out = ClipProperty::Pan;
    else if (name == "muted") out = ClipProperty::Muted;
    else if (name == "color") out = ClipProperty::Color;
    else if (name == "compCrossfadeMs") out = ClipProperty::CompCrossfadeMs;
    else return false;
    return true;
}

std::string clipEdgeName(ClipEdge edge) {
    return edge == ClipEdge::In ? "in" : "out";
}

bool clipEdgeFromName(const std::string& name, ClipEdge& out) {
    if (name == "in") out = ClipEdge::In;
    else if (name == "out") out = ClipEdge::Out;
    else return false;
    return true;
}

std::string takePropertyName(TakeProperty property) {
    switch (property) {
        case TakeProperty::Name: return "name";
        case TakeProperty::OffsetSeconds: return "offsetSeconds";
        case TakeProperty::LengthSeconds: return "lengthSeconds";
        case TakeProperty::ClipOffsetSeconds: return "clipOffsetSeconds";
        case TakeProperty::Gain: return "gain";
        case TakeProperty::Muted: return "muted";
        case TakeProperty::Color: return "color";
    }
    return "name";
}

bool takePropertyFromName(const std::string& name, TakeProperty& out) {
    if (name == "name") out = TakeProperty::Name;
    else if (name == "offsetSeconds") out = TakeProperty::OffsetSeconds;
    else if (name == "lengthSeconds") out = TakeProperty::LengthSeconds;
    else if (name == "clipOffsetSeconds") out = TakeProperty::ClipOffsetSeconds;
    else if (name == "gain") out = TakeProperty::Gain;
    else if (name == "muted") out = TakeProperty::Muted;
    else if (name == "color") out = TakeProperty::Color;
    else return false;
    return true;
}

std::string sendPropertyName(SendProperty property) {
    switch (property) {
        case SendProperty::DestinationTrackId: return "destinationTrackId";
        case SendProperty::Level: return "level";
        case SendProperty::PreFader: return "preFader";
        case SendProperty::Enabled: return "enabled";
    }
    return "level";
}

bool sendPropertyFromName(const std::string& name, SendProperty& out) {
    if (name == "destinationTrackId") out = SendProperty::DestinationTrackId;
    else if (name == "level") out = SendProperty::Level;
    else if (name == "preFader") out = SendProperty::PreFader;
    else if (name == "enabled") out = SendProperty::Enabled;
    else return false;
    return true;
}

std::string pluginChainName(PluginChain chain) {
    switch (chain) {
        case PluginChain::Master: return "master";
        case PluginChain::Track: return "track";
        case PluginChain::Instrument: return "instrument";
        case PluginChain::SamplerFx: return "samplerFx";
        case PluginChain::Clip: return "clip";
    }
    return "track";
}

bool pluginChainFromName(const std::string& name, PluginChain& out) {
    if (name == "master") out = PluginChain::Master;
    else if (name == "track") out = PluginChain::Track;
    else if (name == "instrument") out = PluginChain::Instrument;
    else if (name == "samplerFx") out = PluginChain::SamplerFx;
    else if (name == "clip") out = PluginChain::Clip;
    else return false;
    return true;
}

std::string pluginPropertyName(PluginProperty property) {
    switch (property) {
        case PluginProperty::Name: return "name";
        case PluginProperty::Bypassed: return "bypassed";
        case PluginProperty::Mix: return "mix";
        case PluginProperty::ChannelMode: return "channelMode";
        case PluginProperty::SidechainTrackId: return "sidechainTrackId";
    }
    return "name";
}

bool pluginPropertyFromName(const std::string& name, PluginProperty& out) {
    if (name == "name") out = PluginProperty::Name;
    else if (name == "bypassed") out = PluginProperty::Bypassed;
    else if (name == "mix") out = PluginProperty::Mix;
    else if (name == "channelMode") out = PluginProperty::ChannelMode;
    else if (name == "sidechainTrackId") out = PluginProperty::SidechainTrackId;
    else return false;
    return true;
}

std::string commandKind(const ProjectCommand& command) {
    return std::visit([](const auto& body) -> std::string {
        using T = std::decay_t<decltype(body)>;
        if constexpr (std::is_same_v<T, SetProjectScalar>)
            return "project.setScalar";
        else if constexpr (std::is_same_v<T, SetTimeSignature>)
            return "project.setTimeSignature";
        else if constexpr (std::is_same_v<T, SetProjectKey>)
            return "project.setKey";
        else if constexpr (std::is_same_v<T, AddTrack>)
            return "track.add";
        else if constexpr (std::is_same_v<T, DeleteTrack>)
            return "track.delete";
        else if constexpr (std::is_same_v<T, RestoreTrack>)
            return "track.restore";
        else if constexpr (std::is_same_v<T, MoveTrack>)
            return "track.move";
        else if constexpr (std::is_same_v<T, SetTrackProperty>)
            return "track.setProperty";
        else if constexpr (std::is_same_v<T, SetTrackParent>)
            return "track.setParent";
        else if constexpr (std::is_same_v<T, SetTrackOutput>)
            return "track.setOutput";
        else if constexpr (std::is_same_v<T, AddSend>)
            return "send.add";
        else if constexpr (std::is_same_v<T, DeleteSend>)
            return "send.delete";
        else if constexpr (std::is_same_v<T, RestoreSend>)
            return "send.restore";
        else if constexpr (std::is_same_v<T, MoveSend>)
            return "send.move";
        else if constexpr (std::is_same_v<T, SetSendProperty>)
            return "send.setProperty";
        else if constexpr (std::is_same_v<T, AddClip>)
            return "clip.add";
        else if constexpr (std::is_same_v<T, DeleteClip>)
            return "clip.delete";
        else if constexpr (std::is_same_v<T, RestoreClip>)
            return "clip.restore";
        else if constexpr (std::is_same_v<T, MoveClip>)
            return "clip.move";
        else if constexpr (std::is_same_v<T, SetClipProperty>)
            return "clip.setProperty";
        else if constexpr (std::is_same_v<T, SetClipAsset>)
            return "clip.setAsset";
        else if constexpr (std::is_same_v<T, SetClipSampleEdit>)
            return "clip.setSampleEdit";
        else if constexpr (std::is_same_v<T, SetClipFade>)
            return "clip.setFade";
        else if constexpr (std::is_same_v<T, SetClipFadeCurve>)
            return "clip.setFadeCurve";
        else if constexpr (std::is_same_v<T, SetClipFadeMode>)
            return "clip.setFadeMode";
        else if constexpr (std::is_same_v<T, SetClipPatternOwner>)
            return "clip.setPatternOwner";
        else if constexpr (std::is_same_v<T, SetClipMusicalAnalysis>)
            return "clip.setMusicalAnalysis";
        else if constexpr (std::is_same_v<T, AddPluginInsert>)
            return "plugin.add";
        else if constexpr (std::is_same_v<T, DeletePluginInsert>)
            return "plugin.delete";
        else if constexpr (std::is_same_v<T, RestorePluginInsert>)
            return "plugin.restore";
        else if constexpr (std::is_same_v<T, MovePluginInsert>)
            return "plugin.move";
        else if constexpr (std::is_same_v<T, ReplacePluginInsert>)
            return "plugin.replace";
        else if constexpr (std::is_same_v<T, SetPluginProperty>)
            return "plugin.setProperty";
        else if constexpr (std::is_same_v<T, SetPluginState>)
            return "plugin.setState";
        else if constexpr (std::is_same_v<T, SetPluginParameter>)
            return "plugin.setParameter";
        else if constexpr (std::is_same_v<T, RemovePluginParameter>)
            return "plugin.removeParameter";
        else if constexpr (std::is_same_v<T, SetPluginAssetBinding>)
            return "plugin.setAssetBinding";
        else if constexpr (std::is_same_v<T, RemovePluginAssetBinding>)
            return "plugin.removeAssetBinding";
        else if constexpr (std::is_same_v<T, SetSamplerFxLevels>)
            return "samplerFx.setLevels";
        else if constexpr (std::is_same_v<T, UpsertMidiNote>)
            return "note.upsert";
        else if constexpr (std::is_same_v<T, DeleteMidiNote>)
            return "note.delete";
        else if constexpr (std::is_same_v<T, RestoreMidiNote>)
            return "note.restore";
        else if constexpr (std::is_same_v<T, UpsertAutomationPoint>)
            return "automationPoint.upsert";
        else if constexpr (std::is_same_v<T, DeleteAutomationPoint>)
            return "automationPoint.delete";
        else if constexpr (std::is_same_v<T, RestoreAutomationPoint>)
            return "automationPoint.restore";
        else if constexpr (std::is_same_v<T, AddControllerLane>)
            return "controllerLane.add";
        else if constexpr (std::is_same_v<T, DeleteControllerLane>)
            return "controllerLane.delete";
        else if constexpr (std::is_same_v<T, RestoreControllerLane>)
            return "controllerLane.restore";
        else if constexpr (std::is_same_v<T, SetControllerLaneTarget>)
            return "controllerLane.setTarget";
        else if constexpr (std::is_same_v<T, SetControllerLaneDefault>)
            return "controllerLane.setDefault";
        else if constexpr (std::is_same_v<T, SetAutomationTarget>)
            return "automation.setTarget";
        else if constexpr (std::is_same_v<T, SetAutomationDefault>)
            return "automation.setDefault";
        else if constexpr (std::is_same_v<T, SetAutomationActive>)
            return "automation.setActive";
        else if constexpr (std::is_same_v<T, AddTake>)
            return "take.add";
        else if constexpr (std::is_same_v<T, DeleteTake>)
            return "take.delete";
        else if constexpr (std::is_same_v<T, RestoreTake>)
            return "take.restore";
        else if constexpr (std::is_same_v<T, MoveTake>)
            return "take.move";
        else if constexpr (std::is_same_v<T, SetTakeProperty>)
            return "take.setProperty";
        else if constexpr (std::is_same_v<T, UpsertCompSegment>)
            return "compSegment.upsert";
        else if constexpr (std::is_same_v<T, DeleteCompSegment>)
            return "compSegment.delete";
        else if constexpr (std::is_same_v<T, RestoreCompSegment>)
            return "compSegment.restore";
        else if constexpr (std::is_same_v<T, RecordingCommit>)
            return "recording.commit";
        else
            return "batch";
    }, command.body);
}

bool isUuid(std::string_view value) noexcept {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!(std::isdigit(c) || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool commandHasValidIds(const ProjectCommand& command, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    const auto requireUuid = [&](std::string_view value,
                                 std::string_view label) {
        return isUuid(value) ||
               fail(std::string(label) + " must be an RFC UUID");
    };
    const auto requireOptionalUuid = [&](std::string_view value,
                                         std::string_view label) {
        return value.empty() || requireUuid(value, label);
    };
    const auto requireAssetId = [&](const AssetRef& asset,
                                    std::string_view label, bool allowEmpty) {
        if (allowEmpty && asset.empty()) return true;
        return requireUuid(asset.assetId, label);
    };
    const auto requireLocationIds = [&](const PluginLocation& location) {
        switch (location.chain) {
            case PluginChain::Master:
                return (location.trackId.empty() && location.clipId.empty()) ||
                       fail("master plugin location must not carry entity ids");
            case PluginChain::Track:
            case PluginChain::Instrument:
            case PluginChain::SamplerFx:
                return requireUuid(location.trackId, "location.trackId") &&
                       (location.clipId.empty() ||
                        fail("track plugin location must not carry clipId"));
            case PluginChain::Clip:
                return requireUuid(location.trackId, "location.trackId") &&
                       requireUuid(location.clipId, "location.clipId");
        }
        return fail("plugin location is invalid");
    };
    const auto requireBindingAsset = [&](const PluginAssetBinding& binding) {
        if (binding.key.empty()) return fail("binding key must not be empty");
        return requireAssetId(binding.asset, "binding.asset.assetId", false);
    };

    if (!requireUuid(command.meta.operationId, "opId") ||
        !requireOptionalUuid(command.meta.transactionId, "transactionId") ||
        !requireOptionalUuid(command.meta.projectId, "projectId") ||
        !requireOptionalUuid(command.meta.actorId, "actorId") ||
        !requireOptionalUuid(command.meta.clientId, "clientId")) {
        return false;
    }
    std::size_t preconditionCount = 0;
    const auto validateBody = [&](const auto& self,
                                  const ProjectCommand& value) -> bool {
        preconditionCount += value.conditions.size();
        if (preconditionCount > kMaxProjectCommandPreconditions)
            return fail("command has too many preconditions");
        for (const CommandCondition& condition : value.conditions) {
            if (condition.fieldKey.empty())
                return fail("precondition fieldKey must not be empty");
            if (!requireUuid(condition.operationId,
                             "precondition operationId")) {
                return false;
            }
        }
        return std::visit([&](const auto& body) -> bool {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, AddTrack>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireOptionalUuid(body.parentId, "parentId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteTrack> ||
                                 std::is_same_v<T, SetTrackProperty>) {
                return requireUuid(body.trackId, "trackId");
            } else if constexpr (std::is_same_v<T, RestoreTrack>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, MoveTrack>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, SetTrackParent>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireOptionalUuid(body.parentId, "parentId");
            } else if constexpr (std::is_same_v<T, SetTrackOutput>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireOptionalUuid(body.outputTrackId,
                                           "outputTrackId");
            } else if constexpr (std::is_same_v<T, AddSend>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.send.id, "send.id") &&
                       requireUuid(body.send.destinationTrackId,
                                   "send.destinationTrackId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteSend> ||
                                 std::is_same_v<T, SetSendProperty>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.sendId, "sendId");
            } else if constexpr (std::is_same_v<T, RestoreSend>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.sendId, "sendId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, MoveSend>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.sendId, "sendId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, AddClip>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteClip> ||
                                 std::is_same_v<T, SetClipProperty>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId");
            } else if constexpr (std::is_same_v<T, RestoreClip>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, MoveClip>) {
                return requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.sourceTrackId, "sourceTrackId") &&
                       requireUuid(body.trackId, "trackId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, SetClipAsset> ||
                                 std::is_same_v<T, SetClipSampleEdit> ||
                                 std::is_same_v<T, SetClipFade> ||
                                 std::is_same_v<T, SetClipFadeCurve> ||
                                 std::is_same_v<T, SetClipFadeMode> ||
                                 std::is_same_v<T,
                                                SetClipMusicalAnalysis>) {
                if (!requireUuid(body.trackId, "trackId") ||
                    !requireUuid(body.clipId, "clipId")) {
                    return false;
                }
                if constexpr (std::is_same_v<T, SetClipAsset>)
                    return requireAssetId(body.asset, "asset.assetId", true);
                return true;
            } else if constexpr (std::is_same_v<T,
                                               SetClipPatternOwner>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireOptionalUuid(body.patternClipId,
                                           "patternClipId");
            } else if constexpr (std::is_same_v<T, AddPluginInsert>) {
                if (!requireLocationIds(body.location) ||
                    !requireUuid(body.insert.id, "insert.id") ||
                    !requireOptionalUuid(body.insert.sidechainTrackId,
                                         "insert.sidechainTrackId") ||
                    !requireAssetId(body.insert.stateAsset,
                                    "insert.stateAsset.assetId", true) ||
                    !requireAssetId(body.insert.rightStateAsset,
                                    "insert.rightStateAsset.assetId", true) ||
                    !requireOptionalUuid(body.afterId, "afterId")) {
                    return false;
                }
                for (const PluginAssetBinding& binding :
                     body.insert.assetBindings) {
                    if (!requireBindingAsset(binding)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, DeletePluginInsert>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId");
            } else if constexpr (std::is_same_v<T, RestorePluginInsert>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, MovePluginInsert>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T,
                                               ReplacePluginInsert>) {
                if (!requireLocationIds(body.location) ||
                    !requireUuid(body.insertId, "insertId") ||
                    !requireUuid(body.replacement.id, "replacement.id") ||
                    !requireOptionalUuid(body.replacement.sidechainTrackId,
                                         "replacement.sidechainTrackId") ||
                    !requireAssetId(body.replacement.stateAsset,
                                    "replacement.stateAsset.assetId", true) ||
                    !requireAssetId(body.replacement.rightStateAsset,
                                    "replacement.rightStateAsset.assetId",
                                    true)) {
                    return false;
                }
                for (const PluginAssetBinding& binding :
                     body.replacement.assetBindings) {
                    if (!requireBindingAsset(binding)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, SetPluginProperty>) {
                if (!requireLocationIds(body.location) ||
                    !requireUuid(body.insertId, "insertId")) {
                    return false;
                }
                return body.property != PluginProperty::SidechainTrackId ||
                       !std::holds_alternative<std::string>(body.value) ||
                       requireOptionalUuid(std::get<std::string>(body.value),
                                           "sidechainTrackId");
            } else if constexpr (std::is_same_v<T, SetPluginState>) {
                if (!requireLocationIds(body.location) ||
                    !requireUuid(body.insertId, "insertId") ||
                    !requireAssetId(body.stateAsset, "stateAsset.assetId",
                                    true) ||
                    !requireAssetId(body.rightStateAsset,
                                    "rightStateAsset.assetId", true)) {
                    return false;
                }
                for (const PluginAssetBinding& binding : body.assetBindings) {
                    if (!requireBindingAsset(binding)) return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, SetPluginParameter> ||
                                 std::is_same_v<T,
                                                RemovePluginParameter>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId") &&
                       (!body.parameterId.empty() ||
                        fail("parameterId must not be empty")) &&
                       (body.parameterId.size() <=
                            kMaxPluginParameterIdBytes ||
                        fail("parameterId is too long for a field key"));
            } else if constexpr (std::is_same_v<T,
                                                SetPluginAssetBinding>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId") &&
                       requireBindingAsset(body.binding);
            } else if constexpr (std::is_same_v<T,
                                                RemovePluginAssetBinding>) {
                return requireLocationIds(body.location) &&
                       requireUuid(body.insertId, "insertId") &&
                       (!body.key.empty() || fail("binding key must not be empty"));
            } else if constexpr (std::is_same_v<T,
                                               SetSamplerFxLevels>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.instrumentId, "instrumentId");
            } else if constexpr (std::is_same_v<T, UpsertMidiNote>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.note.id, "note.id") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteMidiNote>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.noteId, "noteId");
            } else if constexpr (std::is_same_v<T, RestoreMidiNote>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.noteId, "noteId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, UpsertAutomationPoint>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireOptionalUuid(body.laneId, "laneId") &&
                       requireUuid(body.point.id, "point.id") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteAutomationPoint>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireOptionalUuid(body.laneId, "laneId") &&
                       requireUuid(body.pointId, "pointId");
            } else if constexpr (std::is_same_v<T,
                                                RestoreAutomationPoint>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireOptionalUuid(body.laneId, "laneId") &&
                       requireUuid(body.pointId, "pointId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, AddControllerLane>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.laneId, "laneId") &&
                       requireOptionalUuid(body.target.slotId, "target.slotId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteControllerLane> ||
                                 std::is_same_v<T, SetControllerLaneDefault>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.laneId, "laneId");
            } else if constexpr (std::is_same_v<T, RestoreControllerLane>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.laneId, "laneId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T,
                                                SetControllerLaneTarget>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.laneId, "laneId") &&
                       requireOptionalUuid(body.target.slotId,
                                           "target.slotId");
            } else if constexpr (std::is_same_v<T, SetAutomationTarget>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.target.channelId,
                                   "target.channelId") &&
                       requireOptionalUuid(body.target.slotId,
                                           "target.slotId") &&
                       requireOptionalUuid(body.target.sendId,
                                           "target.sendId");
            } else if constexpr (std::is_same_v<T, SetAutomationDefault> ||
                                 std::is_same_v<T, SetAutomationActive>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId");
            } else if constexpr (std::is_same_v<T, AddTake>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.take.id, "take.id") &&
                       requireUuid(body.take.asset.assetId,
                                   "take.asset.assetId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteTake>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.takeId, "takeId");
            } else if constexpr (std::is_same_v<T, RestoreTake>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.takeId, "takeId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, MoveTake>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.takeId, "takeId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, SetTakeProperty>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.takeId, "takeId");
            } else if constexpr (std::is_same_v<T, UpsertCompSegment>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.segment.id, "segment.id") &&
                       requireUuid(body.segment.takeId, "segment.takeId") &&
                       requireOptionalUuid(body.afterId, "afterId");
            } else if constexpr (std::is_same_v<T, DeleteCompSegment>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.segmentId, "segmentId");
            } else if constexpr (std::is_same_v<T, RestoreCompSegment>) {
                return requireUuid(body.trackId, "trackId") &&
                       requireUuid(body.clipId, "clipId") &&
                       requireUuid(body.segmentId, "segmentId") &&
                       requireUuid(body.deleteOperationId,
                                   "deleteOperationId");
            } else if constexpr (std::is_same_v<T, RecordingCommit>) {
                const bool leaseFreeNewClips = body.leases.empty();
                if (!body.batch ||
                    (leaseFreeNewClips &&
                     value.meta.schemaVersion ==
                         kProjectCommandSchemaVersionV2) ||
                    body.leases.size() > kMaxProjectCommandBatchSize) {
                    return fail("recording lease count is out of bounds");
                }
                if (body.batch->commands.empty() ||
                    body.batch->commands.size() >
                        kMaxProjectCommandBatchSize) {
                    return fail("recording command count is out of bounds");
                }
                std::set<std::string> leaseTrackIds;
                std::set<std::string> leaseIds;
                for (const RecordingLeaseClaim& lease : body.leases) {
                    if (!requireUuid(lease.trackId,
                                     "recording lease trackId") ||
                        !requireUuid(lease.leaseId,
                                     "recording lease leaseId")) {
                        return false;
                    }
                    if (!leaseTrackIds.insert(lease.trackId).second)
                        return fail("recording lease trackId is duplicated");
                    if (!leaseIds.insert(lease.leaseId).second)
                        return fail("recording lease leaseId is duplicated");
                }
                std::set<std::string> commandTrackIds;
                std::set<std::pair<std::string, std::string>> newClips;
                for (const ProjectCommand& child : body.batch->commands) {
                    if (std::holds_alternative<RecordingCommit>(child.body) ||
                        std::holds_alternative<
                            std::shared_ptr<BatchCommand>>(child.body)) {
                        return fail(
                            "recording.commit cannot contain nested transactions");
                    }
                    const bool allowed =
                        std::holds_alternative<AddClip>(child.body) ||
                        std::holds_alternative<SetClipProperty>(child.body) ||
                        std::holds_alternative<SetClipAsset>(child.body) ||
                        std::holds_alternative<AddTake>(child.body) ||
                        std::holds_alternative<UpsertCompSegment>(child.body);
                    if (!allowed) {
                        return fail(
                            "command kind is not allowed in recording.commit");
                    }
                    if (!self(self, child)) return false;
                    if (const auto* added =
                            std::get_if<AddClip>(&child.body)) {
                        newClips.emplace(added->trackId, added->clipId);
                    }
                    std::visit([&](const auto& childBody) {
                        if constexpr (requires { childBody.trackId; }) {
                            if (!childBody.trackId.empty())
                                commandTrackIds.insert(childBody.trackId);
                        } else if constexpr (requires {
                                                 childBody.location.trackId;
                                             }) {
                            if (!childBody.location.trackId.empty()) {
                                commandTrackIds.insert(
                                    childBody.location.trackId);
                            }
                        }
                    }, child.body);
                }
                if (leaseFreeNewClips) {
                    for (const ProjectCommand& child : body.batch->commands) {
                        const bool targetsNewClip = std::visit(
                            [&](const auto& childBody) {
                                if constexpr (requires {
                                                  childBody.trackId;
                                                  childBody.clipId;
                                              }) {
                                    return newClips.contains(
                                        {childBody.trackId,
                                         childBody.clipId});
                                }
                                return false;
                            },
                            child.body);
                        if (!targetsNewClip) {
                            return fail(
                                "lease-free recording commit may only mutate clips created in the same command");
                        }
                    }
                    return true;
                }
                return commandTrackIds == leaseTrackIds ||
                       fail("recording command tracks do not match leases");
            } else if constexpr (std::is_same_v<T,
                                                std::shared_ptr<BatchCommand>>) {
                if (!body) return true;
                if (body->commands.empty() ||
                    body->commands.size() > kMaxProjectCommandBatchSize) {
                    return fail("batch command count is out of bounds");
                }
                for (const ProjectCommand& child : body->commands) {
                    if (std::holds_alternative<RecordingCommit>(child.body) ||
                        std::holds_alternative<
                            std::shared_ptr<BatchCommand>>(child.body)) {
                        return fail("batch cannot contain nested transactions");
                    }
                    if (!self(self, child)) return false;
                }
                return true;
            } else {
                return true;
            }
        }, value.body);
    };
    if (!validateBody(validateBody, command)) return false;
    if (error) error->clear();
    return true;
}

std::vector<std::string> commandTouchedFields(const ProjectCommand& command) {
    std::set<std::string> fields;
    const auto addNoteFields = [&](const std::string& id) {
        const std::string prefix = "note:" + id + ":";
        fields.insert(prefix + "position");
        fields.insert(prefix + "pitch");
        fields.insert(prefix + "startBeats");
        fields.insert(prefix + "lengthBeats");
        fields.insert(prefix + "velocity");
        fields.insert(prefix + "muted");
        fields.insert(prefix + "color");
        fields.insert(prefix + "pan");
    };
    const auto addAutomationPointFields = [&](const std::string& id) {
        const std::string prefix = "automationPoint:" + id + ":";
        fields.insert(prefix + "position");
        fields.insert(prefix + "beats");
        fields.insert(prefix + "value");
        fields.insert(prefix + "shape");
        fields.insert(prefix + "curve");
    };
    const auto addCompSegmentFields = [&](const std::string& id) {
        const std::string prefix = "compSegment:" + id + ":";
        fields.insert(prefix + "position");
        fields.insert(prefix + "takeId");
        fields.insert(prefix + "startSeconds");
        fields.insert(prefix + "endSeconds");
    };
    const auto addClipDescendants = [&](const std::string& clipId) {
        fields.insert("clip:" + clipId + ":descendants");
    };
    const auto addTrackClipLandingHead = [&](const std::string& trackId) {
        fields.insert("track:" + trackId + ":clipLanding");
    };
    const auto addPluginGenerationHead = [&](const std::string& insertId) {
        fields.insert("plugin:" + insertId + ":generation");
    };
    const auto collect = [&](const auto& self, const ProjectCommand& value) -> void {
        std::visit([&](const auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, SetProjectScalar>) {
                fields.insert("project:" + projectScalarName(body.field));
                if (body.field == ProjectScalar::Tempo)
                    fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, SetTimeSignature>) {
                fields.insert("project:timeSignature");
            } else if constexpr (std::is_same_v<T, SetProjectKey>) {
                fields.insert("project:key");
            } else if constexpr (std::is_same_v<T, AddTrack> ||
                                 std::is_same_v<T, RestoreTrack>) {
                fields.insert("track:" + body.trackId + ":lifecycle");
                fields.insert("track:" + body.trackId + ":position");
                if constexpr (std::is_same_v<T, RestoreTrack>)
                    fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, DeleteTrack>) {
                fields.insert("track:" + body.trackId + ":lifecycle");
                fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, MoveTrack>) {
                fields.insert("track:" + body.trackId + ":position");
            } else if constexpr (std::is_same_v<T, SetTrackProperty>) {
                fields.insert("track:" + body.trackId + ":" +
                              trackPropertyName(body.property));
            } else if constexpr (std::is_same_v<T, SetTrackParent>) {
                fields.insert("track:" + body.trackId + ":parentId");
            } else if constexpr (std::is_same_v<T, SetTrackOutput>) {
                fields.insert("track:" + body.trackId + ":outputTrackId");
            } else if constexpr (std::is_same_v<T, AddSend> ||
                                 std::is_same_v<T, RestoreSend>) {
                const std::string& id = [&]() -> const std::string& {
                    if constexpr (std::is_same_v<T, AddSend>)
                        return body.send.id;
                    else
                        return body.sendId;
                }();
                fields.insert("send:" + id + ":lifecycle");
                fields.insert("send:" + id + ":position");
            } else if constexpr (std::is_same_v<T, DeleteSend>) {
                fields.insert("send:" + body.sendId + ":lifecycle");
            } else if constexpr (std::is_same_v<T, MoveSend>) {
                fields.insert("send:" + body.sendId + ":position");
            } else if constexpr (std::is_same_v<T, SetSendProperty>) {
                fields.insert("send:" + body.sendId + ":" +
                              sendPropertyName(body.property));
            } else if constexpr (std::is_same_v<T, AddClip>) {
                const std::string prefix = "clip:" + body.clipId + ":";
                fields.insert(prefix + "asset");
                fields.insert(prefix + "automationActive");
                fields.insert(prefix + "automationDefaultValue");
                fields.insert(prefix + "automationTarget");
                fields.insert(prefix + "color");
                fields.insert(prefix + "compCrossfadeMs");
                fields.insert(prefix + "durationSeconds");
                fields.insert(prefix + "fadeInSeconds");
                fields.insert(prefix + "fadeOutSeconds");
                fields.insert(prefix + "fadeInCurve");
                fields.insert(prefix + "fadeOutCurve");
                fields.insert(prefix + "fadeInMode");
                fields.insert(prefix + "fadeOutMode");
                fields.insert(prefix + "gain");
                fields.insert(prefix + "lifecycle");
                fields.insert(prefix + "muted");
                fields.insert(prefix + "name");
                fields.insert(prefix + "offsetSeconds");
                fields.insert(prefix + "pan");
                fields.insert(prefix + "patternClipId");
                fields.insert(prefix + "position");
                fields.insert(prefix + "musicalAnalysis");
                fields.insert(prefix + "sampleEdit");
                fields.insert(prefix + "startSeconds");
                addClipDescendants(body.clipId);
                addTrackClipLandingHead(body.trackId);
                fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, RestoreClip>) {
                fields.insert("clip:" + body.clipId + ":lifecycle");
                fields.insert("clip:" + body.clipId + ":position");
                addClipDescendants(body.clipId);
                addTrackClipLandingHead(body.trackId);
                fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, DeleteClip>) {
                fields.insert("clip:" + body.clipId + ":lifecycle");
                addClipDescendants(body.clipId);
                addTrackClipLandingHead(body.trackId);
                fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, MoveClip>) {
                fields.insert("clip:" + body.clipId + ":position");
                addTrackClipLandingHead(body.sourceTrackId);
                addTrackClipLandingHead(body.trackId);
            } else if constexpr (std::is_same_v<T, SetClipProperty>) {
                fields.insert("clip:" + body.clipId + ":" +
                              clipPropertyName(body.property));
                if (body.property == ClipProperty::StartSeconds ||
                    body.property == ClipProperty::DurationSeconds ||
                    body.property == ClipProperty::OffsetSeconds) {
                    addTrackClipLandingHead(body.trackId);
                }
                if (body.property == ClipProperty::StartSeconds ||
                    body.property == ClipProperty::DurationSeconds) {
                    fields.insert("project:tempoCascade");
                }
            } else if constexpr (std::is_same_v<T, SetClipAsset>) {
                fields.insert("clip:" + body.clipId + ":asset");
                addTrackClipLandingHead(body.trackId);
            } else if constexpr (std::is_same_v<T, SetClipSampleEdit>) {
                fields.insert("clip:" + body.clipId + ":sampleEdit");
                addTrackClipLandingHead(body.trackId);
            } else if constexpr (std::is_same_v<T, SetClipFade>) {
                fields.insert("clip:" + body.clipId + ":fadeInSeconds");
                fields.insert("clip:" + body.clipId + ":fadeOutSeconds");
                addTrackClipLandingHead(body.trackId);
                fields.insert("project:tempoCascade");
            } else if constexpr (std::is_same_v<T, SetClipFadeCurve>) {
                fields.insert("clip:" + body.clipId + ":fade" +
                              (body.edge == ClipEdge::In ? "In" : "Out") +
                              "Curve");
            } else if constexpr (std::is_same_v<T, SetClipFadeMode>) {
                fields.insert("clip:" + body.clipId + ":fade" +
                              (body.edge == ClipEdge::In ? "In" : "Out") +
                              "Mode");
            } else if constexpr (std::is_same_v<T,
                                               SetClipPatternOwner>) {
                fields.insert("clip:" + body.clipId + ":patternClipId");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T,
                                               SetClipMusicalAnalysis>) {
                fields.insert("clip:" + body.clipId + ":musicalAnalysis");
            } else if constexpr (std::is_same_v<T, AddPluginInsert> ||
                                 std::is_same_v<T, RestorePluginInsert>) {
                const std::string& id = [&]() -> const std::string& {
                    if constexpr (std::is_same_v<T, AddPluginInsert>)
                        return body.insert.id;
                    else
                        return body.insertId;
                }();
                fields.insert("plugin:" + id + ":lifecycle");
                fields.insert("plugin:" + id + ":position");
                addPluginGenerationHead(id);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T, DeletePluginInsert>) {
                fields.insert("plugin:" + body.insertId + ":lifecycle");
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T, MovePluginInsert>) {
                fields.insert("plugin:" + body.insertId + ":position");
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T,
                                               ReplacePluginInsert>) {
                addPluginGenerationHead(body.insertId);
                fields.insert("plugin:" + body.insertId + ":state");
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T, SetPluginProperty>) {
                fields.insert("plugin:" + body.insertId + ":" +
                              pluginPropertyName(body.property));
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T, SetPluginState>) {
                fields.insert("plugin:" + body.insertId + ":state");
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T, SetPluginParameter> ||
                                 std::is_same_v<T,
                                                RemovePluginParameter>) {
                fields.insert("plugin:" + body.insertId + ":state");
                fields.insert("plugin:" + body.insertId + ":parameter:" +
                              (body.rightChannel ? "right:" : "left:") +
                              body.parameterId);
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T,
                                                SetPluginAssetBinding>) {
                fields.insert("plugin:" + body.insertId + ":state");
                fields.insert("plugin:" + body.insertId + ":assetBinding:" +
                              body.binding.key);
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T,
                                                RemovePluginAssetBinding>) {
                fields.insert("plugin:" + body.insertId + ":state");
                fields.insert("plugin:" + body.insertId + ":assetBinding:" +
                              body.key);
                addPluginGenerationHead(body.insertId);
                if (!body.location.clipId.empty())
                    addClipDescendants(body.location.clipId);
            } else if constexpr (std::is_same_v<T,
                                               SetSamplerFxLevels>) {
                fields.insert("samplerFx:" + body.instrumentId + ":volume");
                fields.insert("samplerFx:" + body.instrumentId + ":pan");
            } else if constexpr (std::is_same_v<T, UpsertMidiNote>) {
                addNoteFields(body.note.id);
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, RestoreMidiNote>) {
                addNoteFields(body.noteId);
                fields.insert("note:" + body.noteId + ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, DeleteMidiNote>) {
                fields.insert("note:" + body.noteId + ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, UpsertAutomationPoint>) {
                addAutomationPointFields(body.point.id);
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T,
                                                RestoreAutomationPoint>) {
                addAutomationPointFields(body.pointId);
                fields.insert("automationPoint:" + body.pointId +
                              ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T,
                                                DeleteAutomationPoint>) {
                fields.insert("automationPoint:" + body.pointId +
                              ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, AddControllerLane> ||
                                 std::is_same_v<T, RestoreControllerLane>) {
                fields.insert("controllerLane:" + body.laneId + ":lifecycle");
                fields.insert("controllerLane:" + body.laneId + ":position");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, DeleteControllerLane>) {
                fields.insert("controllerLane:" + body.laneId + ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T,
                                                SetControllerLaneTarget>) {
                fields.insert("controllerLane:" + body.laneId + ":target");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T,
                                                SetControllerLaneDefault>) {
                fields.insert("controllerLane:" + body.laneId +
                              ":defaultValue");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, SetAutomationTarget>) {
                fields.insert("clip:" + body.clipId + ":automationTarget");
            } else if constexpr (std::is_same_v<T, SetAutomationDefault>) {
                fields.insert("clip:" + body.clipId +
                              ":automationDefaultValue");
            } else if constexpr (std::is_same_v<T, SetAutomationActive>) {
                fields.insert("clip:" + body.clipId + ":automationActive");
            } else if constexpr (std::is_same_v<T, AddTake> ||
                                 std::is_same_v<T, RestoreTake>) {
                const std::string id = [&]() -> std::string {
                    if constexpr (std::is_same_v<T, AddTake>)
                        return body.take.id;
                    else
                        return body.takeId;
                }();
                fields.insert("take:" + id + ":lifecycle");
                fields.insert("take:" + id + ":position");
                fields.insert("take:" + id + ":name");
                fields.insert("take:" + id + ":offsetSeconds");
                fields.insert("take:" + id + ":lengthSeconds");
                fields.insert("take:" + id + ":clipOffsetSeconds");
                fields.insert("take:" + id + ":gain");
                fields.insert("take:" + id + ":muted");
                fields.insert("take:" + id + ":color");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, DeleteTake>) {
                fields.insert("take:" + body.takeId + ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, MoveTake>) {
                fields.insert("take:" + body.takeId + ":position");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, SetTakeProperty>) {
                fields.insert("take:" + body.takeId + ":" +
                              takePropertyName(body.property));
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, UpsertCompSegment>) {
                addCompSegmentFields(body.segment.id);
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, RestoreCompSegment>) {
                addCompSegmentFields(body.segmentId);
                fields.insert("compSegment:" + body.segmentId +
                              ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, DeleteCompSegment>) {
                fields.insert("compSegment:" + body.segmentId +
                              ":lifecycle");
                addClipDescendants(body.clipId);
            } else if constexpr (std::is_same_v<T, RecordingCommit>) {
                for (const RecordingLeaseClaim& lease : body.leases)
                    addTrackClipLandingHead(lease.trackId);
                if (!body.batch) return;
                for (const ProjectCommand& child : body.batch->commands)
                    self(self, child);
            } else if constexpr (std::is_same_v<
                                     T, std::shared_ptr<BatchCommand>>) {
                if (!body) return;
                for (const ProjectCommand& child : body->commands)
                    self(self, child);
            } else {
                static_assert(!sizeof(T), "unhandled ProjectCommand body");
            }
        }, value.body);
    };
    collect(collect, command);
    return {fields.begin(), fields.end()};
}

} // namespace daw::collab
