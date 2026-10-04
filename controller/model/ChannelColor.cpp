#include "model/ChannelColor.hpp"
#include <charconv>
#include <cstdio>

namespace daw {
namespace {
std::uint64_t hash(const std::string& id, std::uint64_t initial) noexcept {
    for (unsigned char c : id) { initial ^= c; initial *= 1099511628211ULL; }
    return initial;
}
std::string hex(std::uint64_t value) {
    char out[17]; std::snprintf(out,sizeof(out),"%016llx",static_cast<unsigned long long>(value)); return out;
}
}
bool supportsChannelColor(TrackKind kind) noexcept {
    return kind==TrackKind::Audio || kind==TrackKind::Midi || kind==TrackKind::Instrument;
}
std::uint64_t channelColorSeed(const std::string& id) noexcept {
    return hash(id,14695981039346656037ULL);
}
std::uint64_t parseChannelColorSeed(const std::string& text) noexcept {
    std::uint64_t value=0;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value,16);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size()?value:0;
}
std::string channelColorSlotId(const std::string& id) {
    // Domain-separated hashes: all peers agree on a virtual slot before its
    // first saved edit. This is an entity id, not a security fingerprint.
    auto digits=hex(channelColorSeed(id))+hex(hash(id,0x434f4c4f522d5631ULL));
    digits[12]='4'; digits[16]='8';
    return digits.substr(0,8)+"-"+digits.substr(8,4)+"-"+digits.substr(12,4)+"-"+digits.substr(16,4)+"-"+digits.substr(20);
}
InsertModel defaultChannelColor(const std::string& id) {
    InsertModel slot;
    slot.id=channelColorSlotId(id); slot.name="COLOR"; slot.bypassed=true;
    slot.format=PluginFormat::Internal; slot.uid=slot.path="daw.channel-color";
    slot.vendor="VLTONE"; slot.pluginVersion="1.0"; slot.stateSchemaVersion=1;
    slot.profileSeed=hex(channelColorSeed(id));
    slot.parameters={{"drive",0},{"tone",0}};
    return slot;
}
} // namespace daw
