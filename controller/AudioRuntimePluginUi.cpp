#include "AudioRuntime.hpp"
#include "Host/ParameterDiagnostics.hpp"
#include "Internal/Cla2aInstance.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/DelayInstance.hpp"
#include "Internal/GraphitInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/GravityInstance.hpp"
#include "Internal/ModulationRackInstance.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SlicerInstance.hpp"

#include <algorithm>
#include <cmath>

namespace daw {
namespace {
std::int32_t parameterIndex(const plugins::PluginInstance& plugin,
    std::span<const plugins::ParameterInfo> parameters, const std::string& id, std::int32_t hint = -1) {
    const auto matches = [&](std::int32_t index) {
        return index >= 0 && std::size_t(index) < parameters.size() && parameters[index].id == id;
    };
    if (matches(hint)) return hint;
    const auto resolved = plugin.parameterIndexForId(id);
    return matches(resolved) ? resolved : -1;
}
}

const AudioRuntime::InsertSlot* AudioRuntime::pluginSlot(
    const std::string& channelId, const std::string& slotId) const {
    const auto found = channels.find(channelId);
    if (found == channels.end()) return nullptr;
    const auto& channel = found->second;
    const auto find = [&](const auto& slots) -> const InsertSlot* {
        const auto entry = std::find_if(slots.begin(), slots.end(),
            [&](const auto& slot) { return slot.slotId == slotId; });
        return entry == slots.end() ? nullptr : &*entry;
    };
    for (const auto* slots : {&channel.miniModules, &channel.instrument, &channel.samplerInserts})
        if (const auto* slot = find(*slots)) return slot;
    for (const auto& [id, clip] : channel.clipFx)
        if (const auto* slot = find(clip.inserts)) return slot;
    return find(channel.inserts);
}

AudioRuntime::InsertSlot* AudioRuntime::pluginSlot(
    const std::string& channelId, const std::string& slotId) {
    return const_cast<InsertSlot*>(std::as_const(*this).pluginSlot(channelId, slotId));
}

plugins::PluginNode* AudioRuntime::pluginNode(const AudioPluginAddress& address) const {
    const auto* slot = pluginSlot(address.channelId, address.slotId);
    if (!slot) return nullptr;
    auto* node = address.right ? slot->rightNode.get() : slot->node.get();
    return node && (!address.instance || node->instanceId() == address.instance) ? node : nullptr;
}

plugins::PluginInstance* AudioRuntime::pluginInstance(const AudioPluginAddress& address) const {
    auto* node = pluginNode(address);
    return node && !safetyStopped.load(std::memory_order_acquire) && !node->faultBypassed() ? node->instance() : nullptr;
}

bool AudioRuntime::hasPlugin(const AudioPluginAddress& address, std::string_view uid) const {
    const auto* node = pluginNode(address);
    return node && node->instance() && (uid.empty() || node->descriptor().uid == uid);
}

std::uint64_t AudioRuntime::pluginInstanceId(const AudioPluginAddress& address) const {
    const auto* node = pluginNode(address);
    return node && node->instance() ? node->instanceId() : 0;
}

std::vector<plugins::ParameterInfo> AudioRuntime::pluginParameters(const AudioPluginAddress& address) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return {};
    const auto parameters = plugin->parameters();
    return {parameters.begin(), parameters.end()};
}

std::optional<plugins::ParameterInfo> AudioRuntime::pluginParameterInfo(
    const AudioPluginAddress& address, const std::string& parameterId) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return {};
    const auto parameters = plugin->parameters();
    const auto index = parameterIndex(*plugin, parameters, parameterId);
    return index >= 0 ? std::optional(parameters[std::size_t(index)]) : std::nullopt;
}

AudioPluginCapabilities AudioRuntime::pluginCapabilities(const AudioPluginAddress& address) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return {};
    const auto& layout = plugin->busLayout();
    return {true, !plugin->descriptor().isInstrument && !layout.inputs.empty(), layout.inputs.size() > 1};
}

bool AudioRuntime::setPluginControls(const std::string& channelId, const std::string& slotId,
    const AudioPluginControlChange& change) {
    auto* slot = pluginSlot(channelId, slotId);
    if (!slot || (change.mix && !std::isfinite(*change.mix))) return false;
    for (const auto& node : {slot->node, slot->rightNode}) if (node) {
        if (change.bypassed) node->setBypassed(*change.bypassed);
        if (change.mix) node->setMix(*change.mix);
    }
    return true;
}

