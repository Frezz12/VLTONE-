#include "AudioRuntime.hpp"
#include "Host/ParameterDiagnostics.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace daw {
namespace {
struct StateImportFailure : std::runtime_error { using std::runtime_error::runtime_error; };
void applyHostControls(plugins::PluginNode& node, const AudioPluginSpec& spec, bool right) {
    node.setPreferredChannelCount(right ? 1 : spec.preferredChannels);
    if (!right) node.setSlideDelivery(plugins::SlideDelivery(spec.slideDelivery), spec.slideBendRange, spec.slideReleaseReserve);
    node.setBypassed(spec.bypassed);
    node.setMix(spec.mix);
}
}

struct AudioRuntime::StagedPluginPreparation {
    using Key = std::tuple<std::string, std::string, bool>;
    AudioRuntime* owner = nullptr;
    std::shared_ptr<const engine::CompiledGraph> graph;
    engine::PrepareInfo prepare;
    std::map<Key, std::shared_ptr<plugins::PluginNode>> nodes;
    std::map<Key, std::string> unavailableReasons;
    std::map<Key, AudioPluginStateEdit> imports;
    std::map<Key, AudioPluginCheckpoint::Side> checkpoints;
    std::vector<std::shared_ptr<plugins::PluginNode>> retained;
    ~StagedPluginPreparation() {
        if (owner && owner->stagedPluginPreparation == this) owner->stagedPluginPreparation = nullptr;
    }
};

