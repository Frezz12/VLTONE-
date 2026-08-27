#pragma once

#include "DSP/Simd.hpp"
#include "Graph/Node.hpp"

#include <string>

namespace daw::engine {

/// Selects one channel from every main input, sums it, and repeats it to the
/// graph's stereo width. Two of these form the left/right entrance to a true
/// dual-mono plugin slot.
class ChannelSelectNode final : public Node {
public:
    ChannelSelectNode(ChannelCount selected, std::string name)
        : m_selected(selected), m_name(std::move(name)) {}

    std::string_view name() const noexcept override { return m_name; }

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

    std::string_view name() const noexcept override { return m_name; }

    void process(const ProcessContext& context) override {
        for (ChannelCount channel = 0;
             channel < context.output.numChannels(); ++channel) {
            const std::span<float> out = context.output.channel(channel);
            const std::size_t branch = channel == 0 ? 0 : 1;
            if (branch >= context.inputs.size() ||
                context.inputs[branch].numChannels() == 0) {
                dsp::clear(out);
                continue;
            }
            dsp::copyScaled(out, context.inputs[branch].channel(0), 1.0f);
        }

        if (!context.midiOutput || context.midiInputs.empty() ||
            !context.midiInputs.front()) {
            return;
        }
        for (const MidiEvent& event : context.midiInputs.front()->events()) {
            (void)context.midiOutput->push(event);
        }
    }

private:
    std::string m_name;
};

} // namespace daw::engine
