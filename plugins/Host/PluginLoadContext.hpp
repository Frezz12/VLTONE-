#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>

namespace daw::plugins {

/// Fixed storage that the crash handler can read without calling back into
/// the controller or retaining memory owned by the plugin being loaded.
struct PluginLoadContext {
    char name[128]{};
    std::atomic<bool> active{false};

    void enter(std::string_view value) noexcept {
        const auto length = std::min(value.size(), sizeof(name) - 1);
        std::memcpy(name, value.data(), length);
        name[length] = '\0';
        active.store(true, std::memory_order_release);
    }
    void leave() noexcept { active.store(false, std::memory_order_release); }
};

inline PluginLoadContext pluginLoadContext;

} // namespace daw::plugins
