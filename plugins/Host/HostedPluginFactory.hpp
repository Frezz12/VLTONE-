#pragma once
#include "Host/PluginInstance.hpp"

namespace daw::plugins {
/// Shared format dispatch for live, preview and offline instances.
std::unique_ptr<PluginInstance> createHostedPlugin(
    const PluginDescriptor& descriptor, std::string* error = nullptr);
}
