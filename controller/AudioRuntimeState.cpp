#include "AudioRuntime.hpp"
#include "ProcessPluginInstance.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SlicerInstance.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace daw {
namespace {
std::optional<AudioPluginCheckpoint::Side> confirmedIsolatedCheckpoint(const plugins::PluginNode& node) {
    const auto* isolated = dynamic_cast<const plugins::ProcessPluginInstance*>(node.instance());
    if (!isolated) return std::nullopt;
    // Delivery to the host queue is not proof of completed processing. Both
    // recovery checkpoints and failed-slot Undo use this one confirmed source.
    auto recovery = isolated->recovery();
    AudioPluginCheckpoint::Side side;
    side.hasState = recovery.hasState;
    side.state = std::move(recovery.state);
    auto& parameters = side.hasState ? side.pending : side.parameters;
    parameters.reserve(recovery.parameters.size());
    for (const auto& [id, value] : recovery.parameters) parameters.push_back({id, value});
    return side;
}
} // namespace

std::vector<AudioPluginAddress> AudioRuntime::pluginAddresses() const {
    std::vector<AudioPluginAddress> result;
    for (const auto& [id, channel] : channels) {
        const auto append = [&](const auto& slots) {
            for (const auto& slot : slots) {
                if (slot.node && slot.node->instance())
                    result.push_back({id, slot.slotId, false, slot.node->instanceId()});
                if (slot.rightNode && slot.rightNode->instance())
                    result.push_back({id, slot.slotId, true, slot.rightNode->instanceId()});
            }
        };
        append(channel.instrument); append(channel.miniModules);
        append(channel.samplerInserts); append(channel.inserts);
        for (const auto& [clipId, clip] : channel.clipFx) append(clip.inserts);
    }
    return result;
}

AudioPluginStateSnapshot AudioRuntime::pluginStateSnapshot(const AudioPluginAddress& address,
    bool includeState, const std::optional<std::string>& packagedSample,
    AudioPluginSnapshotPurpose purpose) {
    if (purpose != AudioPluginSnapshotPurpose::Exact && purpose != AudioPluginSnapshotPurpose::RecoverFailed)
        throw std::invalid_argument("Invalid plugin snapshot purpose.");
    AudioPluginStateSnapshot out;
    out.address = address;
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto* node = pluginNode(address);
    if (!node || !node->instance()) {
        const auto* slot = pluginSlot(address.channelId, address.slotId);
        if (!slot || (address.instance && (!node || node->instanceId() != address.instance))) return out;
        const auto side = address.right ? 1u : 0u;
        out.address.instance = node ? node->instanceId() : 0;
        out.descriptor = slot->configuration.descriptor;
        out.parameters = address.right && !slot->configuration.rightParameters.empty()
            ? slot->configuration.rightParameters : slot->configuration.parameters;
        if (const auto& edit = slot->unavailableEdits[side]) {
            out.parameters = edit->parameters;
            out.samplePath = edit->state.sourcePath;
            out.sample = edit->state.source;
            out.ownsSample = bool(out.sample) || slot->uid == "daw.sampler" || slot->uid == "daw.slicer";
            if (includeState) out.state = edit->state.state;
        }
        if (const auto& retained = slot->unavailableStates[side]) {
            out.parameters = retained->parameters; out.pending = retained->pending;
            if (includeState && retained->hasState) out.state = retained->state;
        }
        out.stateCaptured = includeState && !out.state.empty();
        return out;
    }
    return snapshotPluginNode(*node, address, includeState, packagedSample, purpose);
}