AudioPluginSlideStatus AudioRuntime::pluginSlideStatus(const AudioPluginAddress& address) const {
    const auto* node = pluginNode(address);
    return node ? AudioPluginSlideStatus{int(node->slideDelivery()), node->slideOverloaded(), node->slideClipped()}
                : AudioPluginSlideStatus{};
}

bool AudioRuntime::setPluginSlide(const AudioPluginAddress& address, int mode, double range, double reserve) {
    auto* node = pluginNode(address);
    if (!node || mode < 0 || mode > 4 || !std::isfinite(range) || !std::isfinite(reserve)) return false;
    node->setSlideDelivery(plugins::SlideDelivery(mode), std::clamp(range, 1., 96.), std::clamp(reserve, 0., 20.));
    return true;
}

bool AudioRuntime::setPluginAutomationOverride(const AudioPluginAddress& address, const std::string& parameterId) {
    auto* node = pluginNode(address);
    auto* plugin = node ? node->instance() : nullptr;
    if (!plugin || node->faultBypassed()) return false;
    if (parameterId.empty()) { node->clearAutomationOverrides(); return true; }
    const auto index = parameterIndex(*plugin, plugin->parameters(), parameterId);
    if (index < 0) return false;
    node->overrideAutomation(std::uint32_t(index));
    return true;
}

double AudioRuntime::pluginParameter(const AudioPluginAddress& address, const std::string& parameterId) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return 0;
    const auto index = parameterIndex(*plugin, plugin->parameters(), parameterId);
    return index >= 0 ? plugin->parameterValue(std::uint32_t(index)) : 0;
}

bool AudioRuntime::setPluginParameter(const AudioPluginAddress& address,
    const std::string& parameterId, double value) {
    auto* node = pluginNode(address);
    auto* plugin = node ? node->instance() : nullptr;
    if (!plugin || safetyStopped.load(std::memory_order_acquire) || node->faultBypassed() || !std::isfinite(value)) return false;
    const auto index = parameterIndex(*plugin, plugin->parameters(), parameterId);
    if (index < 0) return false;
    plugins::PluginEvent event;
    event.kind = plugins::PluginEvent::Kind::ParamValue;
    event.paramIndex = std::uint32_t(index);
    event.value = value;
    if (!node->pushEvent(event)) return false;
    auto& edits = checkpointParameterEdits[node->instanceId()];
    const InsertParameter change{parameterId, value, true};
    overlayPendingParameters(edits, std::span{&change, 1});
    plugin->setParameterFromHost(std::uint32_t(index), value);
    plugins::logParameterWrite("knob", plugin, index, value);
    return true;
}

void AudioRuntime::readPluginParameters(const AudioPluginAddress& address,
    std::span<PluginParameterReadout> values) const {
    const auto* plugin = pluginInstance(address);
    const auto parameters = plugin ? plugin->parameters() : std::span<const plugins::ParameterInfo>{};
    for (auto& value : values) {
        value.index = plugin ? parameterIndex(*plugin, parameters, value.id, value.index) : -1;
        value.available = value.index >= 0;
        value.value = value.available ? plugin->parameterValue(std::uint32_t(value.index)) : 0;
    }
}

std::string AudioRuntime::pluginParameterText(const AudioPluginAddress& address,
    const std::string& parameterId, double value, std::int32_t indexHint) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return {};
    const auto index = parameterIndex(*plugin, plugin->parameters(), parameterId, indexHint);
    return index >= 0 ? plugin->parameterText(std::uint32_t(index), value) : std::string{};
}

std::optional<std::array<double, plugins::equalizer::kParameterCount>> AudioRuntime::captureEqualizerComparison(
    const AudioPluginAddress& address, char slot) {
    if (slot != 'A' && slot != 'B') return std::nullopt;
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin || plugin->activeComparison() == slot) return std::nullopt;
    plugin->captureComparison(plugin->activeComparison());
    return plugin->comparison(slot);
}

bool AudioRuntime::activateEqualizerComparison(const AudioPluginAddress& address, char slot) {
    if (slot != 'A' && slot != 'B') return false;
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin) return false;
    plugin->setPresetReference("custom", "Custom");
    plugin->setActiveComparison(slot);
    return true;
}

std::optional<PluginEditorSnapshot> AudioRuntime::pluginEditorSnapshot(const AudioPluginAddress& address) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return std::nullopt;
    const auto& descriptor = plugin->descriptor();
    return PluginEditorSnapshot{{0, pluginInstanceId(address)}, descriptor.name, descriptor.uid,
        descriptor.format, plugin->hasEditor(), plugin->isEditorOpen()};
}