std::shared_ptr<AudioRuntime::StagedPluginPreparation> AudioRuntime::stageSessionPlugins(
    const AudioSessionSpec& session, std::span<const AudioPluginStateEdit> restores,
    std::span<const AudioPluginCheckpoint> checkpoints, AudioSessionPublication* publication,
    const std::function<void()>& preparationProgress) {
    if (stagedPluginPreparation) throw std::runtime_error("Nested plugin preparation is not supported.");
    auto staged = std::make_shared<StagedPluginPreparation>();
    staged->graph = engine.sessionGraph();
    staged->prepare = engine.prepareInfo();
    std::unordered_map<std::string, const AudioGraphSpec::Channel*> descriptions;
    for (const auto& channel : session.graph.channels) descriptions.emplace(channel.id, &channel);
    using Key = StagedPluginPreparation::Key;
    const auto existingSlot = [&](const AudioPluginChainSpec& chain, const AudioPluginSpec& spec) -> const InsertSlot* {
        const auto* live = pluginChain(chain);
        if (!live) return nullptr;
        const auto found = std::find_if(live->begin(), live->end(),
            [&](const auto& slot) { return pluginMatches(slot, spec); });
        return found == live->end() ? nullptr : &*found;
    };
    // Resolve and validate every target before a factory or native loadState
    // runs. A typo late in a batch cannot mutate an earlier healthy processor.
    std::map<Key, std::pair<const AudioPluginSpec*, const InsertSlot*>> targets;
    for (const auto& chain : session.pluginChains) for (const auto& spec : chain.slots) {
        const auto* existing = existingSlot(chain, spec);
        for (bool right : {false, true}) {
            if (right && spec.channelMode != PluginChannelMode::DualMono) continue;
            if (!targets.emplace(Key{chain.channelId, spec.id, right}, std::pair{&spec, existing}).second)
                throw std::runtime_error("Duplicate prepared plugin address: " + spec.id);
        }
    }
    for (const auto& edit : restores) {
        const Key key{edit.address.channelId, edit.address.slotId, edit.address.right};
        const auto found = targets.find(key);
        if (found == targets.end() || !staged->imports.emplace(key, edit).second)
            throw std::runtime_error("Unknown or duplicate plugin state target: " + edit.address.slotId);
        const auto& [spec, existing] = found->second;
        const auto old = existing ? (edit.address.right ? existing->rightNode : existing->node) : nullptr;
        if ((edit.address.instance && (!old || old->instanceId() != edit.address.instance)) ||
            (old && old->instance() && !edit.replaceExisting && !needsPluginReplacement(existing, *spec, edit.address.right)))
            throw std::runtime_error("Session state imports require a new or replaced plugin: " + edit.address.slotId);
        if (edit.state.state.size() > plugins::kMaxPluginStateBytes ||
            std::any_of(edit.parameters.begin(), edit.parameters.end(), [](const auto& parameter) {
                return parameter.id.empty() || !std::isfinite(parameter.value);
            })) throw std::runtime_error("Invalid plugin state import: " + edit.address.slotId);
    }
    for (const auto& checkpoint : checkpoints) {
        for (bool right : {false, true}) {
            if (right && !checkpoint.right) continue;
            const Key key{checkpoint.channelId, checkpoint.slotId, right};
            const auto found = targets.find(key);
            const auto& side = right ? *checkpoint.right : checkpoint.left;
            if (found == targets.end() || !staged->checkpoints.emplace(key, side).second)
                throw std::runtime_error("Unknown or duplicate checkpoint target: " + checkpoint.slotId);
            const auto& [spec, existing] = found->second;
            const auto old = existing ? (right ? existing->rightNode : existing->node) : nullptr;
            if (spec->uid != checkpoint.uid || spec->requiredFormat != checkpoint.format ||
                (spec->channelMode == PluginChannelMode::DualMono) != checkpoint.right.has_value() ||
                (old && old->instance() && !needsPluginReplacement(existing, *spec, right)) ||
                side.state.size() > plugins::kMaxPluginStateBytes || (!side.hasState && !side.state.empty()))
                throw std::runtime_error("Invalid unpublished checkpoint target: " + checkpoint.slotId);
            for (const auto* values : {&side.parameters, &side.pending})
                for (const auto& value : *values)
                    if (value.id.empty() || !std::isfinite(value.value))
                        throw std::runtime_error("Invalid checkpoint parameter: " + checkpoint.slotId);
            const auto imported = staged->imports.find(key);
            const auto* edit = imported != staged->imports.end() ? &imported->second
                : existing && existing->unavailableEdits[right ? 1 : 0] ? &*existing->unavailableEdits[right ? 1 : 0] : nullptr;
            if (!matchesCheckpointProjectState(side, edit))
                throw std::runtime_error("Project checkpoint requires its original state import.");
        }
    }
    const auto sidechain = [&](const AudioPluginChainSpec& chain, const AudioPluginSpec& spec) {
        if (chain.kind == AudioPluginChainSpec::Kind::MiniModules) return false;
        const auto channel = descriptions.find(chain.channelId);
        const auto* routes = chain.channelId == AudioGraphSpec::masterChannelId ? &session.graph.masterSidechains
            : channel != descriptions.end() ? &channel->second->sidechains : nullptr;
        if (!routes) return false;
        for (const auto& route : *routes) if (route.slotId == spec.id)
            return std::any_of(route.sourceChannelIds.begin(), route.sourceChannelIds.end(),
                [&](const auto& id) { return descriptions.contains(id); });
        return false;
    };
    if (preparationProgress) preparationProgress();
    for (const auto& chain : session.pluginChains) {
        for (const auto& spec : chain.slots) {
            const auto* existing = existingSlot(chain, spec);
            for (bool right : {false, true}) {
                if (right && spec.channelMode != PluginChannelMode::DualMono) continue;
                const Key key{chain.channelId, spec.id, right};
                const auto old = existing ? (right ? existing->rightNode : existing->node) : nullptr;
                const auto imported = staged->imports.find(key);
                if (!needsPluginReplacement(existing, spec, right) &&
                    (imported == staged->imports.end() || !imported->second.replaceExisting)) continue;
                if (existing && old && !old->instance()) {
                    if (!staged->imports.contains(key) && existing->unavailableEdits[right ? 1 : 0])
                        staged->imports.emplace(key, *existing->unavailableEdits[right ? 1 : 0]);
                    if (!staged->checkpoints.contains(key) && existing->unavailableStates[right ? 1 : 0])
                        staged->checkpoints.emplace(key, *existing->unavailableStates[right ? 1 : 0]);
                }

                std::shared_ptr<plugins::PluginNode> node;
                if (spec.loadPolicy != AudioPluginLoadPolicy::PlaceholderOnly) try {
                if (!right && spec.miniModule && !staged->imports.contains(key)) {
                    const auto key = (chain.channelId == AudioGraphSpec::masterChannelId ? std::string{} : chain.channelId)
                        + "\n" + spec.id;
                    const auto found = preparedMiniModules.find(key);
                    if (found != preparedMiniModules.end() && found->second != old) {
                        const auto* mini = dynamic_cast<const plugins::mini::MiniModuleInstance*>(found->second->instance());
                        if (mini && mini->definition() == *spec.miniModule && mini->mode() == spec.miniModuleMode)
                            node = found->second;
                    }
                }
                if (!node) {
                    auto instance = createConfiguredPlugin(spec, session.hosting);
                    if (!instance) throw std::runtime_error("Could not load plugin: " + spec.name);
                    node = std::make_shared<plugins::PluginNode>(spec.name + (right ? " Right" : ""), std::move(instance));
                }
                if (preparationProgress) preparationProgress();
                // Pin before native state/activation: even a rejected import
                // tears its candidate down outside the render gate.
                staged->retained.push_back(node);
                if (auto* mini = dynamic_cast<plugins::mini::MiniModuleInstance*>(node->instance())) {
                    bool audioChanged = false;
                    if (!spec.miniModule || !mini->configure(*spec.miniModule, spec.profileSeed, spec.miniModuleMode, &audioChanged))
                        throw std::runtime_error("Could not configure module: " + spec.name);
                    if (audioChanged) node->invalidatePrepare();
                }
                applyHostControls(*node, spec, right);
                if (auto* color = dynamic_cast<plugins::channel_color::ChannelColorInstance*>(node->instance()))
                    color->setProfileSeed(spec.profileSeed);
                // An explicit retry of a missing processor retains its original
                // project-relative state and immutable sample ownership.
                if (auto imported = staged->imports.find(key); imported != staged->imports.end()) {
                    auto parameters = imported->second.parameters;
                    try {
                        if (const auto restored = restorePluginNode(*node, imported->second.state, parameters, spec.id); !restored)
                            throw StateImportFailure(restored.message());
                    } catch (const std::exception& error) { throw StateImportFailure(error.what()); }
                    node->invalidatePrepare();
                } else applyStoredParameters(*node,
                    right && !spec.rightParameters.empty() ? spec.rightParameters : spec.parameters);
                if (preparationProgress) preparationProgress();
                if (const auto saved = staged->checkpoints.find(key); saved != staged->checkpoints.end()) {
                    const auto imported = staged->imports.find(key);
                    try { restoreCheckpointNode(*node, saved->second,
                        imported == staged->imports.end() ? nullptr : &imported->second); }
                    catch (const std::exception& error) { throw StateImportFailure(error.what()); }
                    node->invalidatePrepare();
                }
                if (preparationProgress) preparationProgress();
                node->setSidechainConnected(sidechain(chain, spec));
                if (!node->isPreparedFor(staged->prepare)) {
                    node->prepare(staged->prepare);
                    if (!node->isReady()) throw std::runtime_error("Could not activate plugin: " + spec.name);
                    node->markPrepared(staged->prepare);
                }
                if (!node->isReady()) throw std::runtime_error("Prepared plugin is unavailable: " + spec.name);
                } catch (const StateImportFailure&) { throw;
                } catch (const std::exception& error) {
                    if (spec.loadPolicy == AudioPluginLoadPolicy::Required) throw;
                    staged->unavailableReasons.emplace(key, error.what());
                    node.reset();
                }
                if (!node) {
                    node = std::make_shared<plugins::PluginNode>(spec.name + (right ? " Right" : ""), nullptr);
                    applyHostControls(*node, spec, right);
                    staged->unavailableReasons.try_emplace(key, spec.unavailableReason.empty()
                        ? "Plugin is unavailable: " + spec.name : spec.unavailableReason);
                }
                // Reconciliation may throw after consuming this entry. Keep an
                // owner in the staging lease so native teardown stays off the gate.
                if (std::find(staged->retained.begin(), staged->retained.end(), node) == staged->retained.end())
                    staged->retained.push_back(node);
                if (!staged->nodes.emplace(key, std::move(node)).second)
                    throw std::runtime_error("Duplicate prepared plugin address: " + spec.id);
                if (preparationProgress) preparationProgress();
            }
        }
    }
    if (publication) {
        publication->plugins.reserve(targets.size());
        publication->imported.reserve(staged->imports.size());
        for (const auto& [key, target] : targets) {
            const auto& [channel, slot, right] = key;
            const auto& [spec, existing] = target;
            const auto prepared = staged->nodes.find(key);
            const auto node = prepared != staged->nodes.end() ? prepared->second
                : existing ? (right ? existing->rightNode : existing->node) : nullptr;
            AudioPluginAddress address{channel, slot, right};
            if (node && node->instance()) {
                address.instance = node->instanceId();
                publication->plugins.push_back(address);
            }
            const auto imported = staged->imports.find(key);
            if (imported == staged->imports.end()) continue;
            // Every imported side is unpublished. Native metadata/state access
            // finishes here, before the render gate or topology commit.
            AudioPluginStateSnapshot snapshot;
            if (node && node->instance()) snapshot = snapshotPluginNode(*node, address, false);
            else {
                snapshot.address = address;
                snapshot.descriptor = spec->descriptor;
                snapshot.parameters = imported->second.parameters;
            }
            snapshot.sample.reset();
            publication->imported.push_back(std::move(snapshot));
            if (preparationProgress) preparationProgress();
        }
    }
    staged->owner = this;
    stagedPluginPreparation = staged.get();
    return staged;
}

