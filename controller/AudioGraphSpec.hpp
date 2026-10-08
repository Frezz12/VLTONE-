#pragma once

#include "model/Document.hpp"
#include "Common/Types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace daw::engine { class SampleBuffer; }

namespace daw {

/// Control-thread input for topology assembly. Owns routing values and shares
/// immutable decoded resources; contains no document, UI or plugin pointers.
/// Plugin instances are reconciled separately before applying this description.
struct AudioGraphSpec {
    static constexpr const char* masterChannelId = "master";

    struct MiniModuleRoute {
        std::string slotId;
        bool postFx = false;
    };
    struct SidechainRoute {
        std::string slotId;
        // The document projection has already excluded feedback sources.
        std::vector<std::string> sourceChannelIds;
    };
    struct ClipFx {
        std::string id, name;
        float gain = 1.0f, pan = 0.0f;
        PlaybackInjection playbackInjection;
    };
    struct Input {
        bool present = false, enabled = false;
        std::uint32_t channel = 0, channelCount = 1;
        unsigned monitorMask = 3;
    };
    struct Channel {
        std::string id, name, outputBusId;
        bool acceptsMidi = false, capturing = false;
        Input input;
        float volume = 1.0f, pan = 0.0f;
        bool silent = false, mono = false;
        bool samplerOwned = false;
        float samplerVolume = 1.0f, samplerPan = 0.0f;
        std::vector<SendModel> sends;
        std::vector<MiniModuleRoute> miniModules;
        std::vector<SidechainRoute> sidechains;
        std::vector<ClipFx> clipFx;
        std::shared_ptr<const engine::SampleBuffer> frozenAudio;
        engine::SamplePos frozenFrames = 0;
    };

    std::vector<Channel> channels;
    float masterVolume = 1.0f, masterPan = 0.0f;
    std::vector<MiniModuleRoute> masterMiniModules;
    std::vector<SidechainRoute> masterSidechains;
    bool metronomeEnabled = false;
    std::string auditionCapture;
};

} // namespace daw