bool AudioRuntime::openPluginEditor(const AudioPluginAddress& address,
    void* parent, plugins::PluginEditorHost* host) {
    auto* plugin = pluginInstance(address);
    if (!plugin) return false;
    return parent && host && plugin->openEditor(parent, host);
}

bool AudioRuntime::closePluginEditor(const AudioPluginAddress& address, bool onlyUnattached) {
    if (!address.instance) return false;
    const auto* slot = pluginSlot(address.channelId, address.slotId);
    // A wrapper can close after selection moved to the other dual-mono side.
    auto node = slot ? slot->node : nullptr;
    if ((!node || node->instanceId() != address.instance) && slot) node = slot->rightNode;
    if (!node || node->instanceId() != address.instance) {
        const auto retired = retiringEditorNodes.find(address.instance);
        if (retired != retiringEditorNodes.end()) node = retired->second;
    }
    if (!node || node->instanceId() != address.instance) return false;
    auto* plugin = node->instance();
    if (!plugin || (onlyUnattached && plugin->isEditorOpen())) return false;
    plugin->closeEditor();
    return true;
}

std::optional<PluginEditorSize> AudioRuntime::pluginEditorSize(const AudioPluginAddress& address) const {
    const auto* plugin = pluginInstance(address);
    if (!plugin) return std::nullopt;
    PluginEditorSize size;
    if (!plugin->editorSize(size.width, size.height)) size = {};
    size.resizable = plugin->editorCanResize();
    return size;
}

std::optional<PluginEditorSize> AudioRuntime::resizePluginEditor(const AudioPluginAddress& address,
    PluginEditorSize requested) {
    if (!requested.width || !requested.height) return std::nullopt;
    auto* plugin = pluginInstance(address);
    if (!plugin || !plugin->isEditorOpen() || !plugin->editorCanResize() ||
        !plugin->setEditorSize(requested.width, requested.height)) return std::nullopt;
    requested.resizable = true;
    return requested;
}

bool AudioRuntime::pumpPluginEditor(const AudioPluginAddress& address) {
    auto* plugin = pluginInstance(address);
    if (!plugin || !plugin->isEditorOpen() || plugin->descriptor().format != plugins::Format::Vst) return false;
    plugin->pumpMainThread();
    return true;
}

EffectMeterSnapshot AudioRuntime::effectMeterSnapshot(const AudioPluginAddress& address) {
    auto* instance = pluginInstance(address);
    EffectMeterSnapshot out;
    if (auto* plugin = dynamic_cast<plugins::compressor::CompressorInstance*>(instance)) {
        const auto values = plugin->consumeTelemetry();
        out = {true, values.input, values.output, values.reduction};
    } else if (auto* plugin = dynamic_cast<plugins::cla2a::Cla2aInstance*>(instance)) {
        const auto values = plugin->consumeTelemetry();
        out = {true, values.input, values.output, values.reduction};
    } else if (auto* plugin = dynamic_cast<plugins::delay::DelayInstance*>(instance)) {
        const auto values = plugin->consumeTelemetry();
        out = {true, values.input, values.output, 0, values.wet};
    } else if (auto* plugin = dynamic_cast<plugins::graphit::GraphitInstance*>(instance)) {
        const auto values = plugin->consumeTelemetry();
        out = {true, std::max(values.inputLeft, values.inputRight),
            std::max(values.outputLeft, values.outputRight), values.gainReductionDb};
    } else if (auto* plugin = dynamic_cast<plugins::pitch::PitchCorrectorInstance*>(instance)) {
        const auto values = plugin->telemetrySnapshot();
        out.available = true;
        out.inputHz = values.inputHz; out.targetHz = values.targetHz;
        out.correctionCents = values.correctionCents;
        out.latencySamples = plugin->latencySamples();
        out.quality = plugin->activeQuality();
        out.qualityPending = plugin->qualityChangePending();
    }
    return out;
}

SamplerSnapshot AudioRuntime::samplerSnapshot(const AudioPluginAddress& address) const {
    auto* plugin = dynamic_cast<plugins::sampler::SamplerInstance*>(pluginInstance(address));
    if (!plugin) return {};
    return {true, plugin->precomputePending(), plugin->samplePath(),
            plugin->sampleName(), plugin->sample(), bool(plugin->rawSample())};
}

std::optional<SlicerSnapshot> AudioRuntime::slicerSnapshot(const AudioPluginAddress& address, bool includeActivity) const {
    const auto* plugin = dynamic_cast<plugins::slicer::SlicerInstance*>(pluginInstance(address));
    if (!plugin) return std::nullopt;
    SlicerSnapshot out{PluginIdentity{0, pluginInstanceId(address)}, plugin->captureState(),
                       plugin->sampleName(), plugin->sourceRevision()};
    if (includeActivity)
        for (int key = 0; key < 128; ++key) out.activeKeys[key] = plugin->keyActive(key);
    return out;
}