bool AudioRuntime::stagedSessionPluginsCurrent(const std::shared_ptr<StagedPluginPreparation>& staged) const {
    return staged && stagedPluginPreparation == staged.get() && staged->graph == engine.sessionGraph() &&
           staged->prepare == engine.prepareInfo();
}

std::unique_ptr<plugins::PluginInstance> AudioRuntime::createConfiguredPlugin(
    const AudioPluginSpec& spec, const plugins::HostingConfiguration& hosting) {
    std::string error;
    auto instance = plugins::createHostedPlugin(spec.descriptor, hosting, &error);
    if (!instance && !error.empty()) throw std::runtime_error("Could not load plugin " + spec.name + ": " + error);
    if (!instance) return {};
    const auto& descriptor = instance->descriptor();
    if (descriptor.uid != spec.uid ||
        (spec.requiredFormat != plugins::Format::Unknown && descriptor.format != spec.requiredFormat) ||
        (spec.requireExactVersion && (descriptor.version != spec.requiredVersion ||
            (!spec.requiredParameterFingerprint.empty() && descriptor.parameterFingerprint != spec.requiredParameterFingerprint))))
        return {};
    if (auto* mini = dynamic_cast<plugins::mini::MiniModuleInstance*>(instance.get()))
        if (!spec.miniModule || !mini->configure(*spec.miniModule, spec.profileSeed, spec.miniModuleMode))
            return {};
    return instance;
}

