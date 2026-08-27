#pragma once

#include "model/Document.hpp"

#include <cstdint>
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
};

} // namespace daw::recovery
