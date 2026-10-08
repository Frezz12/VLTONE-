#include "EngineController.hpp"
#include "Internal/PitchCorrectorInstance.hpp"

#include <algorithm>
#include <array>

namespace daw {
namespace {
namespace pitch = plugins::pitch;
struct PitchTarget {
    std::string channel;
    collab::PluginLocation location;
    const InsertModel* model;
};

std::vector<PitchTarget> pitchTargets(const ProjectModel& project) {
    std::vector<PitchTarget> targets;
    const auto collect = [&](const auto& inserts, const std::string& channel,
                             const collab::PluginLocation& location) {
        for (const auto& insert : inserts)
            if (insert.format == PluginFormat::Internal &&
                insert.uid == pitch::PitchCorrectorInstance::uid())
                targets.push_back({channel, location, &insert});
    };
    for (const auto& track : project.tracks) {
        collect(track.inserts, track.id, {collab::PluginChain::Track, track.id, {}});
        if (track.samplerFx.isOwnedBy(track.instrument))
            collect(track.samplerFx.inserts, track.id, {collab::PluginChain::SamplerFx, track.id, {}});
        for (const auto& clip : track.clips)
            collect(clip.inserts, track.id, {collab::PluginChain::Clip, track.id, clip.id});
    }
    collect(project.masterInserts, EngineController::kMasterChannelId,
            {collab::PluginChain::Master, {}, {}});
    return targets;
}
}

std::size_t EngineController::pitchCorrectorCount() const {
    return pitchTargets(m_project).size();
}

audio::Result EngineController::applyKeyToPitchCorrectors(
    int root, const std::string& scale, std::size_t* updated) {
    if (updated) *updated = 0;
    constexpr std::array<std::string_view, 6> scales{
        "major", "natural_minor", "harmonic_minor", "melodic_minor",
        "major_pentatonic", "minor_pentatonic"};
    const auto found = std::find(scales.begin(), scales.end(), scale);
    if (root < 0 || root > 11 || found == scales.end())
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                  "The detected key is not supported by VLT Pitch.");
    return applyPitchCorrectorParameters(
        {{"key", double(root)}, {"scale", double(1 + std::distance(scales.begin(), found))}},
        "Apply Key to VLT Pitch", {}, {}, updated);
}

audio::Result EngineController::copyPitchCorrectorSettings(
    const std::string& channelId, const std::string& insertId, std::size_t* updated) {
    if (updated) *updated = 0;
    const AudioPluginAddress source{channelId, insertId};
    if (!m_runtime.hasPlugin(source, pitch::PitchCorrectorInstance::uid()))
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                  "The source VLT Pitch instance is unavailable.");
    const auto values = m_runtime.pluginParameterValues(source);
    return applyPitchCorrectorParameters(values, "Send VLT Pitch Settings to All",
                                         channelId, insertId, updated);
}

audio::Result EngineController::applyPitchCorrectorParameters(
    const std::vector<InsertParameter>& values, const std::string& label,
    const std::string& excludeChannel, const std::string& excludeInsert,
    std::size_t* updated) {
    struct Edit {
        std::string channel, insert, parameter;
        PluginEditorChannel side;
        double before, after;
    };
    std::vector<Edit> edits;
    auto batch = std::make_shared<collab::BatchCommand>();
    std::size_t targets = 0, changed = 0;
    // Gather and validate before writing anything. Submitting a shared batch
    // may replace the entire project, so no model pointers survive this loop.
    for (const auto& target : pitchTargets(m_project)) {
        const auto& slot = *target.model;
        if (target.channel == excludeChannel && slot.id == excludeInsert) continue;
        ++targets;
        bool slotChanged = false;
        const int sides = slot.channelMode == PluginChannelMode::DualMono ? 2 : 1;
        for (int side = 0; side < sides; ++side) {
            const AudioPluginAddress address{target.channel, slot.id, side != 0};
            if (!m_runtime.hasPlugin(address, pitch::PitchCorrectorInstance::uid()))
                return audio::Result::fail(audio::EngineError::InvalidArgument,
                                          "A VLT Pitch instance is unavailable. No settings were changed.");
            const auto current = m_runtime.pluginParameterValues(address);
            for (const auto& parameter : values) {
                const auto found = std::find_if(current.begin(), current.end(),
                    [&](const auto& value) { return value.id == parameter.id; });
                if (found == current.end())
                    return audio::Result::fail(audio::EngineError::InvalidArgument,
                        "A VLT Pitch parameter is unavailable. No settings were changed.");
                const double before = found->value;
                if (before == parameter.value) continue;
                if (parameter.id == "quality" && liveAudioActivity())
                    return audio::Result::fail(audio::EngineError::InvalidArgument,
                        "Stop playback and input monitoring to send settings with a different quality.");
                edits.push_back({target.channel, slot.id, parameter.id,
                    side ? PluginEditorChannel::Right : PluginEditorChannel::Left, before, parameter.value});
                collab::ProjectCommand command;
                command.body = collab::SetPluginParameter{target.location, slot.id,
                    parameter.id, parameter.value, side != 0};
                batch->commands.push_back(std::move(command));
                slotChanged = true;
            }
        }
        if (slotChanged) ++changed;
    }
    if (targets == 0)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
            excludeInsert.empty() ? "There are no VLT Pitch instances in this project."
                                  : "There are no other VLT Pitch instances in this project.");
    if (edits.empty()) return audio::Result::ok();
    const auto shared = submitSharedMutation(batch, label);
    if (shared == collab::SharedMutationResult::Blocked)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
                                  "The project could not accept the VLT Pitch settings.");
    if (shared == collab::SharedMutationResult::LocalFallback) {
        const auto apply = [this, edits](bool after) {
            for (const auto& edit : edits) {
                const auto* model = insertModel(edit.channel, edit.insert);
                if (!model) continue;
                const auto previousSide = model->editorChannel;
                setInsertEditorChannel(edit.channel, edit.insert, edit.side);
                setInsertParameter(edit.channel, edit.insert, edit.parameter, after ? edit.after : edit.before);
                setInsertEditorChannel(edit.channel, edit.insert, previousSide);
            }
        };
        apply(true);
        m_undo.push(label, [apply] { apply(false); }, [apply] { apply(true); });
    }
    if (updated) *updated = changed;
    return audio::Result::ok();
}
} // namespace daw