AudioPluginStateSnapshot AudioRuntime::snapshotPluginNode(plugins::PluginNode& prepared,
    const AudioPluginAddress& address, bool includeState, const std::optional<std::string>& packagedSample,
    AudioPluginSnapshotPurpose purpose) {
    AudioPluginStateSnapshot out;
    out.address = address;
    auto* node = &prepared;
    auto* instance = node->instance();
    if (!instance) return out;
    out.exists = true;
    out.address.instance = node->instanceId();
    out.descriptor = instance->descriptor();
    out.failed = instance->hasFailed();
    out.isolated = dynamic_cast<plugins::ProcessPluginInstance*>(instance) != nullptr;
    out.supportsState = instance->supportsState();
    out.documentParametersAuthoritative =
        dynamic_cast<plugins::channel_color::ChannelColorInstance*>(instance) ||
        dynamic_cast<plugins::mini::MiniModuleInstance*>(instance);
    const auto parameters = instance->parameters();
    for (const auto& parameter : parameters) {
        const double value = instance->parameterValue(parameter.index);
        if (!parameter.id.empty() && std::isfinite(value))
            out.parameters.push_back({parameter.id, value, instance->parameterNeedsStateRestore(parameter.index)});
    }
    if (out.failed && purpose == AudioPluginSnapshotPurpose::RecoverFailed) {
        if (auto confirmed = confirmedIsolatedCheckpoint(*node)) {
            // parameterValue() above is the confirmed mirror for a failed
            // isolated instance. Only this confirmed journal overlays its
            // older opaque checkpoint; pending events may contain the fault.
            out.pending = std::move(confirmed->pending);
            if (includeState) {
                out.stateCaptured = confirmed->hasState;
                out.state = std::move(confirmed->state);
            }
            return out;
        }
    }
    for (const auto& event : node->pendingParameterEvents()) {
        if (event.paramIndex >= parameters.size() || !std::isfinite(event.value)) continue;
        const auto& id = parameters[event.paramIndex].id;
        if (id.empty()) continue;
        const auto found = std::find_if(out.pending.begin(), out.pending.end(), [&](const auto& p) { return p.id == id; });
        if (found == out.pending.end()) out.pending.push_back({id, event.value});
        else found->value = event.value;
    }
    auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(instance);
    auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(instance);
    out.ownsSample = sampler || slicer;
    if (sampler) { out.samplePath = sampler->samplePath(); out.sample = sampler->rawSample(); }
    else if (slicer) { out.samplePath = slicer->samplePath(); out.sample = slicer->rawSample(); }
    if (includeState) {
        if (packagedSample && sampler) out.stateCaptured = sampler->saveProjectState(out.state, *packagedSample);
        else if (packagedSample && slicer) out.stateCaptured = slicer->saveProjectState(out.state, *packagedSample);
        else out.stateCaptured = instance->saveState(out.state);
        if (!out.stateCaptured || out.state.size() > plugins::kMaxPluginStateBytes) {
            out.state.clear(); out.stateCaptured = false;
        }
    }
    return out;
}

std::vector<AudioPluginStateSnapshot> AudioRuntime::pluginStateSnapshots(
    std::span<const AudioPluginStateRequest> requests) {
    const engine::RealtimeEngine::RenderGate gate(engine);
    std::vector<AudioPluginStateSnapshot> result;
    result.reserve(requests.size());
    for (const auto& request : requests)
        result.push_back(pluginStateSnapshot(request.address, request.includeState, request.packagedSample, request.purpose));
    return result;
}

audio::Result AudioRuntime::restorePluginState(const AudioPluginAddress& address,
    const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters) {
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto* node = pluginNode(address);
    if (!node || !node->instance()) return audio::Result::fail(audio::EngineError::PluginLoadFailed,
        "Plugin state target is unavailable or belongs to another instance: " + address.slotId);
    return restorePluginNode(*node, state, parameters, address.slotId);
}

audio::Result AudioRuntime::restorePluginNode(plugins::PluginNode& node,
    const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters,
    std::string_view slotId) {
    auto* instance = node.instance();
    if (!instance || state.state.size() > plugins::kMaxPluginStateBytes ||
        std::any_of(parameters.begin(), parameters.end(), [](const auto& parameter) {
            return parameter.id.empty() || !std::isfinite(parameter.value);
        })) return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid plugin state import.");
    auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(instance);
    auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(instance);
    bool restored = false;
    if (!state.state.empty()) {
        if (sampler) {
            if (state.source) sampler->adoptSample(state.sourcePath, state.source);
            restored = sampler->loadProjectState(state.state, state.contentDirectory);
            if (!restored || (!sampler->samplePath().empty() && !sampler->rawSample())) {
                if (!state.tolerateErrors)
                    return audio::Result::fail(audio::EngineError::FileNotFound,
                        "Sampler state or embedded Content is missing or unreadable for slot " + std::string(slotId));
                restored = false;
            }
        } else if (slicer) {
            if (state.source) slicer->adoptSample(state.sourcePath, state.source);
            restored = slicer->loadProjectState(state.state, state.contentDirectory);
            if ((!restored || (!slicer->samplePath().empty() && !slicer->rawSample())) && !state.tolerateErrors)
                return audio::Result::fail(audio::EngineError::FileNotFound,
                    "Slicer source or state is missing for slot " + std::string(slotId));
        } else restored = instance->loadState(state.state);
    } else if (!state.stateFile.empty() && !state.tolerateErrors && (sampler || slicer)) {
        return audio::Result::fail(audio::EngineError::FileNotFound,
            "Sampler state file is missing or unreadable: " + state.stateFile);
    }
    if (!state.state.empty() && !restored && !state.tolerateErrors)
        return audio::Result::fail(audio::EngineError::PluginLoadFailed,
            "Cannot restore plugin state: " + instance->descriptor().name);
    const bool overridesOnly = restored && !state.tolerateErrors && !state.applyAllParameters &&
        !dynamic_cast<plugins::channel_color::ChannelColorInstance*>(instance) &&
        !dynamic_cast<plugins::mini::MiniModuleInstance*>(instance);
    if (restored || state.clearPending) {
        node.discardPendingEvents();
        if (overridesOnly) std::erase_if(parameters, [](const auto& p) { return !p.restoreAfterState; });
    }
    applyStoredParameters(node, parameters);
    if (overridesOnly) {
        parameters.clear();
        for (const auto& parameter : instance->parameters()) {
            const auto value = instance->parameterValue(parameter.index);
            if (!parameter.id.empty() && std::isfinite(value))
                parameters.push_back({parameter.id, value, instance->parameterNeedsStateRestore(parameter.index)});
        }
    }
    return audio::Result::ok();
}

