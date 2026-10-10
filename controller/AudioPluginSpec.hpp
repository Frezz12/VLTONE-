#pragma once

#include "Host/PluginTypes.hpp"
#include "model/Document.hpp"

// This framework-independent spec is also included by Qt editor translation
// units, where the signal/slot keyword macro must not erase the member name.
#pragma push_macro("slots")
#undef slots

namespace daw {

/// Required is used for explicit insert/replace/activation. Imported document
/// slots may preserve unavailable processors; blocked state must not be loaded.
enum class AudioPluginLoadPolicy { Required, PreserveUnavailable, PlaceholderOnly };

/// Resolved control-thread input. Project/cloud policy and catalog lookup have
/// already run; the runtime needs no document or callbacks to create a slot.
struct AudioPluginSpec {
    std::string id, uid, name;
    plugins::PluginDescriptor descriptor;
    plugins::Format requiredFormat = plugins::Format::Unknown;
    bool requireExactVersion = false;
    std::string requiredVersion, requiredParameterFingerprint;
    std::vector<InsertParameter> parameters, rightParameters;
    std::optional<plugins::mini::MiniModuleDefinition> miniModule;
    std::string miniModuleMode;
    std::uint64_t profileSeed = 0;
    PluginChannelMode channelMode = PluginChannelMode::Auto;
    std::uint16_t preferredChannels = 2;
    int slideDelivery = 0;
    double slideBendRange = 2.0, slideReleaseReserve = 2.0;
    float mix = 1.0f;
    bool bypassed = false;
    AudioPluginLoadPolicy loadPolicy = AudioPluginLoadPolicy::Required;
    std::string unavailableReason;
};

struct AudioPluginChainSpec {
    enum class Kind { Inserts, Instrument, MiniModules, SamplerInserts, ClipFx };
    std::string channelId;
    Kind kind = Kind::Inserts;
    std::string clipId;
    std::vector<AudioPluginSpec> slots;
};

} // namespace daw

#pragma pop_macro("slots")
