#pragma once

#include "AudioPluginSpec.hpp"
#include "PluginReadout.hpp"

#include <string>
#include <vector>

namespace daw {

struct AudioPluginNotice {
    enum class Kind { Parameter, GestureBegin, StateChanged, Overload };
    Kind kind = Kind::Parameter;
    AudioPluginAddress address;
    AudioPluginChainSpec::Kind chain = AudioPluginChainSpec::Kind::Inserts;
    std::string clipId, parameterId;
    double value = 0;
    bool touch = false;
};

struct AudioPluginServiceResult {
    // Structural changes only; parameter notices are compared to the document
    // by EngineController before marking it dirty.
    bool changed = false, scanned = false;
    std::vector<AudioPluginNotice> notices;
    std::string error;
};

} // namespace daw
