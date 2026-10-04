#include "Internal/InternalFactory.hpp"
#include "Internal/CompressorInstance.hpp"
#include "Internal/Cla2aInstance.hpp"
#include "Internal/ChannelColorInstance.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/DelayInstance.hpp"

#include "Internal/GravityInstance.hpp"
#include "Internal/GraphitInstance.hpp"
#include "Internal/ModulationInstance.hpp"
#include "Internal/ModulationRackInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SlicerInstance.hpp"
#include <mutex>

namespace daw::plugins {
namespace {

/// The one installed decoder. A `std::function` and not a raw pointer because
/// the application hands over a lambda closing on its own file layer.
sampler::SampleDecodeFn& decoderSlot() {
    static sampler::SampleDecodeFn decoder;
    return decoder;
}
std::mutex& decoderMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

namespace sampler {

void setSampleDecoder(SampleDecodeFn decoder) {
    const std::lock_guard lock(decoderMutex());
    decoderSlot() = std::move(decoder);
}

std::shared_ptr<const engine::SampleBuffer> decodeSample(const std::string& path) {
    SampleDecodeFn decoder;
    {
        const std::lock_guard lock(decoderMutex());
        decoder = decoderSlot();
    }
    if (!decoder || path.empty()) return nullptr;
    return decoder(path);
}

} // namespace sampler

std::vector<std::string> InternalFactory::enumerateCandidates(const std::string&) const {
    // Nothing on disk to find. Returning nothing here is what keeps a scan of
    // the user's plugin folders from ever touching the built-ins.
    return {};
}

std::vector<PluginDescriptor> InternalFactory::inspect(const std::string& path) const {
    std::vector<PluginDescriptor> found;
    // Channel-owned stage: resolvable for saved state, absent from FX pickers.
    if (path == channel_color::ChannelColorInstance::uid())
        found.push_back(channel_color::ChannelColorInstance::staticDescriptor());
    for (const PluginDescriptor& descriptor : builtinPlugins()) {
        if (path.empty() || path == descriptor.path || path == descriptor.uid) {
            found.push_back(descriptor);
        }
    }
    return found;
}

std::unique_ptr<PluginInstance> InternalFactory::create(const PluginDescriptor& descriptor) {
    if (descriptor.uid == channel_color::ChannelColorInstance::uid()) return std::make_unique<channel_color::ChannelColorInstance>();
    if (descriptor.uid == cla2a::Cla2aInstance::uid()) return std::make_unique<cla2a::Cla2aInstance>();
    if (descriptor.uid == delay::DelayInstance::uid()) return std::make_unique<delay::DelayInstance>();
    if (descriptor.uid == compressor::CompressorInstance::uid()) {
        return std::make_unique<compressor::CompressorInstance>();
    }
    if (descriptor.uid == pitch::PitchCorrectorInstance::uid()) {
        return std::make_unique<pitch::PitchCorrectorInstance>();
    }
    if (descriptor.uid == equalizer::EqualizerInstance::uid()) {
        return std::make_unique<equalizer::EqualizerInstance>();
    }
    if (descriptor.uid == gravity::GravityInstance::uid()) {
        return std::make_unique<gravity::GravityInstance>();
    }
    if (descriptor.uid == graphit::GraphitInstance::uid()) {
        return std::make_unique<graphit::GraphitInstance>();
    }
    if (descriptor.uid == mini::kUid) return std::make_unique<mini::MiniModuleInstance>();
    if (descriptor.uid == "daw.modulation") return std::make_unique<modulation::ModulationRackInstance>();
    if (descriptor.uid == "daw.doubler") return std::make_unique<modulation::DoublerInstance>();
    if (descriptor.uid == "daw.doubler-pro") return std::make_unique<modulation::DoublerProInstance>();
    if (descriptor.uid == "daw.chorus") return std::make_unique<modulation::ChorusInstance>();
    if (descriptor.uid == "daw.flanger") return std::make_unique<modulation::FlangerInstance>();
    if (descriptor.uid == "daw.phaser") return std::make_unique<modulation::PhaserInstance>();
    if (descriptor.uid == sampler::SamplerInstance::uid()) {
        return std::make_unique<sampler::SamplerInstance>();
    }
    if (descriptor.uid == slicer::SlicerInstance::uid()) {
        return std::make_unique<slicer::SlicerInstance>();
    }
    return nullptr;
}

std::vector<PluginDescriptor> builtinPlugins() {
    return {sampler::SamplerInstance::staticDescriptor(),
            slicer::SlicerInstance::staticDescriptor(),
            delay::DelayInstance::staticDescriptor(),
            compressor::CompressorInstance::staticDescriptor(),
            cla2a::Cla2aInstance::staticDescriptor(),
            channel_color::ChannelColorInstance::staticDescriptor(),
            pitch::PitchCorrectorInstance::staticDescriptor(),
            equalizer::EqualizerInstance::staticDescriptor(),
            gravity::GravityInstance::staticDescriptor(),
            graphit::GraphitInstance::staticDescriptor(),
            modulation::descriptorFor(modulation::Kind::Doubler),
            modulation::descriptorFor(modulation::Kind::DoublerPro),
            modulation::descriptorFor(modulation::Kind::Chorus),
            modulation::descriptorFor(modulation::Kind::Flanger),
            modulation::descriptorFor(modulation::Kind::Phaser),
            modulation::ModulationRackInstance::staticDescriptor()};
}

} // namespace daw::plugins
