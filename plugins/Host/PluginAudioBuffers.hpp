#pragma once

#include "Host/PluginTypes.hpp"

#include <algorithm>

namespace daw::plugins {

inline void clearPluginOutputs(const PluginProcessContext& context) noexcept {
    if (!context.outputs) return;
    for (std::uint16_t ch = 0; ch < context.outputChannels; ++ch)
        if (context.outputs[ch]) std::fill_n(context.outputs[ch], context.frames, 0.0f);
}

/// Planar storage for a format's declared bus. Allocate only while inactive;
/// bind every block because the supplied pointers and silent channels change.
struct PluginAudioBus {
    std::uint32_t channels = 0;
    std::vector<float*> pointers;
    std::vector<float> storage;

    void allocate(std::uint32_t count, std::uint32_t maxBlockSize) {
        channels = count;
        storage.assign(std::size_t(count) * maxBlockSize, 0.0f);
        pointers.resize(count);
        for (std::uint32_t ch = 0; ch < count; ++ch)
            pointers[ch] = storage.data() + std::size_t(ch) * maxBlockSize;
    }

    std::uint64_t bindInput(const PluginProcessContext& context, std::size_t bus,
                            std::uint32_t maxBlockSize) noexcept {
        const auto* source = bus == 0 ? context.inputs : (bus == 1 ? context.sidechainInputs : nullptr);
        const std::uint32_t supplied = bus == 0 ? context.inputChannels : (bus == 1 ? context.sidechainInputChannels : 0);
        const auto sourceSilence = bus == 0 ? context.inputSilenceMask : context.sidechainSilenceMask;
        std::uint64_t silent = 0;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const bool missing = !source || ch >= supplied || !source[ch];
            if (missing) {
                pointers[ch] = storage.data() + std::size_t(ch) * maxBlockSize;
                // Native APIs expose mutable input pointers. Do not retain
                // data a processor may have written to last block's scratch.
                std::fill_n(pointers[ch], context.frames, 0.0f);
            } else {
                pointers[ch] = const_cast<float*>(source[ch]);
            }
            if (ch < 64 && (missing || (sourceSilence & (std::uint64_t{1} << ch))))
                silent |= std::uint64_t{1} << ch;
        }
        return silent;
    }

    void bindOutput(const PluginProcessContext& context, std::size_t bus,
                    std::uint32_t maxBlockSize) noexcept {
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            pointers[ch] = bus == 0 && context.outputs && ch < context.outputChannels && context.outputs[ch]
                ? context.outputs[ch] : storage.data() + std::size_t(ch) * maxBlockSize;
    }
};

} // namespace daw::plugins
