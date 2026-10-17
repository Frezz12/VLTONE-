#include "AudioRuntime.hpp"
#include <stdexcept>

namespace daw {
void AudioRuntime::connectModulation(TrackChannel& channel, const std::string& id, const modulation::Rack& rack) {
    channel.modulation = rack;
    channel.modulationStatus.clear();
    channel.modulationTargets.clear();
    std::erase_if(channel.modulators, [&](const auto& item) {
        return std::none_of(rack.sources.begin(), rack.sources.end(), [&](const auto& s) { return s.id == item.first; });
    });
    auto& graph = engine.graph();
    std::unordered_map<std::string, engine::NodeId> sourceIds;
    for (const auto& source : rack.sources) {
        auto& node = channel.modulators[source.id];
        if (!node) node = std::make_shared<engine::ModulationNode>(source.name);
        auto sourceNode = graph.adoptNode(node);
        if (source.kind == modulation::Kind::Follower) {
            const auto found = channels.find(source.audioChannelId.empty() ? id : source.audioChannelId);
            if (found == channels.end()) continue;
            const auto& taps = found->second.ids;
            auto tap = source.audioChannelId.empty() || source.tap == modulation::Tap::BeforeEffects ? taps.sourceTap
                : source.tap == modulation::Tap::AfterEffects ? taps.preFaderTap : taps.meter;
            if (tap == engine::kInvalidNode) continue;
            graph.connect(tap, sourceNode);
        }
        sourceIds.emplace(source.id, sourceNode);
    }
    for (const auto& route : rack.routes) channel.modulationStatus[route.id] = "Device unavailable";
    const auto connect = [&](const std::vector<InsertSlot>& slots) {
        for (const auto& slot : slots) {
            for (const auto& node : {slot.node, slot.rightNode}) {
                if (!node || !node->instance()) continue;
                const auto targetId = node == slot.node ? slot.nodeId : slot.rightNodeId;
                auto& target = channel.modulationTargets.emplace_back(); target.node = node;
                std::unordered_map<std::string, std::uint32_t> inputs;
                for (const auto& route : rack.routes) {
                    if (route.slotId != slot.slotId) continue;
                    auto& status = channel.modulationStatus[route.id];
                    const auto& descriptor = node->instance()->descriptor();
                    if (route.pluginUid != slot.uid || route.pluginFormat != plugins::toString(descriptor.format)) {
                        status = "Plugin identity changed"; continue;
                    }
                    if (targetId == engine::kInvalidNode || node->faultBypassed()) { status = "Device unavailable"; continue; }
                    const auto parameter = node->instance()->parameterIndexForId(route.parameterId);
                    const auto parameters = node->instance()->parameters();
                    if (parameter < 0 || std::size_t(parameter) >= parameters.size() ||
                        !parameters[parameter].isAutomatable || parameters[parameter].isReadOnly ||
                        !std::isfinite(parameters[parameter].minValue) || !std::isfinite(parameters[parameter].maxValue) ||
                        parameters[parameter].minValue >= parameters[parameter].maxValue) {
                        status = "Parameter unavailable"; continue;
                    }
                    const auto source = sourceIds.find(route.sourceId);
                    if (source == sourceIds.end()) { status = "Audio source unavailable"; continue; }
                    if (graph.wouldCycle(source->second, targetId))
                        throw std::invalid_argument("Modulation would create an audio feedback cycle.");
                    const auto [input, added] = inputs.try_emplace(route.sourceId, std::uint32_t(inputs.size()));
                    if (added) graph.connect(source->second, targetId, engine::InputRole::Modulation);
                    auto& compiled = target.routes.emplace_back();
                    compiled.parameter = parameter; compiled.input = input->second; compiled.id = route.id;
                    status.clear();
                }
            }
        }
    };
    connect(channel.instrument); connect(channel.inserts); connect(channel.miniModules);
    refreshModulation(channel);
}

void AudioRuntime::refreshModulation(TrackChannel& channel) {
    const auto curveFor = [&](const std::string& id, AutomationTargetKind kind, double base) {
        modulation::Curve result; result.fallback = base;
        if (channel.automation) for (const auto& c : *channel.automation) if (c.kind == kind && c.slotId == id) {
            result.automated = true;
            result.fallback = c.defaultValue;
            result.points.insert(result.points.end(), c.points.begin(), c.points.end());
        }
        result.prepare();
        return result;
    };
    for (const auto& source : channel.modulation.sources) {
        const auto found = channel.modulators.find(source.id);
        if (found == channel.modulators.end()) continue;
        auto settings = std::make_shared<engine::ModulationNode::Settings>(); settings->source = source;
        settings->rate = curveFor(source.id, AutomationTargetKind::ModulatorRate, source.sync ? 1.0 / source.beats : source.rate);
        found->second->setSettings(std::move(settings));
    }
    for (auto& target : channel.modulationTargets) {
        for (auto& compiled : target.routes) {
            const auto route = std::find_if(channel.modulation.routes.begin(), channel.modulation.routes.end(),
                [&](const auto& r) { return r.id == compiled.id; });
            if (route == channel.modulation.routes.end()) { compiled.enabled = false; continue; }
            const auto source = std::find_if(channel.modulation.sources.begin(), channel.modulation.sources.end(),
                [&](const auto& s) { return s.id == route->sourceId; });
            compiled.depth = route->depth; compiled.bipolar = route->bipolar;
            compiled.enabled = channel.modulation.enabled && route->enabled && source != channel.modulation.sources.end() && source->enabled;
            compiled.automation = curveFor(route->id, AutomationTargetKind::ModulationDepth, route->depth);
        }
        target.node->modulation().configure(target.routes, *target.node->instance());
        for (const auto& event : target.node->pendingParameterEvents())
            if (event.kind == plugins::PluginEvent::Kind::ParamValue)
                target.node->modulation().edit(event.paramIndex, event.value);
    }
}

bool AudioRuntime::updateModulation(const std::string& id, const modulation::Rack& rack) {
    const auto found = channels.find(id); if (found == channels.end()) return false;
    const engine::RealtimeEngine::RenderGate gate(engine);
    found->second.modulation = rack; refreshModulation(found->second); return true;
}
float AudioRuntime::modulationValue(const std::string& id, const std::string& source) const {
    const auto channel = channels.find(id); if (channel == channels.end()) return 0;
    const auto node = channel->second.modulators.find(source);
    return node == channel->second.modulators.end() ? 0 : node->second->value();
}
std::string AudioRuntime::modulationStatus(const std::string& id, const std::string& route) const {
    const auto channel = channels.find(id); if (channel == channels.end()) return "Channel unavailable";
    const auto status = channel->second.modulationStatus.find(route);
    return status == channel->second.modulationStatus.end() ? "Assignment unavailable" : status->second;
}
} // namespace daw
