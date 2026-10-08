#include "HostedPluginFactory.hpp"
#include "ProcessPluginInstance.hpp"
#include "Host/PluginLoadContext.hpp"

namespace daw::plugins {

std::unique_ptr<PluginInstance> createHostedPlugin(
    const PluginDescriptor& descriptor, const HostingConfiguration& hosting, std::string* error) {
    if (error) error->clear();
    const auto format = descriptor.format;
    const bool external = format == Format::Clap || format == Format::Vst3 ||
                          format == Format::Vst || format == Format::AudioUnit;
    if (hosting.mode == HostingMode::Isolated && external)
        return ProcessPluginInstance::create(descriptor, hosting.executable, error);
    auto* factory = factoryFor(format);
    if (!factory) return {};
    pluginLoadContext.enter(descriptor.name);
    struct ClearMarker { ~ClearMarker() { pluginLoadContext.leave(); } } clear;
    return factory->create(descriptor);
}

} // namespace daw::plugins
