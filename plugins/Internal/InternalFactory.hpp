#pragma once

#include "Host/PluginInstance.hpp"

namespace daw::plugins {

/// The plugins this build carries itself.
///
/// It wears the same `PluginFactory` interface as CLAP, VST3 and AU so that
/// everything downstream — the picker menus, the instrument slot, state chunks,
/// automation — treats a built-in exactly like a third-party plugin. What it
/// deliberately does *not* do is take part in scanning: there is no file to
/// find, `enumerateCandidates` returns nothing, and `builtinPlugins()` is how
/// the manager learns about these without a scan ever running.
class InternalFactory final : public PluginFactory {
public:
    Format format() const noexcept override { return Format::Internal; }
    std::vector<std::string> defaultSearchPaths() const override { return {}; }
    std::vector<std::string> enumerateCandidates(const std::string& directory) const override;
    std::vector<PluginDescriptor> inspect(const std::string& path) const override;
    std::unique_ptr<PluginInstance> create(const PluginDescriptor& descriptor) override;
};

/// Descriptors for every built-in, ready to be listed beside the scanned ones.
std::vector<PluginDescriptor> builtinPlugins();

} // namespace daw::plugins
