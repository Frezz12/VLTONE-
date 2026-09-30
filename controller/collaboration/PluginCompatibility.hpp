#pragma once

#include "model/Document.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {
class PluginManager;
}
namespace daw::plugins { struct PluginDescriptor; }

namespace daw::collab {

enum class PluginReadinessStatus : std::uint8_t {
    Ready,
    Missing,
    VersionMismatch,
    ProbeFailed,
};

/// Exact shared product/state requirement. It intentionally has no module
/// path, binary hash, state filename or editor-window geometry.
struct PluginRequirement {
    PluginFormat format = PluginFormat::None;
    std::string nativeUid;
    std::string vendor;
    std::string version;
    int stateSchemaVersion = 0;
    bool instrument = false;
    PluginChannelMode channelMode = PluginChannelMode::Auto;
    std::string parameterFingerprint;

    friend bool operator==(const PluginRequirement&,
                           const PluginRequirement&) = default;
};

struct PluginReadinessResult {
    PluginRequirement requirement;
    PluginReadinessStatus status = PluginReadinessStatus::Missing;
    std::string buildHmac;
};

struct PluginReadinessReport {
    std::int64_t revision = 1;
    bool stayViewer = false;
    std::vector<PluginReadinessResult> plugins;

    bool ready() const noexcept;
};

std::vector<PluginRequirement> collectPluginRequirements(
    const ProjectModel& project);
/// Cached external entries without a measured parameter schema are omitted;
/// callers can offer a rescan instead of claiming an unverified common plugin.
std::vector<PluginRequirement> collectPluginInventory(const PluginManager& manager);
bool pluginSatisfiesRequirement(const plugins::PluginDescriptor& descriptor,
                               const PluginRequirement& requirement);
PluginReadinessReport evaluatePluginReadiness(
    const std::vector<PluginRequirement>& requirements,
    const PluginManager& manager, std::int64_t revision = 1);

const char* pluginReadinessStatusName(PluginReadinessStatus status) noexcept;

} // namespace daw::collab