bool AudioRuntime::pluginMatches(const InsertSlot& live, const AudioPluginSpec& wanted) {
    if (live.slotId != wanted.id || live.uid != wanted.uid) return false;
    // An unavailable slot records the requested identity, not a successfully
    // loaded descriptor. Retain it without retrying on unrelated graph edits.
    if (!live.node || !live.node->instance())
        return live.configuration.requiredFormat == wanted.requiredFormat &&
            live.configuration.requireExactVersion == wanted.requireExactVersion &&
            live.configuration.requiredVersion == wanted.requiredVersion &&
            live.configuration.requiredParameterFingerprint == wanted.requiredParameterFingerprint;
    const auto& descriptor = live.node->instance()->descriptor();
    if (wanted.requiredFormat != plugins::Format::Unknown && descriptor.format != wanted.requiredFormat) return false;
    return !wanted.requireExactVersion ||
        (descriptor.version == wanted.requiredVersion &&
         (wanted.requiredParameterFingerprint.empty() ||
          descriptor.parameterFingerprint == wanted.requiredParameterFingerprint));
}

bool AudioRuntime::needsPluginReplacement(const InsertSlot* existing, const AudioPluginSpec& spec, bool right) {
    const auto node = existing ? (right ? existing->rightNode : existing->node) : nullptr;
    if (!node) return true;
    if (spec.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly) return node->instance() != nullptr;
    if (!node->instance()) return spec.loadPolicy == AudioPluginLoadPolicy::Required ||
        existing->configuration.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly;
    if (const auto* mini = dynamic_cast<const plugins::mini::MiniModuleInstance*>(node->instance()))
        return !spec.miniModule || !plugins::mini::sameAudioGraph(
            mini->definition(), mini->mode(), *spec.miniModule, spec.miniModuleMode) ||
            existing->configuration.profileSeed != spec.profileSeed;
    return false;
}

