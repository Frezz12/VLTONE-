#pragma once

#include "model/Document.hpp"
#include "Host/PluginTypes.hpp"
#include "PluginReadout.hpp"
#include "Audio/SampleBuffer.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <unordered_set>

namespace daw {

// Clipboard/Undo preserve healthy pending edits; a failed isolated side may
// instead expose its last completed checkpoint and confirmed parameter journal.
enum class AudioPluginSnapshotPurpose { Exact, RecoverFailed };

struct AudioPluginStateRequest {
    AudioPluginAddress address;
    bool includeState = true;
    std::optional<std::string> packagedSample;
    AudioPluginSnapshotPurpose purpose = AudioPluginSnapshotPurpose::Exact;
};

struct AudioPluginStateRestore {
    std::vector<std::uint8_t> state;
    std::string stateFile, contentDirectory, sourcePath;
    std::shared_ptr<const engine::SampleBuffer> source;
    bool tolerateErrors = false;
    bool applyAllParameters = false;
    bool clearPending = false;
};

/// One state import attached to an unpublished session edit. The runtime
/// validates the whole address set before touching any newly prepared side.
/// Explicit state replacement prepares a fresh side without mutating the
/// healthy processor; its old instance ID can guard a delayed editor command.
struct AudioPluginStateEdit {
    AudioPluginAddress address;
    AudioPluginStateRestore state;
    std::vector<InsertParameter> parameters;
    bool replaceExisting = false;
};

enum class AudioPluginRuntimeState { Local, Running, Failed, Restarting, Missing };
struct AudioPluginRuntimeStatus {
    AudioPluginRuntimeState state = AudioPluginRuntimeState::Missing;
    std::string detail;
};

// A single native-side snapshot. Recovery can retain an older opaque chunk
// when capture fails; an explicit export can reject that same failure. That
// document policy remains with the caller, while native access stays here.
struct AudioPluginStateSnapshot {
    AudioPluginAddress address;
    plugins::PluginDescriptor descriptor;
    bool exists = false, failed = false, isolated = false;
    bool supportsState = false, stateCaptured = false;
    bool documentParametersAuthoritative = false;
    bool ownsSample = false;
    std::string samplePath;
    std::shared_ptr<const engine::SampleBuffer> sample;
    std::vector<std::uint8_t> state;
    std::vector<InsertParameter> parameters, pending;
};

/// Prepared before a graph becomes audible. Imports carry canonical parameter
/// mirrors, not native chunks or PCM; identities cover all available sides.
struct AudioSessionPublication {
    std::vector<AudioPluginStateSnapshot> imported;
    std::vector<AudioPluginAddress> plugins;
};

inline void appendMissingParameters(std::vector<InsertParameter>& destination,
                                   std::span<const InsertParameter> source) {
    std::unordered_set<std::string> ids;
    ids.reserve(destination.size() + source.size());
    for (const auto& value : destination) ids.insert(value.id);
    for (const auto& value : source)
        if (!value.id.empty() && ids.insert(value.id).second) destination.push_back(value);
}

/// Edits still queued for DSP must override the older opaque state on restore.
inline void overlayPendingParameters(std::vector<InsertParameter>& destination,
                                     std::span<const InsertParameter> pending) {
    for (const auto& edit : pending) {
        if (edit.id.empty()) continue;
        auto found = std::find_if(destination.begin(), destination.end(),
            [&](const auto& value) { return value.id == edit.id; });
        if (found == destination.end()) destination.push_back({edit.id, edit.value, true});
        else { found->value = edit.value; found->restoreAfterState = true; }
    }
}

// Exact snapshots retain edits queued for the next audio block. Recovery only
// retains isolated-plugin values confirmed by completed processing.
enum class AudioPluginCheckpointPurpose { Exact, Recovery };

struct AudioPluginCheckpoint {
    struct Side {
        bool hasState = false;
        std::vector<std::uint8_t> state;
        // Only parameter-only processors replay the full mirror. A native
        // component state is authoritative; pending host edits overlay it.
        std::vector<InsertParameter> parameters, pending;
        // Original project restore bytes (possibly packaged sampler paths)
        // require the retained StateEdit before pending host edits are replayed.
        // They must never be passed to native loadState as a fresh checkpoint.
        bool projectState = false;
    };
    std::string channelId, slotId, uid;
    plugins::Format format = plugins::Format::Unknown;
    Side left;
    std::optional<Side> right;
};

} // namespace daw
