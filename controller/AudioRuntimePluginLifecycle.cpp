#include "AudioRuntime.hpp"

namespace daw {
AudioPluginRuntimeStatus AudioRuntime::pluginRuntimeStatus(const std::string& channelId, const std::string& slotId) const {
    const auto* live = pluginSlot(channelId, slotId);
    if (!live || !live->node) return {};
    if (live->node->faultBypassed()) {
        AudioPluginRuntimeStatus status{AudioPluginRuntimeState::Faulted, "Plugin processing failed.", true};
        for (bool right : {false, true}) {
            const auto& node = right ? live->rightNode : live->node;
            if (!node || node->fault() == plugins::PluginNode::Fault::None) continue;
            status.address = {channelId, slotId, right, node->instanceId()};
            if (node->fault() == plugins::PluginNode::Fault::NonFiniteOutput)
                status.detail = "Plugin produced a non-finite audio signal.";
            const auto saved = lastGoodPluginStates.find(node->instanceId());
            status.canRetry &= saved != lastGoodPluginStates.end() &&
                (!saved->second.supportsState || saved->second.stateCaptured);
        }
        if (!status.canRetry) status.detail += " No valid recovery state is available.";
        return status;
    }
    for (std::size_t side = 0; side < (live->channelMode == PluginChannelMode::DualMono ? 2u : 1u); ++side) {
        const auto& node = side ? live->rightNode : live->node;
        if (!node || !node->instance()) return {AudioPluginRuntimeState::Missing, live->unavailableReasons[side]};
    }
    return {AudioPluginRuntimeState::Local, {}};
}

bool AudioRuntime::hasPluginFault(const AudioPluginAddress& address) const {
    const auto* node = pluginNode(address);
    return node && node->faultBypassed();
}

std::vector<AudioPluginRuntimeStatus> AudioRuntime::pluginFaults() const {
    std::vector<AudioPluginRuntimeStatus> faults;
    for (const auto& address : pluginAddresses()) {
        if (address.right || !hasPluginFault(address)) continue;
        faults.push_back(pluginRuntimeStatus(address.channelId, address.slotId));
    }
    return faults;
}

void AudioRuntime::refreshMasterSafetyMute() {
    bool failed = safetyStopped;
    const auto master = channels.find(AudioGraphSpec::masterChannelId);
    if (master != channels.end()) {
        for (const auto* chain : {&master->second.inserts, &master->second.miniModules})
            for (const auto& slot : *chain) failed |= slot.node && slot.node->faultBypassed();
    }
    engine.outputSafetyMute().store(failed, std::memory_order_release);
}

void AudioRuntime::stopForFailedRollback() {
    safetyStopped = true;
    engine.outputSafetyMute().store(true, std::memory_order_release);
    engine.transport().pause();
}

void AudioRuntime::bindPluginSafety(const std::string& channelId, InsertSlot& slot) {
    if (!slot.node) return;
    auto group = slot.node->faultGroup();
    group->sides.store((slot.node->fault() != plugins::PluginNode::Fault::None ? 1u : 0u) |
        (slot.rightNode && slot.rightNode->fault() != plugins::PluginNode::Fault::None ? 2u : 0u));
    const auto mute = channelId == AudioGraphSpec::masterChannelId
        ? engine.outputSafetyLatch() : std::shared_ptr<std::atomic<bool>>{};
    slot.node->setFaultGroup(group, 0, mute);
    if (slot.rightNode) slot.rightNode->setFaultGroup(group, 1, mute);
    if (slot.stereoMerge) slot.stereoMerge->setSafetyFallback(
        std::shared_ptr<std::atomic<unsigned>>(group, &group->sides), slot.node->isSource());
}

audio::Result AudioRuntime::recoverPlugin(const AudioPluginAddress& address,
    const std::function<void()>& beforeRetire) {
    if (safetyStopped.load(std::memory_order_acquire))
        return audio::Result::fail(audio::EngineError::AudioThreadError, "The audio engine is stopped. Save and reopen the project.");
    struct Prune { AudioRuntime* owner; ~Prune() { owner->prunePluginSnapshots(); } } prune{this};
    auto* slot = pluginSlot(address.channelId, address.slotId);
    auto* target = pluginNode(address);
    if (!slot || !target || !address.instance || !target->faultBypassed())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "The failed plugin instance no longer exists.");
    if (engine.transport().isPlaying() || hasActiveCaptures() || auditionRuntime || auditionDriven)
        return audio::Result::fail(audio::EngineError::EngineRunning, "Stop playback, recording and audition before restoring the plugin.");
    try {
        std::shared_ptr<plugins::PluginNode> replacements[2];
        for (bool right : {false, true}) {
            const auto& old = right ? slot->rightNode : slot->node;
            if (!old || old->fault() == plugins::PluginNode::Fault::None) continue;
            const auto snapshot = pluginStateSnapshot({address.channelId, address.slotId, right, old->instanceId()}, true);
            if (!snapshot.exists || (snapshot.supportsState && !snapshot.stateCaptured))
                return audio::Result::fail(audio::EngineError::PluginLoadFailed, "No valid plugin recovery state is available.");
            auto instance = createConfiguredPlugin(slot->configuration);
            if (!instance) return audio::Result::fail(audio::EngineError::PluginLoadFailed, "Could not create replacement plugin.");
            auto fresh = std::make_shared<plugins::PluginNode>(std::string(old->name()), std::move(instance));
            AudioPluginCheckpoint::Side checkpoint;
            checkpoint.hasState = snapshot.supportsState;
            checkpoint.state = snapshot.state;
            checkpoint.parameters = snapshot.parameters;
            checkpoint.pending = snapshot.pending;
            restoreCheckpointNode(*fresh, checkpoint);
            fresh->copyControlSettingsFrom(*old);
            fresh->setPreferredChannelCount(old->preferredChannelCount());
            fresh->setSidechainConnected(old->sidechainConnected());
            fresh->prepare(engine.prepareInfo());
            if (!fresh->isReady()) return audio::Result::fail(audio::EngineError::PluginLoadFailed, "Could not activate replacement plugin.");
            fresh->markPrepared(engine.prepareInfo());
            auto retained = snapshot;
            retained.address.instance = fresh->instanceId();
            retainPluginSnapshot(retained);
            replacements[right ? 1 : 0] = std::move(fresh);
        }
        return replacePluginNodes(address.channelId, *slot,
            std::move(replacements[0]), std::move(replacements[1]), beforeRetire);
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::PluginLoadFailed, error.what());
    }
}