bool AudioRuntime::requiresPluginPreparation(const AudioPluginAddress& address, const AudioPluginSpec& spec) const {
    const auto* slot = pluginSlot(address.channelId, address.slotId);
    if (!slot || !pluginMatches(*slot, spec)) return true;
    return needsPluginReplacement(slot, spec, address.right);
}

const std::vector<AudioRuntime::InsertSlot>* AudioRuntime::pluginChain(
    const AudioPluginChainSpec& chain) const {
    const auto found = channels.find(chain.channelId);
    if (found == channels.end()) return nullptr;
    const auto& channel = found->second;
    switch (chain.kind) {
        case AudioPluginChainSpec::Kind::Inserts: return &channel.inserts;
        case AudioPluginChainSpec::Kind::Instrument: return &channel.instrument;
        case AudioPluginChainSpec::Kind::MiniModules: return &channel.miniModules;
        case AudioPluginChainSpec::Kind::SamplerInserts: return &channel.samplerInserts;
        case AudioPluginChainSpec::Kind::ClipFx: {
            const auto clip = channel.clipFx.find(chain.clipId);
            return clip == channel.clipFx.end() ? nullptr : &clip->second.inserts;
        }
    }
    return nullptr;
}

std::vector<AudioPluginAddress> AudioRuntime::retiringPlugins(std::span<const AudioPluginChainSpec> wanted) const {
    using Kind = AudioPluginChainSpec::Kind;
    using Key = std::tuple<std::string_view, Kind, std::string_view>;
    std::map<Key, const AudioPluginChainSpec*> byChain;
    for (const auto& chain : wanted) byChain.emplace(Key{chain.channelId, chain.kind, chain.clipId}, &chain);
    std::vector<AudioPluginAddress> retired;
    const auto append = [&](const std::string& channelId, Kind kind, const std::string& clipId,
                            const auto& slots) {
        const auto chain = byChain.find(Key{channelId, kind, clipId});
        for (const auto& slot : slots) {
            if (!slot.node && !slot.rightNode) continue;
            if (chain != byChain.end() && std::any_of(chain->second->slots.begin(), chain->second->slots.end(),
                [&](const auto& spec) { return pluginMatches(slot, spec); })) continue;
            const bool right = !slot.node;
            retired.push_back({channelId, slot.slotId, right,
                (right ? slot.rightNode : slot.node)->instanceId()});
        }
    };
    for (const auto& [id, channel] : channels) {
        append(id, Kind::MiniModules, {}, channel.miniModules);
        append(id, Kind::Instrument, {}, channel.instrument);
        append(id, Kind::SamplerInserts, {}, channel.samplerInserts);
        for (const auto& [clipId, clip] : channel.clipFx) append(id, Kind::ClipFx, clipId, clip.inserts);
        append(id, Kind::Inserts, {}, channel.inserts);
    }
    return retired;
}

bool AudioRuntime::applyStoredParameters(plugins::PluginNode& node,
    std::span<const InsertParameter> values) {
    auto* instance = node.instance();
    if (!instance) return false;
    for (const auto& parameter : values) {
        const auto index = instance->parameterIndexForId(parameter.id);
        if (index < 0) continue;
        plugins::PluginEvent event;
        event.kind = plugins::PluginEvent::Kind::ParamValue;
        event.paramIndex = std::uint32_t(index);
        event.value = parameter.value;
        node.pushEvent(event);
        instance->setParameterFromHost(std::uint32_t(index), parameter.value);
        plugins::logParameterWrite("restore", instance, index, parameter.value);
    }
    return !values.empty();
}

