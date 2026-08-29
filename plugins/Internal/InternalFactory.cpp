#include "Internal/InternalFactory.hpp"

#include "Internal/GravityInstance.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/SampleDecoder.hpp"
#include "Internal/SamplerInstance.hpp"

namespace daw::plugins {
namespace {

/// The one installed decoder. A `std::function` and not a raw pointer because
/// the application hands over a lambda closing on its own file layer.
sampler::SampleDecodeFn& decoderSlot() {
    static sampler::SampleDecodeFn decoder;
    return decoder;
}

} // namespace

namespace sampler {

void setSampleDecoder(SampleDecodeFn decoder) { decoderSlot() = std::move(decoder); }

std::shared_ptr<const engine::SampleBuffer> decodeSample(const std::string& path) {
    const SampleDecodeFn& decoder = decoderSlot();
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
    for (const PluginDescriptor& descriptor : builtinPlugins()) {
        if (path.empty() || path == descriptor.path || path == descriptor.uid) {
            found.push_back(descriptor);
        }
    }
    return found;
}

std::unique_ptr<PluginInstance> InternalFactory::create(const PluginDescriptor& descriptor) {
    if (descriptor.uid == equalizer::EqualizerInstance::uid()) {
        return std::make_unique<equalizer::EqualizerInstance>();
    }
    if (descriptor.uid == gravity::GravityInstance::uid()) {
        return std::make_unique<gravity::GravityInstance>();
    }
    if (descriptor.uid == sampler::SamplerInstance::uid()) {
        return std::make_unique<sampler::SamplerInstance>();
    }
    return nullptr;
}

std::vector<PluginDescriptor> builtinPlugins() {
    return {sampler::SamplerInstance::staticDescriptor(),
            equalizer::EqualizerInstance::staticDescriptor(),
            gravity::GravityInstance::staticDescriptor()};
}

} // namespace daw::plugins