audio::Result AudioRuntime::replacePluginNodes(const std::string& channelId, InsertSlot& slot,
    std::shared_ptr<plugins::PluginNode> left, std::shared_ptr<plugins::PluginNode> right,
    const std::function<void()>& beforeRetire) {
    if (!hasPublishedGraph)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "No published plugin graph.");
    const engine::RealtimeEngine::RenderGate gate(engine);
    const auto oldLeft = slot.node, oldRight = slot.rightNode;
    auto previousGraph = engine.graph();
    const auto previousPublication = routingGraph();
    const auto rollback = [&] {
        if (routingGraph() != previousPublication) return;
        slot.node = oldLeft; slot.rightNode = oldRight;
        engine.graph() = std::move(previousGraph);
    };
    try {
        auto next = publishedGraph;
        const auto replace = [&](const auto& old, const auto& fresh) {
            if (!fresh || !old) return;
            fresh->copyControlSettingsFrom(*old);
            fresh->setPreferredChannelCount(old->preferredChannelCount());
            fresh->setSidechainConnected(old->sidechainConnected());
            // Handles from an intervening failed assembly are not committed
            // topology. Dormant recording Clip FX may have no published node.
            for (engine::NodeId id = 0; id < next.nodeCount(); ++id)
                if (next.node(id) == old.get()) (void)next.replaceNode(id, fresh);
        };
        replace(oldLeft, left); replace(oldRight, right);
        if (left) slot.node = std::move(left);
        if (right) slot.rightNode = std::move(right);
        applyPluginAutomation(channels.at(channelId));
        engine.graph() = std::move(next);
        // Candidates are already prepared. Re-preparing every healthy node
        // would erase tails and needlessly call unrelated vendor code.
        const auto result = commitGraph();
        if (!result) rollback();
        else {
            bindPluginSafety(channelId, slot);
            applyPluginAutomation(channels.at(channelId));
            refreshMasterSafetyMute();
            if (beforeRetire) {
                retiringEditorNodes.emplace(oldLeft->instanceId(), oldLeft);
                if (oldRight) retiringEditorNodes.emplace(oldRight->instanceId(), oldRight);
                beforeRetire();
                retiringEditorNodes.clear();
            }
        }
        return result;
    } catch (const std::exception& error) {
        retiringEditorNodes.clear();
        if (routingGraph() != previousPublication) stopForFailedRollback();
        rollback();
        return audio::Result::fail(audio::EngineError::InvalidArgument, error.what());
    } catch (...) {
        retiringEditorNodes.clear();
        if (routingGraph() != previousPublication) stopForFailedRollback();
        rollback();
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Plugin replacement failed.");
    }
}
}