std::optional<EqualizerSnapshot> AudioRuntime::equalizerSnapshot(const AudioPluginAddress& address, bool consumeMeters) {
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin) return std::nullopt;
    EqualizerSnapshot out;
    for (std::uint32_t band = 0; band < out.bands.size(); ++band) out.bands[band] = plugin->bandState(band);
    out.analyzer = plugin->analyzerConfig();
    if (consumeMeters) out.telemetry = plugin->consumeTelemetry();
    out.comparison = plugin->activeComparison();
    out.preset = plugin->presetReference();
    return out;
}

std::optional<EqualizerResponse> AudioRuntime::equalizerResponse(const AudioPluginAddress& address) const {
    const auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(
        pluginInstance(address));
    if (!plugin) return std::nullopt;
    EqualizerResponse out;
    for (std::size_t i = 0; i < out.combined.size(); ++i)
        out.combined[i] = plugin->responseDb(10.0 * std::pow(3000.0, double(i) / (out.combined.size() - 1)));
    for (std::uint32_t band = 0; band < out.bands.size(); ++band) {
        if (!plugin->bandState(band).enabled) continue;
        auto& curve = out.bands[band];
        for (std::size_t i = 0; i < curve.size(); ++i)
            curve[i] = float(plugin->bandResponseDb(band, 10.0 * std::pow(3000.0, double(i) / (curve.size() - 1))));
    }
    return out;
}

std::optional<std::array<double, 180>> AudioRuntime::modulationResponse(const AudioPluginAddress& address) const {
    const auto* plugin = dynamic_cast<plugins::modulation::ModulationRackInstance*>(
        pluginInstance(address));
    if (!plugin) return std::nullopt;
    std::array<double, 180> out;
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = plugin->equalizer().responseDb(20.0 * std::pow(1000.0, double(i) / (out.size() - 1)));
    return out;
}

std::optional<GravitySnapshot> AudioRuntime::gravitySnapshot(const AudioPluginAddress& address) {
    auto* plugin = dynamic_cast<plugins::gravity::GravityInstance*>(pluginInstance(address));
    if (!plugin) return std::nullopt;
    return GravitySnapshot{plugin->consumeTelemetry(), plugin->frozen(), plugin->lastPreset(), plugin->presetReference()};
}

bool AudioRuntime::setInsertPresetReference(const AudioPluginAddress& address, std::string kind, std::string name) {
    auto* instance = pluginInstance(address);
    if (auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(instance))
        plugin->setPresetReference(std::move(kind), std::move(name));
    else if (auto* plugin = dynamic_cast<plugins::gravity::GravityInstance*>(instance))
        plugin->setPresetReference(std::move(kind), std::move(name));
    else if (auto* plugin = dynamic_cast<plugins::modulation::ModulationInstance*>(instance))
        plugin->setPresetReference(std::move(kind), std::move(name));
    else return false;
    return true;
}

bool AudioRuntime::setEqualizerAnalyzer(const AudioPluginAddress& address, const plugins::equalizer::AnalyzerConfig& config) {
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin) return false;
    plugin->setAnalyzerConfig(config);
    return true;
}

bool AudioRuntime::auditionEqualizerBand(const AudioPluginAddress& address, int band) {
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin) return false;
    plugin->setAuditionBand(band);
    return true;
}

bool AudioRuntime::copyEqualizerComparison(const AudioPluginAddress& address) {
    auto* plugin = dynamic_cast<plugins::equalizer::EqualizerInstance*>(pluginInstance(address));
    if (!plugin) return false;
    const char source = plugin->activeComparison();
    plugin->captureComparison(source);
    plugin->copyComparison(source, source == 'A' ? 'B' : 'A');
    return true;
}

bool AudioRuntime::setGravityFrozen(const AudioPluginAddress& address, bool frozen) {
    auto* plugin = dynamic_cast<plugins::gravity::GravityInstance*>(pluginInstance(address));
    if (!plugin) return false;
    plugin->setFrozen(frozen);
    return true;
}

bool AudioRuntime::clearGravityTail(const AudioPluginAddress& address) {
    auto* plugin = dynamic_cast<plugins::gravity::GravityInstance*>(pluginInstance(address));
    if (!plugin) return false;
    plugin->clearTail();
    return true;
}

