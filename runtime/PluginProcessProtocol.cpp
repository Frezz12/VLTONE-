#include "PluginProcessProtocol.hpp"

#include <algorithm>
#include <cmath>

namespace daw::plugins::ipc {

Event encode(const PluginEvent& e) noexcept {
    return {std::uint32_t(e.kind), e.frameOffset, e.sortOrder, e.paramIndex,
        e.channel, e.key, e.noteId,
        (e.preferMidi ? 1u : 0u) | (e.pitch.active ? 2u : 0u) | (e.pitch.edited ? 4u : 0u),
        e.value, e.notePan,
        {e.pitch.from, e.pitch.to, e.pitch.phaseFrom, e.pitch.phaseTo,
         e.pitch.tension, e.pitch.segmentStart, e.pitch.shapeFrom, e.pitch.shapeTo,
         e.pitch.priority}, e.pitch.frames, std::uint32_t(e.pitch.shape)};
}

bool decode(const Event& w, PluginEvent& e, std::uint32_t frames) noexcept {
    if (w.kind > std::uint32_t(PluginEvent::Kind::NoteEnd) || w.frame >= frames ||
        w.channel < -1 || w.channel > 15 || w.key < -1 || w.key > 127 ||
        (w.flags & ~7u) || !std::isfinite(w.value) || !std::isfinite(w.pan) ||
        w.pitchShape > std::uint32_t(engine::curve::Shape::SCurve) ||
        !std::all_of(std::begin(w.pitch), std::end(w.pitch), [](double v) { return std::isfinite(v); }))
        return false;
    e = {};
    e.kind = PluginEvent::Kind(w.kind); e.frameOffset = w.frame;
    e.sortOrder = w.order; e.paramIndex = w.parameter;
    e.channel = std::int16_t(w.channel); e.key = std::int16_t(w.key); e.noteId = w.note;
    e.preferMidi = (w.flags & 1) != 0; e.value = w.value; e.notePan = w.pan;
    e.pitch = {w.pitch[0], w.pitch[1], w.pitch[2], w.pitch[3], w.pitch[4], w.pitch[5],
        w.pitch[6], w.pitch[7], w.pitch[8], w.pitchFrames, engine::curve::Shape(w.pitchShape),
        (w.flags & 2) != 0, (w.flags & 4) != 0};
    return true;
}

bool validBlock(const Block& b, PluginProcessLimits l) noexcept {
    return b.frames > 0 && b.frames <= l.frames && b.inputs <= l.channels &&
        b.outputs <= l.channels && b.sidechains <= l.channels &&
        b.inputEvents <= l.events && b.outputEvents <= l.events && !(b.flags & ~31u) &&
        b.numerator > 0 && b.numerator <= 1024 && b.denominator > 0 && b.denominator <= 1024 &&
        std::isfinite(b.tempo) && b.tempo > 0 && std::isfinite(b.ppq) &&
        std::isfinite(b.bar) && std::isfinite(b.loopStart) && std::isfinite(b.loopEnd);
}

} // namespace daw::plugins::ipc
