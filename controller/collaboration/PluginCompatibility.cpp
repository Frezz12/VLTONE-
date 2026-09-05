#include "collaboration/PluginCompatibility.hpp"

#include "plugins/PluginConvert.hpp"
#include "plugins/PluginManager.hpp"

#include <algorithm>
#include <tuple>

namespace daw::collab {
namespace {

auto requirementKey(const PluginRequirement& value) {
    return std::tie(value.format, value.nativeUid, value.vendor, value.version,
                    value.stateSchemaVersion, value.instrument,
                    value.channelMode);
}

void appendRequirement(std::vector<PluginRequirement>& result,
                       const InsertModel& insert, bool instrument) {
    if (!insert.isLoaded()) return;
    PluginRequirement requirement;
    requirement.format = insert.format;
    requirement.nativeUid = insert.uid;
    requirement.vendor = insert.vendor;
    requirement.version = insert.pluginVersion;
    requirement.stateSchemaVersion = insert.stateSchemaVersion;
    requirement.instrument = instrument;
    requirement.channelMode = insert.channelMode;
    result.push_back(std::move(requirement));
}

bool supportsChannelMode(const plugins::PluginDescriptor& descriptor,
                         PluginChannelMode mode) {
    const bool inputOk = descriptor.isInstrument ||
                         descriptor.mainInputChannels >=
                             (mode == PluginChannelMode::Stereo ? 2 : 1);
    const bool outputOk = descriptor.mainOutputChannels >=
                          (mode == PluginChannelMode::Stereo ? 2 : 1);
    return mode == PluginChannelMode::Auto || (inputOk && outputOk);
}

} // namespace

bool PluginReadinessReport::ready() const noexcept {
    return !stayViewer &&
           std::ranges::all_of(plugins, [](const PluginReadinessResult& value) {
               return value.status == PluginReadinessStatus::Ready;
           });
}

std::vector<PluginRequirement> collectPluginRequirements(
    const ProjectModel& project) {
    std::vector<PluginRequirement> result;
    for (const InsertModel& insert : project.masterInserts)
        appendRequirement(result, insert, false);
    for (const TrackModel& track : project.tracks) {
        appendRequirement(result, track.instrument, true);
        for (const InsertModel& insert : track.samplerFx.inserts)
            appendRequirement(result, insert, false);
        for (const InsertModel& insert : track.inserts)
            appendRequirement(result, insert, false);
        for (const ClipModel& clip : track.clips)
            for (const InsertModel& insert : clip.inserts)
                appendRequirement(result, insert, false);
    }
    std::ranges::sort(result, {}, requirementKey);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

PluginReadinessReport evaluatePluginReadiness(
    const std::vector<PluginRequirement>& requirements,
    const PluginManager& manager, std::int64_t revision) {
    PluginReadinessReport report;
    report.revision = revision;
    report.plugins.reserve(requirements.size());
    for (const PluginRequirement& requirement : requirements) {
        PluginReadinessResult result;
        result.requirement = requirement;
        const auto descriptor = manager.find(toHostFormat(requirement.format),
                                             requirement.nativeUid);
        if (!descriptor) {
            result.status = PluginReadinessStatus::Missing;
        } else if (descriptor->vendor != requirement.vendor ||
                   descriptor->version != requirement.version ||
                   descriptor->stateSchemaVersion !=
                       requirement.stateSchemaVersion ||
                   descriptor->isInstrument != requirement.instrument ||
                   !supportsChannelMode(*descriptor,
                                        requirement.channelMode)) {
            result.status = PluginReadinessStatus::VersionMismatch;
        } else {
            // Every external cache entry was instantiated, activated and
            // processed in isolated daw_scan before it could be marked valid.
            // Opaque shared state is loaded later from the hydrated asset by
            // the existing projection path, never from a participant path.
            result.status = PluginReadinessStatus::Ready;
        }
        report.plugins.push_back(std::move(result));
    }
    return report;
}

const char* pluginReadinessStatusName(PluginReadinessStatus status) noexcept {
    switch (status) {
        case PluginReadinessStatus::Ready: return "ready";
        case PluginReadinessStatus::Missing: return "missing";
        case PluginReadinessStatus::VersionMismatch:
            return "version_mismatch";
        case PluginReadinessStatus::ProbeFailed: return "probe_failed";
    }
    return "probe_failed";
}

} // namespace daw::collab
