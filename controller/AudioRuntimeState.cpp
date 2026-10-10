#include "AudioRuntime.hpp"
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
    bool includeState, const std::optional<std::string>& packagedSample) {
    AudioPluginStateSnapshot out;
    out.address = address;
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto* node = pluginNode(address);
    if (node && (node->faultBypassed() || safetyStopped.load(std::memory_order_acquire))) {
        const auto saved = lastGoodPluginStates.find(node->instanceId());
        if (saved == lastGoodPluginStates.end()) {
            out.exists = true;
            out.address.instance = node->instanceId();
            if (const auto* slot = pluginSlot(address.channelId, address.slotId))
                out.descriptor = slot->configuration.descriptor;
            out.supportsState = true; // Missing checkpoint must never mean factory defaults.
            return out;
        }
        out = saved->second;
        if (const auto shared = sharedCheckpointBytes.find(node->instanceId()); includeState && shared != sharedCheckpointBytes.end())
            out.state = *shared->second;
        if (const auto edits = checkpointParameterEdits.find(node->instanceId()); edits != checkpointParameterEdits.end())
            overlayPendingParameters(out.pending, edits->second);
        if (!includeState) { out.state.clear(); out.stateCaptured = false; }
        return out;
    }
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
    AudioPluginStateSnapshot snapshot;
    try {
        snapshot = snapshotPluginNode(*node, address, includeState, packagedSample);
    } catch (const std::exception&) {
        // A failed control-thread capture must not escape a Qt timer or
        // overwrite the last valid checkpoint. Explicit renders reject it.
        snapshot.address = address; snapshot.address.instance = node->instanceId();
        snapshot.descriptor = node->descriptor();
        snapshot.exists = true; snapshot.supportsState = true;
    }
    // Packaged sampler paths are a document representation, not a native
    // checkpoint. Keep the previous native snapshot for a live replacement.
    if (includeState && !packagedSample) retainPluginSnapshot(snapshot);
    return snapshot;
}

void AudioRuntime::retainPluginSnapshot(const AudioPluginStateSnapshot& snapshot) {
    if (!snapshot.exists || (snapshot.supportsState && !snapshot.stateCaptured)) return;
    lastGoodPluginStates[snapshot.address.instance] = snapshot;
    sharedCheckpointBytes.erase(snapshot.address.instance);
    checkpointParameterEdits.erase(snapshot.address.instance);
}

void AudioRuntime::sharePluginCheckpoint(const AudioPluginAddress& address,
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    const auto* node = pluginNode(address);
    if (!node || !bytes) return;
    const auto saved = lastGoodPluginStates.find(node->instanceId());
    if (saved == lastGoodPluginStates.end() || !saved->second.stateCaptured) return;
    sharedCheckpointBytes[node->instanceId()] = std::move(bytes);
    std::vector<std::uint8_t>().swap(saved->second.state);
}

AudioPluginStateSnapshot AudioRuntime::snapshotPluginNode(plugins::PluginNode& prepared,
    const AudioPluginAddress& address, bool includeState, const std::optional<std::string>& packagedSample) {
    AudioPluginStateSnapshot out;
    out.address = address;
    auto* node = &prepared;
    auto* instance = node->instance();
    if (!instance) return out;
    out.exists = true;
    out.address.instance = node->instanceId();
    out.descriptor = instance->descriptor();
    out.supportsState = instance->supportsState();
    out.documentParametersAuthoritative =
        dynamic_cast<plugins::channel_color::ChannelColorInstance*>(instance) ||
        dynamic_cast<plugins::mini::MiniModuleInstance*>(instance);
    const auto parameters = instance->parameters();
    for (const auto& parameter : parameters) {
        const double value = instance->parameterValue(parameter.index);
        if (!parameter.id.empty() && std::isfinite(value))
            out.parameters.push_back({parameter.id, value, false});
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
        result.push_back(pluginStateSnapshot(request.address, request.includeState, request.packagedSample));
    return result;
}

audio::Result AudioRuntime::restorePluginState(const AudioPluginAddress& address,
    const AudioPluginStateRestore& state, std::vector<InsertParameter>& parameters,
    const std::function<void()>& beforeRetire) {
    auto* node = pluginNode(address);
    if (!node || !node->instance() || node->faultBypassed()) return audio::Result::fail(audio::EngineError::PluginLoadFailed,
        "Plugin state target is unavailable or belongs to another instance: " + address.slotId);
    if (state.state.empty() && state.stateFile.empty() && !state.source) {
        const engine::RealtimeEngine::RenderGate gate(engine);
        return restorePluginNode(*node, state, parameters, address.slotId);
    }
    // loadState is allowed to mutate a processor before returning false.
    // Import into a candidate so neither failure nor rollback touches the
    // published native state or its editor.
    struct Prune { AudioRuntime* owner; ~Prune() { owner->prunePluginSnapshots(); } } prune{this};
    try {
        auto* slot = pluginSlot(address.channelId, address.slotId);
        auto instance = createConfiguredPlugin(slot->configuration);
        if (!instance) return audio::Result::fail(audio::EngineError::PluginLoadFailed, "Could not create state import candidate.");
        auto fresh = std::make_shared<plugins::PluginNode>(std::string(node->name()), std::move(instance));
        auto restoredParameters = parameters;
        if (const auto restored = restorePluginNode(*fresh, state, restoredParameters, address.slotId); !restored) return restored;
        fresh->copyControlSettingsFrom(*node);
        fresh->setPreferredChannelCount(node->preferredChannelCount());
        fresh->setSidechainConnected(node->sidechainConnected());
        fresh->prepare(engine.prepareInfo());
        if (!fresh->isReady()) return audio::Result::fail(audio::EngineError::PluginLoadFailed, "Could not activate state import candidate.");
        fresh->markPrepared(engine.prepareInfo());
        auto snapshot = snapshotPluginNode(*fresh, {address.channelId, address.slotId, address.right, fresh->instanceId()}, false);
        if (snapshot.ownsSample) snapshot = snapshotPluginNode(*fresh, snapshot.address, true);
        else { snapshot.state = state.state; snapshot.stateCaptured = !state.state.empty(); }
        retainPluginSnapshot(snapshot);
        const auto result = replacePluginNodes(address.channelId, *slot,
            address.right ? nullptr : fresh, address.right ? fresh : nullptr, beforeRetire);
        if (result) parameters = std::move(restoredParameters);
        return result;
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::PluginLoadFailed, error.what());
    }
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
                parameters.push_back({parameter.id, value, false});
        }
    }
    return audio::Result::ok();
}

audio::Result AudioRuntime::capturePluginCheckpoints(std::vector<AudioPluginCheckpoint>& out) {
    out.clear();
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
            auto snapshot = pluginStateSnapshot(address);
            if (!snapshot.exists)
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
                (instance && (instance->descriptor().uid != checkpoint.uid ||
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
