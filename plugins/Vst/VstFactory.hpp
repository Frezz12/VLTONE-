#pragma once

#include "Host/PluginInstance.hpp"
#include <string_view>

struct AEffect;

namespace daw::plugins {

namespace vst {
PluginDescriptor describeEffect(AEffect* effect, const std::string& path,
                                std::string_view shellName = {});
}

/// Finds and opens legacy AEffect-based VST 1.x/2.x plugins.
class VstFactory final : public PluginFactory {
public:
    Format format() const noexcept override { return Format::Vst; }
    std::vector<std::string> defaultSearchPaths() const override;
    std::vector<std::string> enumerateCandidates(
        const std::string& directory) const override;
    std::vector<PluginDescriptor> inspect(const std::string& path) const override;
    std::vector<PluginDescriptor> discover(const std::string& path) const override;
    std::unique_ptr<PluginInstance> create(
        const PluginDescriptor& descriptor) override;
};

} // namespace daw::plugins