bool AudioRuntime::reconcilePluginChain(const AudioPluginChainSpec& chain,
    const plugins::HostingConfiguration& hosting) {
    auto& channel = channels[chain.channelId];
    // Only Clip FX needs a second map entry before the common lookup can run.
    if (chain.kind == AudioPluginChainSpec::Kind::ClipFx) channel.clipFx.try_emplace(chain.clipId);
    auto* selected = const_cast<std::vector<InsertSlot>*>(pluginChain(chain));
    if (!selected) return false;
    auto& live = *selected;
    std::vector<InsertSlot> rebuilt;
    rebuilt.reserve(chain.slots.size());
    bool parametersApplied = false;
    const auto restore = [&](plugins::PluginNode& node, const auto& values) {
        parametersApplied |= applyStoredParameters(node, values);
    };
    const auto takePrepared = [&](const AudioPluginSpec& spec, bool right) {
        std::shared_ptr<plugins::PluginNode> node;
        if (!stagedPluginPreparation) return node;
        const auto found = stagedPluginPreparation->nodes.find({chain.channelId, spec.id, right});
        if (found == stagedPluginPreparation->nodes.end()) return node;
        node = std::move(found->second);
        stagedPluginPreparation->nodes.erase(found);
        const auto& parameters = right && !spec.rightParameters.empty() ? spec.rightParameters : spec.parameters;
        parametersApplied |= !parameters.empty();
        return node;
    };
    const auto retainUnavailable = [&](InsertSlot& loaded, bool right) {
        const auto side = right ? 1u : 0u;
        const auto& node = right ? loaded.rightNode : loaded.node;
        if (!node) { loaded.unavailableStates[side].reset(); loaded.unavailableEdits[side].reset();
            loaded.unavailableReasons[side].clear(); return; }
        if (stagedPluginPreparation) {
            const StagedPluginPreparation::Key key{chain.channelId, loaded.slotId, right};
            if (const auto why = stagedPluginPreparation->unavailableReasons.find(key);
                why != stagedPluginPreparation->unavailableReasons.end()) loaded.unavailableReasons[side] = why->second;
            if (const auto edit = stagedPluginPreparation->imports.find(key);
                edit != stagedPluginPreparation->imports.end()) {
                loaded.unavailableEdits[side] = edit->second;
                loaded.unavailableStates[side].reset();
            }
            if (const auto saved = stagedPluginPreparation->checkpoints.find(key);
                saved != stagedPluginPreparation->checkpoints.end() && !node->instance())
                loaded.unavailableStates[side] = saved->second;
        }
        if (node->instance()) { loaded.unavailableStates[side].reset(); loaded.unavailableReasons[side].clear(); return; }
        if (loaded.unavailableReasons[side].empty())
            loaded.unavailableReasons[side] = loaded.configuration.unavailableReason.empty()
                ? "Plugin is unavailable: " + loaded.configuration.name : loaded.configuration.unavailableReason;
    };

    for (const auto& slot : chain.slots) {
        auto existing = std::find_if(live.begin(), live.end(),
            [&](const auto& candidate) { return pluginMatches(candidate, slot); });
        InsertSlot loaded;
        if (existing != live.end()) {
            loaded = std::move(*existing);
            live.erase(existing);
        }
        bool preparedLeft = false;
        if (auto prepared = takePrepared(slot, false)) {
            if (auto* mini = dynamic_cast<plugins::mini::MiniModuleInstance*>(prepared->instance()); mini && loaded.node)
                mini->transitionFrom(loaded.node);
            loaded.node = std::move(prepared);
            loaded.unavailableEdits[0].reset();
            loaded.unavailableStates[0].reset();
            loaded.unavailableReasons[0].clear();
            loaded.slotId = slot.id;
            loaded.uid = slot.uid;
            preparedLeft = true;
        }
        if (!stagedPluginPreparation && loaded.node &&
            ((slot.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly && loaded.node->instance()) ||
             (!loaded.node->instance() && (slot.loadPolicy == AudioPluginLoadPolicy::Required ||
                (loaded.configuration.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly &&
                 slot.loadPolicy != AudioPluginLoadPolicy::PlaceholderOnly))))) loaded.node.reset();
        if (!loaded.node) {
            if (stagedPluginPreparation) throw std::runtime_error("Prepared plugin is missing: " + slot.id);
            auto instance = slot.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly
                ? nullptr : createConfiguredPlugin(slot, hosting);
            if (!instance && slot.loadPolicy == AudioPluginLoadPolicy::Required) continue;
            loaded.slotId = slot.id;
            loaded.uid = slot.uid;
            loaded.node = std::make_shared<plugins::PluginNode>(slot.name, std::move(instance));
            if (loaded.node->instance()) restore(*loaded.node, slot.parameters);
        }

        const bool unblocked = loaded.configuration.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly &&
            slot.loadPolicy != AudioPluginLoadPolicy::PlaceholderOnly;
        loaded.configuration = slot;
        loaded.hosting = hosting;
        loaded.channelMode = slot.channelMode;
        if (auto* mini = dynamic_cast<plugins::mini::MiniModuleInstance*>(loaded.node->instance())) {
            if (!slot.miniModule) continue;
            if (mini->definition() != *slot.miniModule || mini->mode() != slot.miniModuleMode) {
                if (mini->definition() == *slot.miniModule) {
                    const engine::RealtimeEngine::RenderGate gate(engine);
                    bool audioChanged = false;
                    if (!mini->configure(*slot.miniModule, slot.profileSeed, slot.miniModuleMode, &audioChanged)) continue;
                    if (audioChanged) loaded.node->invalidatePrepare();
                } else if (plugins::mini::sameAudioGraph(mini->definition(), mini->mode(),
                                                       *slot.miniModule, slot.miniModuleMode)) {
                    if (!mini->configure(*slot.miniModule, slot.profileSeed, slot.miniModuleMode)) continue;
                } else {
                    const auto key = (chain.channelId == AudioGraphSpec::masterChannelId
                        ? std::string{} : chain.channelId) + "\n" + slot.id;
                    auto prepared = preparedMiniModules.find(key);
                    std::shared_ptr<plugins::PluginNode> replacement;
                    if (prepared != preparedMiniModules.end()) replacement = prepared->second;
                    else {
                        auto instance = std::make_unique<plugins::mini::MiniModuleInstance>();
                        if (!instance->configure(*slot.miniModule, slot.profileSeed, slot.miniModuleMode)) continue;
                        replacement = std::make_shared<plugins::PluginNode>(slot.name, std::move(instance));
                    }
                    auto* next = static_cast<plugins::mini::MiniModuleInstance*>(replacement->instance());
                    next->transitionFrom(loaded.node);
                    loaded.node = std::move(replacement);
                }
            }
            if (!preparedLeft) restore(*loaded.node, slot.parameters);
        }
        if (auto* color = dynamic_cast<plugins::channel_color::ChannelColorInstance*>(loaded.node->instance())) {
            color->setProfileSeed(slot.profileSeed);
            if (!preparedLeft) restore(*loaded.node, slot.parameters);
        }
        applyHostControls(*loaded.node, slot, false);
        if (dynamic_cast<plugins::channel_color::ChannelColorInstance*>(loaded.node->instance()) &&
            !loaded.node->instance()->isActive()) loaded.node->reset();

        if (slot.channelMode == PluginChannelMode::DualMono) {
            if (auto prepared = takePrepared(slot, true)) {
                loaded.rightNode = std::move(prepared);
                loaded.unavailableEdits[1].reset(); loaded.unavailableStates[1].reset(); loaded.unavailableReasons[1].clear();
            }
            if (!stagedPluginPreparation && loaded.rightNode &&
                ((slot.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly && loaded.rightNode->instance()) ||
                 ((slot.loadPolicy == AudioPluginLoadPolicy::Required || unblocked) && !loaded.rightNode->instance()))) loaded.rightNode.reset();
            if (!loaded.rightNode) {
                if (stagedPluginPreparation) throw std::runtime_error("Prepared right plugin is missing: " + slot.id);
                auto right = slot.loadPolicy == AudioPluginLoadPolicy::PlaceholderOnly
                    ? nullptr : createConfiguredPlugin(slot, hosting);
                if (right || slot.loadPolicy != AudioPluginLoadPolicy::Required) {
                    loaded.rightNode = std::make_shared<plugins::PluginNode>(slot.name + " Right", std::move(right));
                    if (loaded.rightNode->instance())
                        restore(*loaded.rightNode, slot.rightParameters.empty() ? slot.parameters : slot.rightParameters);
                }
            }
            if (!loaded.leftSelector) {
                loaded.leftSelector = std::make_shared<engine::ChannelSelectNode>(0, slot.name + " Left Input");
                loaded.rightSelector = std::make_shared<engine::ChannelSelectNode>(1, slot.name + " Right Input");
                loaded.stereoMerge = std::make_shared<engine::StereoMergeNode>(slot.name + " Dual Mono Merge");
            }
        } else loaded.rightNode.reset();
        if (loaded.rightNode) {
            applyHostControls(*loaded.rightNode, slot, true);
        }
        retainUnavailable(loaded, false);
        retainUnavailable(loaded, true);
        rebuilt.push_back(std::move(loaded));
    }
    // The controller consumed retiringPlugins before this operation.
    // Published graphs co-own old nodes until no audio block references them.
    live = std::move(rebuilt);
    return parametersApplied;
}

} // namespace daw