std::vector<InsertParameter> AudioRuntime::pluginParameterValues(const AudioPluginAddress& address) const {
    std::vector<InsertParameter> values;
    const auto* plugin = pluginInstance(address);
    if (!plugin) return values;
    const auto parameters = plugin->parameters();
    values.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        if (parameter.id.empty()) continue;
        const auto value = plugin->parameterValue(parameter.index);
        if (std::isfinite(value))
            values.push_back({parameter.id, value, false});
    }
    return values;
}

bool AudioRuntime::loadInstrumentSample(const AudioPluginAddress& address, const std::string& path,
    std::shared_ptr<const engine::SampleBuffer> decoded) {
    preservePluginSourceForTransactions(address);
    auto* instance = pluginInstance(address);
    if (auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(instance))
        return decoded ? sampler->adoptSample(path, std::move(decoded)) : sampler->loadSample(path);
    if (auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(instance))
        return decoded ? slicer->adoptSample(path, std::move(decoded)) : slicer->loadSample(path);
    return false;
}

bool AudioRuntime::clearInstrumentSample(const AudioPluginAddress& address) {
    preservePluginSourceForTransactions(address);
    auto* instance = pluginInstance(address);
    if (auto* sampler = dynamic_cast<plugins::sampler::SamplerInstance*>(instance)) sampler->clearSample();
    else if (auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(instance)) slicer->clearSample();
    else return false;
    return true;
}

bool AudioRuntime::restoreSlicerState(const AudioPluginAddress& address, const plugins::slicer::ControlState& state) {
    auto* node = pluginNode(address);
    if (node && node->faultBypassed()) return false;
    auto* slicer = node ? dynamic_cast<plugins::slicer::SlicerInstance*>(node->instance()) : nullptr;
    if (!slicer) return false;
    preservePluginSourceForTransactions(address);
    const engine::RealtimeEngine::RenderGate gate(engine);
    node->discardPendingEvents();
    slicer->restoreState(state);
    return true;
}

bool AudioRuntime::setSlicerSlices(const AudioPluginAddress& address,
    std::shared_ptr<const plugins::slicer::SliceTable> table, const plugins::slicer::AnalysisSettings& settings) {
    auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(pluginInstance(address));
    if (!slicer) return false;
    preservePluginSourceForTransactions(address);
    slicer->setAnalysisSettings(settings);
    slicer->setSliceTable(std::move(table));
    return true;
}

bool AudioRuntime::setSlicerAnalysis(const AudioPluginAddress& address, const plugins::slicer::AnalysisSettings& settings) {
    auto* slicer = dynamic_cast<plugins::slicer::SlicerInstance*>(pluginInstance(address));
    if (!slicer) return false;
    preservePluginSourceForTransactions(address);
    slicer->setAnalysisSettings(settings);
    return true;
}

bool AudioRuntime::flushSamplerPrecompute(bool wait) {
    bool ready = true;
    const auto flush = [&](const auto& slots) {
        for (const auto& slot : slots)
            for (const auto& node : {slot.node, slot.rightNode})
                if (auto* sampler = node && !node->faultBypassed() ? dynamic_cast<plugins::sampler::SamplerInstance*>(node->instance()) : nullptr) {
                    if (wait) sampler->flushPendingPrecompute();
                    else sampler->pumpMainThread();
                    ready &= !sampler->precomputePending();
                }
    };
    for (const auto& [id, channel] : channels) {
        flush(channel.instrument); flush(channel.samplerInserts); flush(channel.inserts);
        for (const auto& [clipId, clip] : channel.clipFx) flush(clip.inserts);
    }
    return ready;
}

double AudioRuntime::pluginParameterIncludingPending(const AudioPluginAddress& address,
    const std::string& parameterId) {
    // A CLAP/AU host edit may still be queued for the next audio block.
    // Undo includes pending host edits; painting reads the rendered value
    // without parking the audio callback.
    const engine::RealtimeEngine::RenderGate gate(engine);
    auto* node = pluginNode(address);
    if (node && node->faultBypassed()) {
        const auto snapshot = pluginStateSnapshot(address, false);
        for (const auto& parameter : snapshot.pending) if (parameter.id == parameterId) return parameter.value;
        for (const auto& parameter : snapshot.parameters) if (parameter.id == parameterId) return parameter.value;
        return 0;
    }
    auto value = pluginParameter(address, parameterId);
    if (!node || !node->instance()) return value;
    const auto parameters = node->instance()->parameters();
    for (const auto& event : node->pendingParameterEvents()) {
        if (event.paramIndex < parameters.size() &&
            parameters[event.paramIndex].id == parameterId && std::isfinite(event.value))
            value = event.value;
    }
    return value;
}

} // namespace daw
