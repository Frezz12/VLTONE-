#include "AudioRuntime.hpp"
#include "AudioRuntimeSession.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <variant>

namespace daw {
namespace {
template<class Node> struct SavedControl {
    std::shared_ptr<Node> node;
    typename Node::ControlState state;
    void restore() const { node->restoreControlState(state); }
};
using Control = std::variant<SavedControl<engine::GainNode>, SavedControl<engine::SendNode>,
    SavedControl<engine::InputNode>, SavedControl<engine::ClipPlayerNode>,
    SavedControl<engine::MidiClipPlayerNode>, SavedControl<plugins::PluginNode>>;
struct BuiltinState {
    std::shared_ptr<plugins::PluginNode> node;
    std::vector<std::uint8_t> bytes;
    void restore() const {
        // These built-ins update their native configuration during graph
        // reconciliation. Reused foreign processors keep their DSP histories.
        std::vector<std::uint8_t> current;
        auto* plugin = node->instance();
        if (plugin->saveState(current) && current == bytes) return;
        if (!plugin->loadState(bytes)) throw std::runtime_error("Could not restore built-in configuration.");
        node->invalidatePrepare();
    }
};
} // namespace

struct AudioRuntime::SessionTransaction {
    decltype(AudioRuntime::channels) channels;
    // A failed candidate may own foreign processes. Keep them until the lease
    // releases outside the publication gate, rather than stopping them here.
    std::vector<decltype(AudioRuntime::channels)> retired;
    engine::AudioGraph editable, published;
    std::shared_ptr<const engine::CompiledGraph> compiled;
    bool hasPublished = false;
    std::shared_ptr<engine::SumNode> masterSum;
    std::shared_ptr<engine::GainNode> masterFader;
    std::shared_ptr<engine::MetronomeNode> metronome;
    std::shared_ptr<engine::PreviewPlayerNode> preview;
    bool metronomeEnabled = false;
    std::shared_ptr<const engine::SampleBuffer> metronomeSample;
    engine::NodeId masterSumId = engine::kInvalidNode, masterFaderId = engine::kInvalidNode;
    decltype(AudioRuntime::renderTaps) renderTaps;
    decltype(AudioRuntime::preparedMiniModules) preparedMiniModules;
    std::vector<Control> controls;
    std::vector<BuiltinState> builtins;
    std::map<plugins::PluginNode*, std::shared_ptr<const AudioPluginStateSnapshot>> sources;
};

AudioRuntime::TransactionId AudioRuntime::captureTransaction() {
    if (sessionTransactions.size() >= 64 || nextSessionTransaction == std::numeric_limits<TransactionId>::max())
        throw std::runtime_error("Audio transaction capacity exceeded.");
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto saved = std::make_shared<SessionTransaction>();
    saved->channels = channels;
    saved->editable = engine.graph(); saved->published = publishedGraph;
    saved->compiled = engine.sessionGraph(); saved->hasPublished = hasPublishedGraph;
    saved->masterSum = masterSum; saved->masterFader = masterFader;
    saved->metronome = metronome; saved->preview = preview;
    saved->metronomeEnabled = metronome && metronome->enabled();
    if (metronome) saved->metronomeSample = metronome->sample();
    saved->masterSumId = masterSumId; saved->masterFaderId = masterFaderId;
    saved->renderTaps = renderTaps; saved->preparedMiniModules = preparedMiniModules;
    std::unordered_set<const engine::Node*> seen;
    const auto remember = [&]<class Node>(const std::shared_ptr<Node>& node) {
        if (node && seen.insert(node.get()).second)
            saved->controls.emplace_back(SavedControl<Node>{node, node->controlState()});
    };
    const auto rememberSlots = [&](const auto& slots) {
        for (const auto& slot : slots) for (const auto& node : {slot.node, slot.rightNode}) {
            if (!node || seen.contains(node.get())) continue;
            remember(node);
            if (node->faultBypassed()) continue;
            if (dynamic_cast<plugins::channel_color::ChannelColorInstance*>(node->instance()) ||
                dynamic_cast<plugins::mini::MiniModuleInstance*>(node->instance())) {
                BuiltinState state; state.node = node;
                if (!node->instance()->saveState(state.bytes))
                    throw std::runtime_error("Could not capture built-in configuration.");
                saved->builtins.push_back(std::move(state));
            }
        }
    };
    remember(masterFader);
    for (const auto& [id, channel] : channels) {
        remember(channel.clips); remember(channel.frozenPlayer); remember(channel.midiClips);
        remember(channel.input); remember(channel.fader); remember(channel.samplerFader);
        for (const auto& send : channel.sends) remember(send);
        rememberSlots(channel.instrument); rememberSlots(channel.miniModules);
        rememberSlots(channel.inserts); rememberSlots(channel.samplerInserts);
        for (const auto& [clipId, clip] : channel.clipFx) {
            remember(clip.player); remember(clip.fader); rememberSlots(clip.inserts);
        }
    }
    const auto id = ++nextSessionTransaction;
    sessionTransactions.emplace(id, std::move(saved));
    return id;
}

void AudioRuntime::preservePluginSourceForTransactions(const AudioPluginAddress& address) {
    if (sessionTransactions.empty()) return;
    auto* node = pluginNode(address);
    if (!node || !node->instance()) return;
    const engine::RealtimeEngine::RenderGate gate(engine);
    std::shared_ptr<const AudioPluginStateSnapshot> source;
    for (const auto& [id, saved] : sessionTransactions) {
        if (saved->sources.contains(node)) continue;
        const auto owned = std::find_if(saved->controls.begin(), saved->controls.end(), [&](const Control& value) {
            const auto* plugin = std::get_if<SavedControl<plugins::PluginNode>>(&value);
            return plugin && plugin->node.get() == node;
        });
        if (owned == saved->controls.end()) continue;
        if (!source) {
            auto snapshot = pluginStateSnapshot(address, true);
            if (!snapshot.ownsSample) return;
            if (!snapshot.stateCaptured)
                throw std::runtime_error("Could not capture the instrument source before editing.");
            source = std::make_shared<const AudioPluginStateSnapshot>(std::move(snapshot));
        }
        saved->sources.emplace(node, source);
    }
}

audio::Result AudioRuntime::restoreTransaction(TransactionId id) {
    const auto found = sessionTransactions.find(id);
    if (found == sessionTransactions.end())
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio transaction no longer exists.");
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto& saved = *found->second;
    try {
        saved.retired.push_back(std::move(channels));
        channels = saved.channels;
        masterSum = saved.masterSum; masterFader = saved.masterFader;
        metronome = saved.metronome; preview = saved.preview;
        masterSumId = saved.masterSumId; masterFaderId = saved.masterFaderId;
        renderTaps = saved.renderTaps; preparedMiniModules = saved.preparedMiniModules;
        if (metronome) {
            metronome->setEnabled(saved.metronomeEnabled);
            metronome->setSample(saved.metronomeSample);
        }
        for (const auto& builtin : saved.builtins) builtin.restore();
        for (const auto& [node, snapshot] : saved.sources) {
            AudioPluginStateRestore state;
            state.state = snapshot->state;
            state.source = snapshot->sample;
            state.sourcePath = snapshot->samplePath;
            state.clearPending = true;
            auto parameters = snapshot->parameters;
            if (const auto restored = restorePluginNode(*node, state, parameters, snapshot->address.slotId); !restored)
                throw std::runtime_error(restored.message());
            node->invalidatePrepare();
        }
        for (const auto& control : saved.controls)
            std::visit([](const auto& value) { value.restore(); }, control);
        engine.restoreSessionGraph(saved.compiled);
        publishedGraph = saved.published; hasPublishedGraph = saved.hasPublished;
        engine.graph() = saved.published;
        // Only changed bus layouts/configurations need preparation. Compilation
        // reuses node owners and compensation delays, even for removed channels.
        if (saved.hasPublished) {
            if (auto result = commitGraph(); !result) {
                engine.graph() = saved.editable;
                stopForFailedRollback();
                return result;
            }
        }
        engine.graph() = saved.editable;
        return audio::Result::ok();
    } catch (const std::exception& error) {
        engine.graph() = saved.editable;
        stopForFailedRollback();
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}

void AudioRuntime::releaseTransaction(TransactionId id) { sessionTransactions.erase(id); }

void AudioRuntime::prunePluginSnapshots() {
    std::unordered_set<std::uint64_t> retained;
    for (const auto& address : pluginAddresses()) retained.insert(address.instance);
    for (const auto& [id, saved] : sessionTransactions)
        for (const auto& control : saved->controls)
            if (const auto* plugin = std::get_if<SavedControl<plugins::PluginNode>>(&control))
                retained.insert(plugin->node->instanceId());
    std::erase_if(lastGoodPluginStates, [&](const auto& entry) { return !retained.contains(entry.first); });
    std::erase_if(checkpointParameterEdits, [&](const auto& entry) { return !retained.contains(entry.first); });
    std::erase_if(sharedCheckpointBytes, [&](const auto& entry) { return !retained.contains(entry.first); });
}
void AudioRuntime::collectTransactionRetirements(TransactionId id) {
    const auto found = sessionTransactions.find(id);
    if (found != sessionTransactions.end()) found->second->retired.clear();
}

audio::Result AudioRuntime::applySession(AudioSessionSpec session, bool reconfigurePlugins,
    std::span<const AudioPluginStateEdit> restores, std::span<const AudioPluginCheckpoint> checkpoints,
    AudioSessionPublication* publication, const std::function<void()>& afterCommitBeforeRetire) {
    try {
        AudioSessionPublication preparedPublication;
        auto staging = stageSessionPlugins(session, restores, checkpoints,
            publication ? &preparedPublication : nullptr);
        // Both leases outlive the gate: native construction, unused staged
        // processors and retired plugin destruction stay off the render stop.
        std::optional<ScopedAudioTransaction<AudioRuntime>> transaction;
        // Controls and immutable schedules cannot become audible until commit.
        // Reconciliation retains existing processors, voices and meters; only
        // new or changed plugin slots are loaded.
        const engine::RealtimeEngine::RenderGate gate(engine);
        if (!stagedSessionPluginsCurrent(staging))
            return audio::Result::fail(audio::EngineError::InvalidArgument, "Audio runtime changed during plugin preparation.");
        transaction.emplace(*this);
        std::unordered_map<std::uint64_t, std::shared_ptr<plugins::PluginNode>> oldEditors;
        if (afterCommitBeforeRetire) for (const auto& address : pluginAddresses()) {
            const auto* slot = pluginSlot(address.channelId, address.slotId);
            oldEditors.emplace(address.instance, address.right ? slot->rightNode : slot->node);
        }
        auto result = audio::Result::ok();
        try {
            // applySession is a complete topology projection. Omitted chains
            // retire their slots; applyContent remains the partial-update API.
            std::set<std::tuple<std::string, AudioPluginChainSpec::Kind, std::string>> wanted;
            for (const auto& chain : session.pluginChains) wanted.emplace(chain.channelId, chain.kind, chain.clipId);
            for (auto& [channelId, channel] : channels) {
                const auto retireOmitted = [&](AudioPluginChainSpec::Kind kind,
                    const std::string& clipId, auto& slots) {
                    if (!wanted.contains({channelId, kind, clipId})) slots.clear();
                };
                using Kind = AudioPluginChainSpec::Kind;
                retireOmitted(Kind::Instrument, {}, channel.instrument);
                retireOmitted(Kind::MiniModules, {}, channel.miniModules);
                retireOmitted(Kind::SamplerInserts, {}, channel.samplerInserts);
                retireOmitted(Kind::Inserts, {}, channel.inserts);
                for (auto& [clipId, clip] : channel.clipFx) retireOmitted(Kind::ClipFx, clipId, clip.inserts);
            }
            buildSession(std::move(session));
            result = commitGraph(reconfigurePlugins);
        } catch (const std::exception& error) {
            result = audio::Result::fail(audio::EngineError::Unknown, error.what());
        }
        if (result) {
            if (afterCommitBeforeRetire) {
                retiringEditorNodes = std::move(oldEditors);
                try { afterCommitBeforeRetire(); }
                catch (const std::exception& error) {
                    retiringEditorNodes.clear();
                    stopForFailedRollback();
                    return audio::Result::fail(audio::EngineError::AudioThreadError,
                        std::string("Could not retire the previous plugin editor: ") + error.what());
                }
                retiringEditorNodes.clear();
            }
            if (publication) *publication = std::move(preparedPublication);
            return result;
        }
        if (const auto rollback = transaction->restore(); !rollback) {
            stopForFailedRollback();
            return audio::Result::fail(rollback.error(), result.message() + "; audio rollback failed: " + rollback.message());
        }
        return result;
    } catch (const std::exception& error) {
        return audio::Result::fail(audio::EngineError::Unknown, error.what());
    }
}
} // namespace daw
