#pragma once

#include "Host/PluginInstance.hpp"

#include <cstdint>

namespace daw::plugins {

namespace au {
/// An Audio Unit's identity is a triple of four-character codes. They are
/// written as hex rather than as the characters themselves: the codes are
/// arbitrary 32-bit values and plenty of real ones contain bytes that are not
/// printable, which a project file would then mangle.
std::string identityToString(std::uint32_t type, std::uint32_t subtype,
                             std::uint32_t manufacturer);
bool identityFromString(const std::string& text, std::uint32_t& type,
                        std::uint32_t& subtype, std::uint32_t& manufacturer);
} // namespace au

/// Finds and opens Audio Units.
///
/// Unlike CLAP and VST3, an AU is not opened by path: components are registered
/// with the system and found through `AudioComponentFindNext`. The scan still
/// walks directories, because that is what gives the cache something to
/// invalidate against — but what it reads out of a `.component` is its
/// `Info.plist`, and what `create` uses is the identity, not the path. A plugin
/// that moved therefore still loads.
class AuFactory final : public PluginFactory {
public:
    Format format() const noexcept override { return Format::AudioUnit; }
    std::vector<std::string> defaultSearchPaths() const override;
    std::vector<std::string> enumerateCandidates(const std::string& directory) const override;
    std::vector<PluginDescriptor> inspect(const std::string& path) const override;
    std::unique_ptr<PluginInstance> create(const PluginDescriptor& descriptor) override;
};

} // namespace daw::plugins
