#include "AudioRuntime.hpp"
#include "ProcessPluginInstance.hpp"

namespace daw {
namespace {
plugins::ProcessPluginInstance* remote(const std::shared_ptr<plugins::PluginNode>& node) {
    return node ? dynamic_cast<plugins::ProcessPluginInstance*>(node->instance()) : nullptr;
}
bool failed(const std::shared_ptr<plugins::PluginNode>& node) {
    const auto* instance = remote(node);
    return instance && (!node->isReady() || instance->hasFailed());
}
}

AudioPluginRuntimeStatus AudioRuntime::pluginRuntimeStatus(
    const std::string& channelId, const std::string& slotId) const {
    const auto* live = pluginSlot(channelId, slotId);
    if (!live || !live->node) return {};
    for (std::size_t side = 0; side < (live->channelMode == PluginChannelMode::DualMono ? 2u : 1u); ++side) {
        const auto& node = side ? live->rightNode : live->node;
        if (!node || !node->instance())
            return {AudioPluginRuntimeState::Missing, live->unavailableReasons[side]};
    }
    for (const auto& pending : pluginRecoveries)
        if (pending.channelId == channelId && pending.slotId == slotId &&
            pending.originalLeft == live->node && pending.originalRight == live->rightNode)
            return {AudioPluginRuntimeState::Restarting, {}};
    for (const auto& node : {live->node, live->rightNode})
        if (failed(node)) return {AudioPluginRuntimeState::Failed, remote(node)->error()};
    return {remote(live->node) ? AudioPluginRuntimeState::Running : AudioPluginRuntimeState::Local, {}};
}

bool AudioRuntime::restartPlugin(const std::string& channelId, const std::string& slotId) {
    if (!hasPublishedGraph || pluginRuntimeStatus(channelId, slotId).state != AudioPluginRuntimeState::Failed)
        return false;
    auto* live = pluginSlot(channelId, slotId);
    if (!live) return false;
    PluginRecovery pending;
    pending.channelId = channelId; pending.slotId = slotId;
    pending.originalLeft = live->node; pending.originalRight = live->rightNode;
    pending.prepare = engine.prepareInfo();
    struct Side {
        std::optional<plugins::ProcessPluginInstance::Recovery> snapshot;
        std::string name;
        std::uint16_t channels = 2;
        bool sidechain = false;
    };
    const auto capture = [](const std::shared_ptr<plugins::PluginNode>& node) {
        Side side;
        if (failed(node)) {
            side.snapshot = remote(node)->recovery();
            side.name = node->name(); side.channels = node->preferredChannelCount();
            side.sidechain = node->sidechainConnected();
        }
        return side;
    };
    auto left = capture(live->node), right = capture(live->rightNode);
    const auto prepare = pending.prepare;
    try {
        pluginRecoveries.reserve(pluginRecoveries.size() + 1);
        pending.result = std::async(std::launch::async,
            [left = std::move(left), right = std::move(right), prepare] {
                const auto build = [&](const Side& side) -> std::shared_ptr<plugins::PluginNode> {
                    if (!side.snapshot) return {};
                    auto instance = plugins::ProcessPluginInstance::recover(*side.snapshot);
                    if (!instance) return {};
                    auto node = std::make_shared<plugins::PluginNode>(side.name, std::move(instance));
                    node->setPreferredChannelCount(side.channels);
                    node->setSidechainConnected(side.sidechain);
                    node->prepare(prepare);
                    if (!node->isReady()) return {};
                    node->markPrepared(prepare);
                    return node;
                };
                return PluginRecovery::Nodes{build(left), build(right)};
            });
        pluginRecoveries.push_back(std::move(pending));
    } catch (const std::exception&) { return false; }
    return true;
}

audio::Result AudioRuntime::replacePluginNodes(const std::string& channelId, InsertSlot& slot,
    std::shared_ptr<plugins::PluginNode> left, std::shared_ptr<plugins::PluginNode> right) {
    if (!hasPublishedGraph)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "No published plugin graph.");
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
        const auto result = commitGraph(true);
        if (!result) rollback();
        return result;
    } catch (const std::exception& error) {
        rollback();
        return audio::Result::fail(audio::EngineError::InvalidArgument, error.what());
    } catch (...) {
        rollback();
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Plugin replacement failed.");
    }
}

void AudioRuntime::refreshIsolatedPlugins() {
    std::vector<std::weak_ptr<plugins::PluginNode>> nodes;
    std::unordered_set<const plugins::PluginNode*> live;
    const auto collect = [&](const InsertSlot& slot) {
        for (const auto& node : {slot.node, slot.rightNode}) if (remote(node)) {
            nodes.push_back(node); live.insert(node.get());
        }
    };
    for (const auto& [id, channel] : channels) {
        for (const auto& slot : channel.instrument) collect(slot);
        for (const auto& slot : channel.inserts) collect(slot);
        for (const auto& slot : channel.miniModules) collect(slot);
        for (const auto& slot : channel.samplerInserts) collect(slot);
        for (const auto& [clipId, clip] : channel.clipFx)
            for (const auto& slot : clip.inserts) collect(slot);
    }
    isolatedNodes = std::move(nodes);
    std::erase_if(reportedPluginFailures, [&](const auto* node) { return !live.contains(node); });
}

bool AudioRuntime::pumpIsolatedPlugins() {
    bool changed = false;
    for (const auto& weak : isolatedNodes) if (const auto node = weak.lock()) {
        auto* instance = remote(node);
        if (instance->hasMainThreadWork()) plugins::PluginMainThreadWork::request();
        if (!instance->checkHealth() && reportedPluginFailures.insert(node.get()).second) {
            // A short barrier only drains a block already in flight. Plugin
            // loading/activation below never holds the live render gate.
            const engine::RealtimeEngine::RenderGate gate(engine);
            (void)instance->service();
            changed = true;
        }
    }
    for (auto it = pluginRecoveries.begin(); it != pluginRecoveries.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        PluginRecovery::Nodes replacement;
        try { replacement = it->result.get(); } catch (...) { /* keep failed slot retryable */ }
        auto* live = pluginSlot(it->channelId, it->slotId);
        const auto current = engine.prepareInfo();
        const auto compatible = [](const auto& old, const auto& fresh) {
            return !fresh || (old && old->preferredChannelCount() == fresh->preferredChannelCount() &&
                             old->sidechainConnected() == fresh->sidechainConnected());
        };
        if (live && live->node == it->originalLeft && live->rightNode == it->originalRight &&
            current == it->prepare && compatible(live->node, replacement.first) &&
            compatible(live->rightNode, replacement.second) &&
            (!failed(live->node) || replacement.first) &&
            (!failed(live->rightNode) || replacement.second) &&
            (replacement.first || replacement.second)) {
            (void)replacePluginNodes(it->channelId, *live,
                std::move(replacement.first), std::move(replacement.second));
        }
        it = pluginRecoveries.erase(it);
        changed = true;
    }
    return changed;
}
}
