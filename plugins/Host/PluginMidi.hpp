#pragma once

#include "Host/PluginTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace daw::plugins {

/// The same MIDI 1 messages are carried by legacy VST, CLAP's MIDI dialect
/// and AU. Native note expressions stay in their format-specific adapters.
inline bool encodePluginMidi(const PluginEvent& event, std::array<std::uint8_t, 3>& bytes) noexcept {
    if (event.channel < 0 || event.channel >= 16 || !std::isfinite(event.value)) return false;
    bytes = {};
    const auto channel = std::uint8_t(event.channel);
    const auto value = std::uint8_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 127.0));
    switch (event.kind) {
        case PluginEvent::Kind::NoteOn:
        case PluginEvent::Kind::NoteOff:
        case PluginEvent::Kind::NoteChoke:
        case PluginEvent::Kind::PolyPressure:
            if (event.key < 0 || event.key >= 128) return false;
            bytes[0] = std::uint8_t(channel | (event.kind == PluginEvent::Kind::NoteOn ? 0x90 :
                event.kind == PluginEvent::Kind::PolyPressure ? 0xA0 : 0x80));
            bytes[1] = std::uint8_t(event.key);
            bytes[2] = event.kind == PluginEvent::Kind::NoteChoke ? 0 : value;
            return true;
        case PluginEvent::Kind::MidiController:
            if (event.paramIndex < 128) bytes = {std::uint8_t(0xB0 | channel), std::uint8_t(event.paramIndex), value};
            else if (event.paramIndex == 128) bytes = {std::uint8_t(0xD0 | channel), value, 0};
            else if (event.paramIndex == 129) {
                const auto bend = std::uint16_t(std::lround(std::clamp(event.value, 0.0, 1.0) * 16383.0));
                bytes = {std::uint8_t(0xE0 | channel), std::uint8_t(bend & 0x7F), std::uint8_t(bend >> 7)};
            } else if (event.paramIndex == 130) bytes = {std::uint8_t(0xC0 | channel), value, 0};
            else return false;
            return true;
        default: return false;
    }
}

inline bool decodePluginMidi(const std::uint8_t* bytes, PluginEvent& event) noexcept {
    event.channel = bytes[0] & 0x0F;
    event.noteId = -1;
    const auto first = std::uint8_t(bytes[1] & 0x7F), second = std::uint8_t(bytes[2] & 0x7F);
    switch (bytes[0] & 0xF0) {
        case 0x80: case 0x90: case 0xA0:
            event.kind = (bytes[0] & 0xF0) == 0xA0 ? PluginEvent::Kind::PolyPressure :
                ((bytes[0] & 0xF0) == 0x90 && second ? PluginEvent::Kind::NoteOn : PluginEvent::Kind::NoteOff);
            event.key = first; event.value = double(second) / 127.0;
            return true;
        case 0xB0: case 0xC0: case 0xD0: case 0xE0:
            event.kind = PluginEvent::Kind::MidiController;
            switch (bytes[0] & 0xF0) {
                case 0xB0: event.paramIndex = first; event.value = double(second) / 127.0; break;
                case 0xC0: event.paramIndex = 130; event.value = double(first) / 127.0; break;
                case 0xD0: event.paramIndex = 128; event.value = double(first) / 127.0; break;
                default: event.paramIndex = 129; event.value = double(first | (unsigned(second) << 7)) / 16383.0; break;
            }
            return true;
        default: return false;
    }
}

} // namespace daw::plugins
