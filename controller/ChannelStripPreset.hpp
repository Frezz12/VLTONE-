#pragma once

#include "EngineController.hpp"

#include "Core/Result.hpp"

#include <string>

namespace daw {

/// Serializer for one portable Channel Strip template. A `.vlts` file is a
/// compact CBOR document containing the insert descriptions, each plugin's
/// opaque state chunks, and the channel's volume/pan. Sends and routing are
/// intentionally not part of this format.
class ChannelStripPreset {
public:
    static constexpr const char* kExtension = "vlts";
    static constexpr const char* kFormat = "VLTS";
    static constexpr int kFormatVersion = 1;

    static audio::Result save(const EngineController::ChannelSnapshot& snapshot,
                              const std::string& filePath);
    static audio::Result load(EngineController::ChannelSnapshot& out,
                              const std::string& filePath);
};

} // namespace daw
