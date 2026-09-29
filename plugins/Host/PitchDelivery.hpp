#pragma once
#include "Host/PluginInstance.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace daw::plugins {
// One adapter owns voice/channel assignment and musical-unit bend composition.
// Storage is allocated in prepare; process only visits fixed voices and
// scratch.
class PitchDelivery {
  public:
    static constexpr std::size_t polyphony = 128;
    void prepare(std::size_t capacity) {
        m_events.reserve(capacity);
        m_outputIdentities.reserve(capacity);
        reset();
    }
    void reset() noexcept {
        m_voices = {};
        m_outputIdentities.clear();
        m_bend = {};
        m_clock = 0;
        m_serial = 0;
        m_overloadUntil = 0;
        m_clipUntil = 0;
        m_mode = SlideDelivery::Auto;
        m_lastBend.fill(1e100);
        overloaded = clipped = false;
    }
    static SlideDelivery resolve(SlideDelivery requested, PitchCapabilities caps) noexcept {
        if (caps.continuous)
            return requested == SlideDelivery::Off ? SlideDelivery::Off
                                                   : SlideDelivery::NoteExpression;
        if (requested == SlideDelivery::Auto)
            return caps.perNote     ? SlideDelivery::NoteExpression
                   : caps.mpe       ? SlideDelivery::MPE
                   : caps.pitchBend ? SlideDelivery::PitchBend
                                    : SlideDelivery::Off;
        if (requested == SlideDelivery::NoteExpression && !caps.perNote)
            return SlideDelivery::Off;
        if ((requested == SlideDelivery::PitchBend || requested == SlideDelivery::MPE) &&
            !caps.pitchBend && !caps.mpe)
            return SlideDelivery::Off;
        return requested;
    }
    void voiceEnded(std::int32_t id, int channel, int key) noexcept {
        for (auto &v : m_voices)
            if (v.used && (id >= 0 ? v.destinationId == id
                                   : (!v.held && v.destination == channel && v.key == key)))
                v.used = false;
    }
    std::int32_t sourceNoteId(std::int32_t destinationId) const noexcept {
        // Keep mappings for this whole process call, including voices which
        // ended/reused their slot before the plugin emitted its MIDI output.
        if (destinationId >= 0)
            for (const auto &[pluginId, sourceId] : m_outputIdentities)
                if (pluginId == destinationId)
                    return sourceId;
        return destinationId;
    }
    bool overloaded = false, clipped = false;
    std::span<const PluginEvent> process(std::span<const PluginEvent> input, std::uint32_t frames,
                                         double rate, SlideDelivery mode, double range,
                                         double reserveSeconds, std::uint32_t tail,
                                         bool continuous) noexcept {
        if (continuous && mode == SlideDelivery::NoteExpression)
            return input;
        m_events.clear();
        m_outputIdentities.clear();
        for (const auto &v : m_voices)
            if (v.used)
                m_outputIdentities.emplace_back(v.destinationId, v.id);
        overloaded = m_clock < m_overloadUntil;
        clipped = m_clock < m_clipUntil;
        range = std::max(1., range);
        auto push = [&](const PluginEvent &e) {
            if (m_events.size() < m_events.capacity())
                m_events.push_back(e);
            else
                overloaded = true;
        };
        auto bend = [&](int channel, double st, std::uint32_t frame) {
            clipped |= std::abs(st) > range;
            if (st == m_lastBend[channel])
                return;
            m_lastBend[channel] = st;
            PluginEvent e;
            e.kind = PluginEvent::Kind::MidiController;
            e.paramIndex = 129;
            e.channel = std::int16_t(channel);
            e.frameOffset = frame;
            const double unit = std::clamp(st / range, -1., 1.);
            e.value = (8192. + unit * (unit < 0 ? 8192. : 8191.)) / 16383.;
            push(e);
        };
        // A mode switch must release the old destination channels before
        // reassigning them.
        if (m_mode != SlideDelivery::Auto && m_mode != mode) {
            for (const auto &v : m_voices)
                if (v.used) {
                    PluginEvent end;
                    end.kind = PluginEvent::Kind::NoteChoke;
                    end.channel = m_mode == SlideDelivery::MPE ? v.destination : v.channel;
                    end.noteId = v.destinationId;
                    end.key = v.key;
                    end.preferMidi =
                        m_mode == SlideDelivery::MPE || m_mode == SlideDelivery::PitchBend;
                    push(end);
                }
            m_voices = {};
            m_lastBend.fill(1e100);
        }
        m_mode = mode;
        auto pitchAt = [&](const Voice &v, std::uint32_t frame) {
            return v.pitch.at(
                std::uint32_t(std::min<std::uint64_t>(0xffffffff, m_clock + frame - v.pitchAt)));
        };
        auto emitPitches = [&](std::uint32_t frame) {
            std::array<const Voice *, 16> winner{};
            for (auto &v : m_voices) {
                if (!v.used)
                    continue;
                if (!v.held && m_clock + frame >= v.expires) {
                    if (mode == SlideDelivery::MPE)
                        bend(v.destination, 0, frame);
                    v.used = false;
                    continue;
                }
                const double value =
                    pitchAt(v, frame) + (v.held ? m_bend[v.channel] : v.releaseBend);
                if (mode == SlideDelivery::NoteExpression) {
                    clipped |= std::abs(value) > 120;
                    if (value == v.last)
                        continue;
                    PluginEvent e;
                    e.kind = PluginEvent::Kind::NotePitch;
                    e.frameOffset = frame;
                    e.channel = v.channel;
                    e.key = v.key;
                    e.noteId = v.destinationId;
                    e.value = value;
                    e.pitch.active = true;
                    e.pitch.from = e.pitch.to = value;
                    push(e);
                    v.last = value;
                } else if (mode == SlideDelivery::MPE)
                    bend(v.destination, value, frame);
                else if (mode == SlideDelivery::PitchBend &&
                         (!winner[v.channel] ||
                          winner[v.channel]->pitch.priority < v.pitch.priority ||
                          (winner[v.channel]->pitch.priority == v.pitch.priority &&
                           winner[v.channel]->serial < v.serial)))
                    winner[v.channel] = &v;
            }
            if (mode == SlideDelivery::PitchBend)
                for (int ch = 0; ch < 16; ++ch)
                    bend(ch, m_bend[ch] + (winner[ch] ? pitchAt(*winner[ch], frame) : 0), frame);
        };
        const double samplesPerTick = std::max(1., rate / 1000.);
        std::uint32_t frame = 0;
        std::size_t index = 0;
        while (frame < frames) {
            while (index < input.size() && input[index].frameOffset <= frame) {
                auto e = input[index++];
                if (e.kind == PluginEvent::Kind::ParamValue ||
                    e.kind == PluginEvent::Kind::ParamGestureBegin ||
                    e.kind == PluginEvent::Kind::ParamGestureEnd) {
                    push(e);
                    continue;
                }
                if (mode == SlideDelivery::Off) {
                    if (e.kind == PluginEvent::Kind::NotePitch) {
                        if (continuous) {
                            e.pitch = {};
                            e.pitch.edited = true;
                            e.value = 0;
                            push(e);
                        }
                    } else
                        push(e);
                    continue;
                }
                if (e.channel < 0 || e.channel >= 16) {
                    push(e);
                    continue;
                }
                auto found = std::find_if(m_voices.begin(), m_voices.end(), [&](const auto &v) {
                    return v.used && v.held &&
                           ((e.noteId >= 0 && v.id == e.noteId && v.channel == e.channel) ||
                            (e.noteId < 0 && v.held && v.key == e.key && v.channel == e.channel));
                });
                e.preferMidi = mode == SlideDelivery::MPE || mode == SlideDelivery::PitchBend;
                if (e.kind == PluginEvent::Kind::NoteOn) {
                    auto free = std::find_if(m_voices.begin(), m_voices.end(), [&](const auto &v) {
                        return !v.used || (!v.held && m_clock + frame >= v.expires);
                    });
                    if (free == m_voices.end()) {
                        overloaded = true;
                        continue;
                    }
                    int member = e.channel;
                    if (mode == SlideDelivery::MPE) {
                        member = 1;
                        for (; member < 16; ++member)
                            if (std::none_of(m_voices.begin(), m_voices.end(), [&](const auto &v) {
                                    return v.used && (v.held || m_clock + frame < v.expires) &&
                                           v.destination == member;
                                }))
                                break;
                        if (member == 16) {
                            overloaded = true;
                            continue;
                        }
                    }
                    *free = {};
                    free->used = free->held = true;
                    free->id = e.noteId;
                    free->key = e.key;
                    free->channel = e.channel;
                    free->destination = std::int16_t(member);
                    free->serial = ++m_serial;
                    free->destinationId = std::int32_t(m_serial & 0x7fffffff);
                    if (m_outputIdentities.size() < m_outputIdentities.capacity())
                        m_outputIdentities.emplace_back(free->destinationId, free->id);
                    e.noteId = free->destinationId;
                    free->pitchAt = m_clock + frame;
                    if (mode == SlideDelivery::MPE) {
                        bend(member, m_bend[e.channel], frame);
                        e.channel = std::int16_t(member);
                    }
                    push(e);
                } else if (e.kind == PluginEvent::Kind::NoteOff ||
                           e.kind == PluginEvent::Kind::NoteChoke) {
                    if (found != m_voices.end()) {
                        auto &v = *found;
                        double final = pitchAt(v, frame);
                        const double priority = v.pitch.priority;
                        v.pitch = {};
                        v.pitch.priority = priority;
                        v.pitch.from = v.pitch.to = final;
                        v.pitchAt = m_clock + frame;
                        emitPitches(frame);
                        v.held = false;
                        v.releaseBend = m_bend[v.channel];
                        e.noteId = v.destinationId;
                        const double seconds =
                            tail < 0x7fffffff ? double(tail) / rate : reserveSeconds;
                        v.expires = m_clock + frame + std::uint64_t(seconds * rate);
                        if (mode == SlideDelivery::MPE)
                            e.channel = v.destination;
                        push(e);
                    } else if (mode != SlideDelivery::MPE)
                        push(e);
                } else if (e.kind == PluginEvent::Kind::NotePitch) {
                    if (found != m_voices.end() && found->held) {
                        found->pitch = e.pitch;
                        found->pitchAt = m_clock + frame;
                    }
                } else if (e.kind == PluginEvent::Kind::MidiController && e.paramIndex == 129) {
                    const double raw = e.value * 16383. - 8192.;
                    m_bend[e.channel] = raw / (raw < 0 ? 8192. : 8191.) * range;
                } else if (mode == SlideDelivery::MPE) {
                    if (e.kind == PluginEvent::Kind::PolyPressure && found != m_voices.end()) {
                        e.channel = found->destination;
                        e.noteId = found->destinationId;
                        push(e);
                    } else {
                        if (e.kind == PluginEvent::Kind::MidiController &&
                            (e.paramIndex == 120 || e.paramIndex == 123)) {
                            for (auto &v : m_voices)
                                if (v.used && v.channel == e.channel) {
                                    auto end = e;
                                    end.channel = v.destination;
                                    push(end);
                                    v.used = false;
                                }
                        } else {
                            e.channel = 0;
                            push(e);
                        }
                    }
                } else {
                    if (e.kind == PluginEvent::Kind::MidiController &&
                        (e.paramIndex == 120 || e.paramIndex == 123))
                        for (auto &v : m_voices)
                            if (v.channel == e.channel)
                                v.used = false;
                    if (e.kind == PluginEvent::Kind::PolyPressure && found != m_voices.end())
                        e.noteId = found->destinationId;
                    push(e);
                }
            }
            if (mode != SlideDelivery::Off)
                emitPitches(frame);
            const auto absolute = m_clock + frame;
            const auto nextTick =
                std::uint32_t(std::max<double>(
                    1, std::ceil((std::floor(absolute / samplesPerTick) + 1) * samplesPerTick) -
                           absolute)) +
                frame;
            const auto nextEvent = index < input.size() ? input[index].frameOffset : frames;
            frame = std::max(frame + 1, std::min(nextTick, nextEvent));
        }
        if (overloaded && m_clock >= m_overloadUntil)
            m_overloadUntil = m_clock + std::uint64_t(rate);
        if (clipped && m_clock >= m_clipUntil)
            m_clipUntil = m_clock + std::uint64_t(rate);
        m_clock += frames;
        return m_events;
    }

  private:
    struct Voice {
        bool used = false, held = false;
        std::int32_t id = -1, destinationId = -1;
        std::int16_t key = 0, channel = 0, destination = 0;
        std::uint64_t pitchAt = 0, expires = 0, serial = 0;
        engine::PitchRamp pitch;
        double last = 1e100, releaseBend = 0;
    };
    std::array<Voice, polyphony> m_voices{};
    std::array<double, 16> m_bend{}, m_lastBend{};
    std::uint64_t m_clock = 0, m_serial = 0, m_overloadUntil = 0, m_clipUntil = 0;
    SlideDelivery m_mode = SlideDelivery::Auto;
    std::vector<PluginEvent> m_events;
    std::vector<std::pair<std::int32_t, std::int32_t>> m_outputIdentities;
};
} // namespace daw::plugins
