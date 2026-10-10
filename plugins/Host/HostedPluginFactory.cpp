#include "HostedPluginFactory.hpp"
#include "PluginLoadContext.hpp"

namespace daw::plugins {
std::unique_ptr<PluginInstance> createHostedPlugin(const PluginDescriptor& descriptor, std::string* error) {
    if (error) error->clear();
    auto* factory = factoryFor(descriptor.format);
    if (!factory) {
        if (error) *error = "Plugin format is unavailable.";
        return {};
    }
    pluginLoadContext.enter(descriptor.name);
    struct ClearMarker { ~ClearMarker() { pluginLoadContext.leave(); } } clear;
    return factory->create(descriptor);
}
}
