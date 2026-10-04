#pragma once
#include "model/Document.hpp"

namespace daw {
bool supportsChannelColor(TrackKind) noexcept;
std::string channelColorSlotId(const std::string& trackId);
std::uint64_t channelColorSeed(const std::string& trackId) noexcept;
std::uint64_t parseChannelColorSeed(const std::string&) noexcept;
InsertModel defaultChannelColor(const std::string& trackId);
} // namespace daw
