#pragma once

#include <cstdint>

namespace daw::plugins {

/// Editor → host notifications, delivered on the control thread.
///
/// Separate from `PluginListener` because these are UI-thread-only and a
/// plugin without an open editor never produces them, whereas the listener has
/// to stay valid for as long as the plugin is loaded.
class PluginEditorHost {
public:
    virtual ~PluginEditorHost() = default;

    /// The plugin wants its window to be this size, in logical pixels. The
    /// host is free to refuse, and must tell the plugin what it settled on.
    virtual void onEditorResized(std::uint32_t width, std::uint32_t height) noexcept = 0;
    /// The plugin closed its own editor. The host should destroy the window;
    /// it must not call `closeEditor` from inside this callback.
    virtual void onEditorClosed() noexcept = 0;
    virtual double contentScaleFactor() const noexcept { return 1.0; }
};

} // namespace daw::plugins
