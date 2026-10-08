#pragma once

#include "Host/PluginInstance.hpp"

namespace daw::plugins {

enum class HostingMode { Local, Isolated };

struct HostingConfiguration {
    HostingMode mode = HostingMode::Local;
    std::string executable;
};

/// The runtime and standalone previews use the same format dispatch. A failed
/// process launch never falls back to loading the external module locally.
std::unique_ptr<PluginInstance> createHostedPlugin(
    const PluginDescriptor& descriptor, const HostingConfiguration& hosting, std::string* error = nullptr);

} // namespace daw::plugins
