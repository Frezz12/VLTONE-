#pragma once

#include "PluginInstance.hpp"
#include <cstdio>
#include <cstdlib>

namespace daw::plugins {

inline void logParameterWrite(const char* source, PluginInstance* instance,
                              std::int32_t index, double plainValue) {
    static const bool enabled = std::getenv("DAW_PLUGIN_DIAGNOSTICS") != nullptr;
    if (!enabled || !instance) return;
    const auto parameters = instance->parameters();
    const char* name = index >= 0 && std::size_t(index) < parameters.size()
        ? parameters[std::size_t(index)].name.c_str() : "?";
    std::fprintf(stderr, "[param] %-9s %s :: %s (index %d) <- %.6f\n", source,
                 instance->descriptor().name.c_str(), name, index, plainValue);
}

} // namespace daw::plugins
