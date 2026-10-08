#pragma once

#include "Host/PluginTypes.hpp"
#include "Internal/EqualizerParams.hpp"
#include "Internal/GravityParams.hpp"
#include "Internal/SlicerParams.hpp"

#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace daw {

namespace plugins::sampler { struct SampleData; }

/// Control-thread identity, never an address. Undo, recovery and project load
/// may reuse a slot ID or an allocator address but never this pair.
struct PluginIdentity {
    std::uint64_t project = 0, instance = 0;
    explicit operator bool() const noexcept { return instance != 0; }
    friend bool operator==(const PluginIdentity&, const PluginIdentity&) = default;
};

/// Address inside one runtime session. Commands optionally name the expected
/// native instance so delayed edits cannot reach a replacement in the same slot.
struct AudioPluginAddress {
    std::string channelId, slotId;
    bool right = false;
    std::uint64_t instance = 0;
};

struct AudioPluginControlChange {
    std::optional<bool> bypassed;
    std::optional<float> mix;
};
struct AudioPluginCapabilities {
    bool available = false, dualMono = false, sidechain = false;
};
struct AudioPluginSlideStatus {
    int mode = 4; // SlideDelivery::Off
    bool overloaded = false, clipped = false;
};

struct PluginEditorSnapshot {
    PluginIdentity identity;
    std::string name, uid;
    plugins::Format format = plugins::Format::Unknown;
    bool remote = false, hasEditor = false, open = false;
    bool openFailed = false;
    std::uint64_t processId = 0;
    bool pending = false;
};

struct PluginEditorSize {
    std::uint32_t width = 0, height = 0;
    bool resizable = false;
};

/// UI-owned polling storage. The stable ID is authoritative; index is only a
/// validated lookup hint. Reused in place so idle polling copies no catalog.
struct PluginParameterReadout {
    std::string id;
    std::int32_t index = -1;
    double value = 0;
    bool available = false;
};

struct EqualizerSnapshot {
    std::array<plugins::equalizer::BandState, plugins::equalizer::kBandCount> bands{};
    plugins::equalizer::AnalyzerConfig analyzer;
    plugins::equalizer::Telemetry telemetry;
    char comparison = 'A';
    std::pair<std::string, std::string> preset;
};

/// Requested separately: curves are only recalculated when the UI cache changes.
struct EqualizerResponse {
    static constexpr int curvePoints = 240;
    std::array<double, 256> combined{};
    std::array<std::array<float, curvePoints>, plugins::equalizer::kBandCount> bands{};
};

struct GravitySnapshot {
    plugins::gravity::Telemetry telemetry;
    bool frozen = false;
    int lastPreset = 0;
    std::pair<std::string, std::string> preset;
};

struct SlicerSnapshot {
    PluginIdentity identity;
    plugins::slicer::ControlState state;
    std::string name;
    std::uint64_t sourceRevision = 0;
    std::bitset<128> activeKeys;
};

/// Control-thread value snapshot. No DSP pointers, borrowed spans or lifetime
/// tied to an editor. Peaks are consumed only when a visible meter asks for it.
struct EffectMeterSnapshot {
    bool available = false;
    float input = 0, output = 0, reduction = 0, wet = 0;
    double inputHz = 0, targetHz = 0, correctionCents = 0;
    std::uint32_t latencySamples = 0;
    int quality = 0;
    bool qualityPending = false;
};

struct SamplerSnapshot {
    bool available = false;
    bool precomputePending = false;
    std::string path, name;
    /// Shared immutable resource, retained even if the source slot is removed.
    std::shared_ptr<const plugins::sampler::SampleData> sample;
    /// The decoded source is present even while processed audio is rebuilding.
    bool hasSource = false;
};

} // namespace daw
