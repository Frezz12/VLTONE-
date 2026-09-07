#pragma once

#include "model/Document.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw::recovery {

/// One immutable crash-recovery generation. Plugin APIs are called while this
/// object is assembled on the message thread; the journal worker only writes
/// the resulting bytes and never reaches into a live plugin instance.
struct RecoverySnapshot {
    struct PluginState {
        std::string fileName;
        std::vector<std::uint8_t> bytes;
    };

    ProjectModel project;
    std::vector<PluginState> pluginStates;
    // Optional immutable parts. The journal worker materializes these for the
    // serializer; it never reads the live model or a mutable cache.
    bool fragmented = false;
    std::vector<std::shared_ptr<const TrackModel>> trackParts;
    std::vector<std::shared_ptr<const PluginState>> sharedPluginStates;
};

} // namespace daw::recovery