audio::Result AudioRuntime::capturePluginCheckpoints(std::vector<AudioPluginCheckpoint>& out,
    AudioPluginCheckpointPurpose purpose) {
    out.clear();
    if (purpose != AudioPluginCheckpointPurpose::Exact && purpose != AudioPluginCheckpointPurpose::Recovery)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid plugin checkpoint purpose.");
    try {
        const engine::RealtimeEngine::RenderGate gate(engine);
        std::vector<AudioPluginCheckpoint> captured;
        const auto capture = [&](const InsertSlot& slot, const AudioPluginAddress& address) {
            const auto& node = address.right ? slot.rightNode : slot.node;
            if (node && !node->instance() && slot.configuration.loadPolicy != AudioPluginLoadPolicy::Required) {
                const auto index = address.right ? 1u : 0u;
                if (slot.unavailableStates[index]) return *slot.unavailableStates[index];
                AudioPluginCheckpoint::Side side;
                if (slot.unavailableEdits[index]) {
                    const auto& edit = *slot.unavailableEdits[index];
                    side.projectState = true;
                    side.hasState = !edit.state.state.empty();
                    side.state = edit.state.state;
                    side.parameters = edit.parameters;
                } else {
                    side.parameters = address.right && !slot.configuration.rightParameters.empty()
                        ? slot.configuration.rightParameters : slot.configuration.parameters;
                }
                return side;
            }
            if (purpose == AudioPluginCheckpointPurpose::Recovery && node)
                if (auto confirmed = confirmedIsolatedCheckpoint(*node)) return std::move(*confirmed);
            auto snapshot = pluginStateSnapshot(address);
            if (!snapshot.exists || snapshot.failed)
                throw std::runtime_error("cannot checkpoint an unavailable plugin");
            AudioPluginCheckpoint::Side side;
            side.hasState = snapshot.supportsState;
            if (side.hasState) {
                if (!snapshot.stateCaptured)
                    throw std::runtime_error("cannot checkpoint plugin state: " + snapshot.descriptor.name);
                side.state = std::move(snapshot.state);
            } else {
                side.parameters = std::move(snapshot.parameters);
            }
            side.pending = std::move(snapshot.pending);
            return side;
        };
        for (const auto& [id, channel] : channels) {
            const auto append = [&](const auto& slots) {
                for (const auto& slot : slots) {
                    if (!slot.node) continue;
                    AudioPluginCheckpoint checkpoint;
                    checkpoint.channelId = id;
                    checkpoint.slotId = slot.slotId;
                    checkpoint.uid = slot.uid;
                    checkpoint.format = slot.configuration.requiredFormat;
                    checkpoint.left = capture(slot, {id, slot.slotId});
                    if (slot.channelMode == PluginChannelMode::DualMono) checkpoint.right = capture(slot, {id, slot.slotId, true});
                    captured.push_back(std::move(checkpoint));
                }
            };
            append(channel.instrument); append(channel.miniModules);
            append(channel.samplerInserts); append(channel.inserts);
            for (const auto& [clipId, clip] : channel.clipFx) append(clip.inserts);
        }
        std::sort(captured.begin(), captured.end(), [](const auto& left, const auto& right) {
            return std::tie(left.channelId, left.slotId) < std::tie(right.channelId, right.slotId);
        });
        out = std::move(captured);
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

bool AudioRuntime::matchesCheckpointProjectState(const AudioPluginCheckpoint::Side& side,
    const AudioPluginStateEdit* projectState) {
    return !side.projectState || (projectState && side.state == projectState->state.state &&
        std::equal(side.parameters.begin(), side.parameters.end(), projectState->parameters.begin(), projectState->parameters.end(),
            [](const auto& a, const auto& b) {
                return a.id == b.id && a.value == b.value && a.restoreAfterState == b.restoreAfterState;
            }));
}

void AudioRuntime::restoreCheckpointNode(plugins::PluginNode& node,
    const AudioPluginCheckpoint::Side& side, const AudioPluginStateEdit* projectState) {
    if (!matchesCheckpointProjectState(side, projectState))
        throw std::runtime_error("Project checkpoint requires its original state import.");
    if (!side.projectState) {
        if (side.hasState && !node.instance()->loadState(side.state))
            throw std::runtime_error("plugin rejected checkpoint: " + std::string(node.name()));
        node.discardPendingEvents();
        if (!side.hasState) applyStoredParameters(node, side.parameters);
    }
    applyStoredParameters(node, side.pending);
    node.invalidatePrepare();
}

audio::Result AudioRuntime::restorePluginCheckpoints(std::span<const AudioPluginCheckpoint> checkpoints) {
    try {
        const engine::RealtimeEngine::RenderGate gate(engine);
        const auto published = engine.sessionGraph();
        std::unordered_set<const plugins::PluginNode*> targets;
        // Validate the whole transaction before any native mutation. Native
        // loadState can fail after mutating its processor, so restore is only
        // permitted on nodes absent from the published graph. A failed import
        // is discarded by its caller without ever making partial state audible.
        const auto validate = [&](const InsertSlot& slot, bool right,
                                  const AudioPluginCheckpoint& checkpoint,
                                  const AudioPluginCheckpoint::Side& side) {
            const auto& node = right ? slot.rightNode : slot.node;
            const auto* instance = node ? node->instance() : nullptr;
            if (!node || slot.uid != checkpoint.uid || slot.configuration.requiredFormat != checkpoint.format ||
                (!instance && slot.configuration.loadPolicy == AudioPluginLoadPolicy::Required) ||
                (instance && (instance->hasFailed() || instance->descriptor().uid != checkpoint.uid ||
                              instance->descriptor().format != checkpoint.format)) ||
                side.state.size() > plugins::kMaxPluginStateBytes ||
                (!side.hasState && !side.state.empty()) || !targets.insert(node.get()).second)
                throw std::runtime_error("invalid plugin checkpoint target: " + checkpoint.slotId);
            if (published && std::any_of(published->nodes.begin(), published->nodes.end(),
                    [&](const auto& entry) { return entry.node == node.get(); }))
                throw std::runtime_error("plugin checkpoints require an unpublished runtime generation");
            const auto& imported = slot.unavailableEdits[right ? 1u : 0u];
            if (instance && !matchesCheckpointProjectState(side, imported ? &*imported : nullptr))
                throw std::runtime_error("Project checkpoint requires its original state import.");
            for (const auto* values : {&side.parameters, &side.pending})
                for (const auto& value : *values)
                    if (value.id.empty() || !std::isfinite(value.value))
                        throw std::runtime_error("invalid checkpoint parameter");
        };
        for (const auto& checkpoint : checkpoints) {
            const auto* slot = pluginSlot(checkpoint.channelId, checkpoint.slotId);
            if (!slot || (slot->channelMode == PluginChannelMode::DualMono) != checkpoint.right.has_value())
                throw std::runtime_error("checkpoint slot no longer exists: " + checkpoint.slotId);
            validate(*slot, false, checkpoint, checkpoint.left);
            if (checkpoint.right) validate(*slot, true, checkpoint, *checkpoint.right);
        }
        const auto restore = [&](InsertSlot& slot, bool right,
                                 const AudioPluginCheckpoint::Side& side) {
            const auto& node = right ? slot.rightNode : slot.node;
            if (!node->instance()) {
                // Missing processors retain the original bytes verbatim. They
                // remain unavailable; this is storage, never successful DSP load.
                slot.unavailableStates[right ? 1u : 0u] = side;
                return;
            }
            const auto& imported = slot.unavailableEdits[right ? 1u : 0u];
            restoreCheckpointNode(*node, side, imported ? &*imported : nullptr);
        };
        for (const auto& checkpoint : checkpoints) {
            auto* slot = pluginSlot(checkpoint.channelId, checkpoint.slotId);
            restore(*slot, false, checkpoint.left);
            if (checkpoint.right) restore(*slot, true, *checkpoint.right);
        }
        return audio::Result::ok();
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

} // namespace daw
