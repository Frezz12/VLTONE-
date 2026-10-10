#include "AudioRuntime.hpp"
#include "AudioContentValidation.hpp"
#include "Host/ParameterDiagnostics.hpp"
#include "Internal/SamplerInstance.hpp"

#include <algorithm>

namespace daw {

bool AudioRuntime::buildSession(AudioSessionSpec session) {
    bool parametersApplied = false;
    for (const auto& chain : session.pluginChains)
        parametersApplied |= reconcilePluginChain(chain);
    buildGraph(session.graph);
    for (auto& channel : session.channels) applyContent(channel.id, std::move(channel.content));
    return parametersApplied;
}

bool AudioRuntime::applyContent(const std::string& channelId, AudioContentSpec content) {
    validateAudioContent(content);
    const auto found = channels.find(channelId);
    if (found == channels.end()) return false;
    auto& channel = found->second;
    if (content.clips) {
        static const auto empty = std::make_shared<const engine::ClipPlayerNode::ClipList>();
        if (channel.clips) channel.clips->setClips(content.clips->shared ? content.clips->shared : empty);
        // An omitted private player is cleared as well, including dormant
        // recording chains. A deleted take must not survive in its old node.
        for (auto& [id, clip] : channel.clipFx) {
            const auto list = content.clips->individual.find(id);
            if (clip.player) clip.player->setClips(list != content.clips->individual.end() && list->second
                ? list->second : empty);
        }
    }
    if (content.midi && channel.midiClips) {
        channel.midiClips->setTimelineSuppressed(content.midi->timelineSuppressed);
        channel.midiClips->setControllers(std::move(content.midi->controllers));
        channel.midiClips->setNotes(std::move(content.midi->notes));
    }
    if (content.plugins) {
        channel.automation = std::make_shared<const AudioContentSpec::PluginCurves>(std::move(*content.plugins));
        applyPluginAutomation(channel);
    }
    if (content.levels) {
        if (channel.fader) channel.fader->setAutomation(std::move(content.levels->fader));
        for (std::size_t i = 0; i < channel.sends.size(); ++i) {
            if (!channel.sends[i]) continue;
            const auto curve = i < channel.sendIds.size()
                ? content.levels->sends.find(channel.sendIds[i]) : content.levels->sends.end();
            channel.sends[i]->setAutomation(curve != content.levels->sends.end() ? curve->second : nullptr);
        }
    }
    return true;
}

bool AudioRuntime::sendLiveMidi(const std::string& channelId, const engine::MidiEvent& event) {
    const auto found = channels.find(channelId);
    if (found == channels.end() || !found->second.midiClips || !found->second.midiClips->sendLiveEvent(event))
        return false;
    auto& notes = found->second.heldMidiNotes;
    const auto key = std::uint16_t((std::uint16_t(event.channel()) << 8) | event.data1);
    if (event.isNoteOn()) notes.insert(key);
    else if (event.isNoteOff()) notes.erase(key);
    else if (event.type() == engine::MidiEvent::kControlChange && event.data1 == 64) {
        const auto pedal = std::uint16_t((std::uint16_t(event.channel()) << 8) | 128);
        if (event.data2 >= 64) notes.insert(pedal);
        else notes.erase(pedal);
    }
    else if (event.type() == engine::MidiEvent::kControlChange && (event.data1 == 120 || event.data1 == 123))
        std::erase_if(notes, [&](auto note) {
            return (note >> 8) == event.channel() && (event.data1 == 120 || (note & 255) < 128);
        });
    lastLiveMidiNs = rt::nowNanos();
    return true;
}

void AudioRuntime::applyPluginAutomation(TrackChannel& channel) {
    if (!channel.automation) return;
    using Curve = AudioContentSpec::PluginCurve;
    std::unordered_map<std::string, std::vector<const Curve*>> bySlot;
    for (const auto& curve : *channel.automation) {
        const auto& slotId = curve.slotId.empty() && !channel.instrument.empty()
            ? channel.instrument.front().slotId : curve.slotId;
        bySlot[slotId].push_back(&curve);
    }
    static const auto empty = std::make_shared<const plugins::PluginNode::AutomationCurves>();
    const auto apply = [&](const std::vector<InsertSlot>& slots) {
        for (const auto& slot : slots) for (const auto& node : {slot.node, slot.rightNode}) {
            if (!node || !node->instance() || node->faultBypassed()) continue;
            const auto sources = bySlot.find(slot.slotId);
            if (sources == bySlot.end()) { node->setAutomation(empty); continue; }
            auto curves = std::make_shared<plugins::PluginNode::AutomationCurves>();
            std::unordered_map<std::uint32_t, std::size_t> indices;
            auto* instance = node->instance();
            const auto parameters = instance->parameters();
            for (const auto* source : sources->second) {
                const auto index = instance->parameterIndexForId(source->parameterId);
                if (index < 0 || std::size_t(index) >= parameters.size()) continue;
                const auto& parameter = parameters[std::size_t(index)];
                if (!parameter.isAutomatable) continue;
                const auto toPlain = [&](double value) {
                    return parameter.minValue + (parameter.maxValue - parameter.minValue) * std::clamp(value, 0.0, 1.0);
                };
                const auto [entry, inserted] = indices.try_emplace(std::uint32_t(index), curves->size());
                if (inserted) curves->push_back({std::uint32_t(index), toPlain(source->defaultValue), {}});
                auto& points = (*curves)[entry->second].points;
                points.reserve(points.size() + source->points.size());
                for (const auto& [beat, value] : source->points) points.emplace_back(beat, toPlain(value));
            }
            for (auto& curve : *curves) {
                std::stable_sort(curve.points.begin(), curve.points.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
                plugins::logParameterWrite("automation", instance, std::int32_t(curve.parameterIndex),
                    curve.points.empty() ? curve.defaultValue : curve.points.front().second);
            }
            if (auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(instance)) {
                const auto mode = std::uint32_t(plugins::sampler::Param::StretchMode);
                if (std::any_of(curves->begin(), curves->end(), [mode](const auto& curve) {
                        return curve.parameterIndex == mode;
                    })) sampler->prepareStretchModeAutomation();
            }
            node->setAutomation(std::move(curves));
        }
    };
    apply(channel.instrument);
    apply(channel.miniModules);
    apply(channel.samplerInserts);
    for (const auto& [id, clip] : channel.clipFx) apply(clip.inserts);
    apply(channel.inserts);
}

} // namespace daw
