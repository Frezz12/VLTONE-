#pragma once

#include "EngineController.hpp"

namespace daw {

struct RenderSessionSpec::Data {
    rendering::Spec spec;
    recovery::RecoverySnapshot snapshot;
    PluginManager::CatalogSnapshot catalog;
    std::unordered_map<std::string, std::vector<InsertParameter>> pendingParameterOverrides;
    std::unordered_map<std::string, std::shared_ptr<const engine::SampleBuffer>> sourceSamples, samples;
    std::unordered_map<std::string, EngineController::ClipSampleCacheEntry> clipSamples;
    std::vector<EngineController::SharedClipSampleCacheEntry> sharedClipSamples;
    double sampleRate = 48000;
    std::uint32_t blockSize = 512;
    std::uint64_t revision = 0, generation = 0;
};

} // namespace daw
