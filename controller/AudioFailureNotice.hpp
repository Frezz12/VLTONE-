#pragma once

#include "PluginReadout.hpp"
#include <string>

namespace daw {
// Control-thread data only. No strings, observers or GUI calls reach the DSP.
struct AudioFailureNotice {
    enum class Source { Plugin, Device, Operation, Overload };
    enum class State { Waiting, Recovering, Failed, Recovered, Stopped, Retired };
    Source source = Source::Plugin;
    State state = State::Failed;
    std::uint64_t project = 0;
    std::uint64_t incident = 0;
    AudioPluginAddress address;
    std::string operation, name, channelName, detail;
    bool canRetry = false;
    bool requiresStop = false;
    bool recording = false;
    bool master = false, instrument = false;
};
}
