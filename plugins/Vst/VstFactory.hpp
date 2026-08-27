#pragma once

#include "Host/PluginInstance.hpp"

namespace daw::plugins {

/// Finds and opens legacy AEffect-based VST 1.x/2.x plugins.
class VstFactory final : public PluginFactory {
public:
    Format format() const noexcept override { return Format::Vst; }
    std::vector<std::string> defaultSearchPaths() const override;
    std::vector<std::string> enumerateCandidates(
        const std::string& directory) const override;
    std::vector<PluginDescriptor> inspect(const std::string& path) const override;
    std::unique_ptr<PluginInstance> create(
        const PluginDescriptor& descriptor) override;
};

} // namespace daw::plugins
