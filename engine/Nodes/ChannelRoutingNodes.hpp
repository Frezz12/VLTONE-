#pragma once

#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"

#include <string>
#include <atomic>
#include <memory>
#include <array>
#include <limits>

namespace daw::engine {

/// Selects one channel from every main input, sums it, and repeats it to the
/// graph's stereo width. Two of these form the left/right entrance to a true
/// dual-mono plugin slot.
class ChannelSelectNode final : public Node {
public:
    ChannelSelectNode(ChannelCount selected, std::string name)
        : m_selected(selected), m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override { return OfflineNodePolicy::Ordered; }

    void process(const ProcessContext& context) override {
        for (ChannelCount outChannel = 0;
             outChannel < context.output.numChannels(); ++outChannel) {
            const std::span<float> out = context.output.channel(outChannel);
            bool wrote = false;
            for (std::size_t i = 0; i < context.inputs.size(); ++i) {
                if (i < context.inputRoles.size() &&
                    context.inputRoles[i] == InputRole::Sidechain) {
                    continue;
                }
                const AudioBlock& input = context.inputs[i];
                if (input.numChannels() == 0) continue;
                const ChannelCount source =
                    m_selected < input.numChannels() ? m_selected
                                                     : ChannelCount(0);
                if (wrote) {
                    dsp::addScaled(out, input.channel(source), 1.0f);
                } else {
                    dsp::copyScaled(out, input.channel(source), 1.0f);
                    wrote = true;
                }
            }
            if (!wrote) dsp::clear(out);
        }

        // Both branches may receive MIDI, but StereoMergeNode forwards only
        // the left branch so a transparent effect never duplicates notes.
        if (!context.midiOutput) return;
        for (const MidiBuffer* input : context.midiInputs) {
            if (!input) continue;
            for (const MidiEvent& event : input->events()) {
                (void)context.midiOutput->push(event);
            }
        }
    }

private:
    ChannelCount m_selected = 0;
    std::string m_name;
};

/// Reassembles the mono output of two independent plugin instances into a
/// stereo stream. Input order is left then right and is deterministic in the
/// compiled graph.
class StereoMergeNode final : public Node {
public:
    explicit StereoMergeNode(std::string name) : m_name(std::move(name)) {}
    void setSafetyFallback(std::shared_ptr<std::atomic<unsigned>> failure, bool instrument) {
        m_failure = std::move(failure); m_instrument = instrument;
    }

    std::string_view name() const noexcept override { return m_name; }
    OfflineNodePolicy offlineNodePolicy() const noexcept override { return OfflineNodePolicy::Ordered; }

    void process(const ProcessContext& context) override {
        const bool failed = m_failure && m_failure->load(std::memory_order_acquire) != 0;
        for (ChannelCount channel = 0;
             channel < context.output.numChannels(); ++channel) {
            const std::span<float> out = context.output.channel(channel);
            // The third edge carries the original stereo signal. Graph PDC
            // delays it to the slower branch, including the discovery block.
            if (failed) {
                if (!m_instrument && context.inputs.size() > 2 &&
                    context.inputs[2].numChannels() > channel)
                    dsp::copyScaled(out, context.inputs[2].channel(channel), 1.0f);
                else dsp::clear(out);
                continue;
            }
            const std::size_t branch = channel == 0 ? 0 : 1;
            if (branch >= context.inputs.size() ||
                context.inputs[branch].numChannels() == 0) {
                dsp::clear(out);
                continue;
            }
            dsp::copyScaled(out, context.inputs[branch].channel(0), 1.0f);
        }

        if (!context.midiOutput) return;
        if (failed && m_heldCount) {
            for (std::size_t i = 0; i < m_held.size(); ++i) {
                while (m_held[i]) {
                    if (!context.midiOutput->push(MidiEvent::noteOff(0, std::uint8_t(i / 128), std::uint8_t(i % 128)))) return;
                    --m_held[i]; --m_heldCount;
                }
            }
        }
        const std::size_t midiBranch = failed ? 2 : 0;
        if ((failed && m_instrument) || midiBranch >= context.midiInputs.size() || !context.midiInputs[midiBranch]) return;
        for (const MidiEvent& event : context.midiInputs[midiBranch]->events()) {
            if (!context.midiOutput->push(event)) break;
            if (!failed && (event.isNoteOn() || event.isNoteOff())) {
                auto& held = m_held[std::size_t(event.channel()) * 128 + (event.data1 & 0x7f)];
                if (event.isNoteOn() && held != std::numeric_limits<std::uint16_t>::max()) { ++held; ++m_heldCount; }
                else if (event.isNoteOff() && held) { --held; --m_heldCount; }
            }
        }
    }

private:
    std::string m_name;
    std::shared_ptr<std::atomic<unsigned>> m_failure;
    bool m_instrument = false;
    std::array<std::uint16_t, 16 * 128> m_held{};
    unsigned m_heldCount = 0;
};

} // namespace daw::engine
