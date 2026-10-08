#include "AudioRuntime.hpp"
#include "Internal/PitchCorrectorInstance.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

bool AudioRuntime::advancePluginEdits() {
    // A stopped diagnostic/preview runtime has no device to consume its host
    // event ring. Advance one bounded block without tapping a recording input.
    if (liveDeviceAllowed || auditionDriven || !prepared) return false;
    constexpr engine::FrameCount capacity = 256;
    const auto frames = std::min(capacity, bufferSize);
    if (!frames) return false;
    std::array<float, capacity> left{}, right{};
    float* channels[] = {left.data(), right.data()};
    engine.renderBlock(engine::AudioBlock(channels, 2, frames), nullptr, 0, frames);
    return engine.lastBlockResult() == engine::RealtimeEngine::BlockResult::Complete;
}

bool AudioRuntime::pluginAudioActivity() const {
    const auto now = rt::nowNanos();
    if (engine.transport().isPlaying() ||
        std::max(engine.masterPeakLeft(), engine.masterPeakRight()) > 1e-5f ||
        (lastLiveMidiNs && now - lastLiveMidiNs < 2'000'000'000ull) ||
        now < countInUntilNs || previewSnapshot().playing) return true;
    for (const auto& [id, channel] : channels)
        if (!channel.heldMidiNotes.empty() || (channel.input && channel.input->enabled())) return true;
    return hasActiveCaptures();
}

AudioPluginServiceResult AudioRuntime::servicePlugins(bool externallyActive) {
    AudioPluginServiceResult out;
    out.changed = pumpIsolatedPlugins();
    const auto generation = plugins::PluginMainThreadWork::generation();
    const bool compatibilitySweep = ++pluginCompatibilitySweepTicks >= 64;
    const bool pendingCanApply = pendingPitchQualityChanges && !externallyActive && !pluginAudioActivity();
    if (generation == pluginMainThreadGeneration && !compatibilitySweep && !pendingCanApply) return out;
    pluginMainThreadGeneration = generation;
    pluginCompatibilitySweepTicks = 0;
    out.scanned = true;
    pendingPitchQualityChanges = false;
    const bool canApplyQuality = !externallyActive && !pluginAudioActivity();
    bool reconfigure = false;
    std::unique_ptr<engine::RealtimeEngine::RenderGate> qualityGate;

    const auto applyQuality = [&](plugins::PluginNode& node) {
        auto* corrector = dynamic_cast<plugins::pitch::PitchCorrectorInstance*>(node.instance());
        if (!corrector || !corrector->qualityChangePending()) return;
        if (!canApplyQuality) { pendingPitchQualityChanges = true; return; }
        if (!qualityGate) qualityGate = std::make_unique<engine::RealtimeEngine::RenderGate>(engine);
        if (corrector->applyPendingQuality()) { node.invalidatePrepare(); reconfigure = true; }
    };
    const auto pump = [&](const std::string& channelId, AudioPluginChainSpec::Kind kind,
                          const std::string& clipId, InsertSlot& slot) {
        std::shared_ptr<plugins::PluginNode> replacements[2];
        for (unsigned side = 0; side < 2; ++side) {
            const auto& node = side ? slot.rightNode : slot.node;
            if (!node || !node->instance()) continue;
            const AudioPluginAddress address{channelId, slot.slotId, side != 0, node->instanceId()};
            const auto notice = [&](AudioPluginNotice::Kind type, const std::string& parameter = {},
                                    double value = 0, bool touch = false) {
                out.notices.push_back({type, address, kind, clipId, parameter, value, touch});
            };
            node->beginMainThreadPump();
            if (node->takeStateChanged()) notice(AudioPluginNotice::Kind::StateChanged);
            if (node->takeReloadRequested()) {
                // Preserve the old native object and publication until its
                // replacement has restored successfully. One side's failure
                // must never destroy the healthy half of a dual-mono slot.
                const auto snapshot = pluginStateSnapshot(address, true);
                if (snapshot.exists && !snapshot.failed && (!snapshot.supportsState || snapshot.stateCaptured)) {
                    auto instance = createConfiguredPlugin(slot.configuration, slot.hosting);
                    if (instance && (!snapshot.supportsState || instance->loadState(snapshot.state))) {
                        replacements[side] = std::make_shared<plugins::PluginNode>(
                            std::string(node->name()), std::move(instance));
                        if (!snapshot.supportsState) applyStoredParameters(*replacements[side], snapshot.parameters);
                        applyStoredParameters(*replacements[side], snapshot.pending);
                    }
                }
                if (!replacements[side]) out.error = "Could not restore reloaded plugin: " + slot.slotId;
                continue;
            }
            // Both latches must be consumed even if latency already changed.
            const bool latency = node->takeLatencyChanged();
            const bool restart = node->takeRestartRequested();
            if (latency || restart) { node->invalidatePrepare(); reconfigure = true; }
            node->instance()->pumpMainThread();
            applyQuality(*node);
            plugins::PluginEvent event;
            while (node->popNotification(event)) {
                const bool gesture = event.kind == plugins::PluginEvent::Kind::ParamGestureBegin;
                if ((!gesture && event.kind != plugins::PluginEvent::Kind::ParamValue) ||
                    (side && gesture)) continue;
                const auto parameters = node->instance()->parameters();
                if (event.paramIndex >= parameters.size() || (!gesture && !std::isfinite(event.value))) continue;
                notice(gesture ? AudioPluginNotice::Kind::GestureBegin : AudioPluginNotice::Kind::Parameter,
                    parameters[event.paramIndex].id, event.value,
                    !side && (gesture || node->instance()->isEditorOpen()));
                out.changed |= !gesture;
            }
        }
        if (replacements[0] || replacements[1]) {
            const auto result = replacePluginNodes(channelId, slot,
                std::move(replacements[0]), std::move(replacements[1]));
            if (!result) out.error = "Could not publish reloaded plugin: " + slot.slotId;
            out.changed = true;
        }
    };
    const auto pumpChain = [&](const auto& id, auto kind, const std::string& clipId, auto& slots) {
        for (auto& slot : slots) pump(id, kind, clipId, slot);
    };
    using Kind = AudioPluginChainSpec::Kind;
    for (auto& [id, channel] : channels) {
        pumpChain(id, Kind::Instrument, {}, channel.instrument);
        pumpChain(id, Kind::MiniModules, {}, channel.miniModules);
        pumpChain(id, Kind::SamplerInserts, {}, channel.samplerInserts);
        for (auto& [clipId, clip] : channel.clipFx) pumpChain(id, Kind::ClipFx, clipId, clip.inserts);
        pumpChain(id, Kind::Inserts, {}, channel.inserts);
    }
    if (reconfigure && hasPublishedGraph) {
        auto previous = engine.graph();
        engine.graph() = publishedGraph;
        if (!commitGraph(true)) {
            engine.graph() = std::move(previous);
            out.error = "Could not reconfigure plugin graph.";
        }
        out.changed = true;
    }
    return out;
}

} // namespace daw
