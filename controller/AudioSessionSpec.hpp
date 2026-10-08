#pragma once

#include "AudioGraphSpec.hpp"
#include "AudioPluginSpec.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include "Nodes/MidiClipPlayerNode.hpp"
#include "Nodes/BasicNodes.hpp"

#include <optional>
#include <unordered_map>

namespace daw {

/// Prepared playback data, applied on the control thread. An absent section
/// leaves it unchanged; a present empty section clears it. Resources and lists
/// are immutable after publication and can outlive the originating document.
struct AudioContentSpec {
    struct Clips {
        std::shared_ptr<const engine::ClipPlayerNode::ClipList> shared;
        std::unordered_map<std::string, std::shared_ptr<const engine::ClipPlayerNode::ClipList>> individual;
    };
    struct Midi {
        std::shared_ptr<const engine::MidiClipPlayerNode::NoteList> notes;
        std::shared_ptr<const engine::MidiClipPlayerNode::ControlCurves> controllers;
        bool timelineSuppressed = false;
    };
    struct PluginCurve {
        std::string slotId, parameterId;
        double defaultValue = 0;
        // Timeline beats and normalized values. Runtime resolves stable IDs
        // and converts to each instance's plain units, including Dual Mono.
        std::vector<std::pair<double, double>> points;
    };
    using PluginCurves = std::vector<PluginCurve>;
    struct Levels {
        std::shared_ptr<const engine::LevelAutomation> fader;
        std::unordered_map<std::string, std::shared_ptr<const engine::LevelCurve>> sends;
    };

    std::optional<Clips> clips;
    std::optional<Midi> midi;
    std::optional<PluginCurves> plugins;
    std::optional<Levels> levels;
};

/// Resolved plugin slots, topology and prepared playback. This local resource
/// description is not an IPC wire format.
struct AudioSessionSpec {
    plugins::HostingConfiguration hosting;
    std::vector<AudioPluginChainSpec> pluginChains;
    AudioGraphSpec graph;
    struct Channel {
        std::string id;
        AudioContentSpec content;
    };
    std::vector<Channel> channels;
};

} // namespace daw
