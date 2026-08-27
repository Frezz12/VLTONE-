#pragma once

#include <pluginterfaces/base/ipluginbase.h>

#include <memory>
#include <string>

namespace daw::plugins {

/// One loaded VST3 module, with its platform entry point run.
///
/// Same ownership rule as `ClapModule`: the factory's descriptors and every
/// object it hands out stay valid only while the module is loaded, so each
/// instance holds a `shared_ptr` to it and the library stays mapped for exactly
/// as long as something is using it.
class Vst3Module {
public:
    ~Vst3Module();

    Vst3Module(const Vst3Module&) = delete;
    Vst3Module& operator=(const Vst3Module&) = delete;

    /// Open a `.vst3` bundle (macOS/Linux) or DLL (Windows). Null when it is
    /// not a VST3 module or its entry point refused.
    static std::shared_ptr<Vst3Module> open(const std::string& path);

    Steinberg::IPluginFactory* factory() const noexcept { return m_factory; }
    const std::string& path() const noexcept { return m_path; }

private:
    Vst3Module() = default;

    std::string m_path;
    void* m_handle = nullptr;
    /// macOS only: the `CFBundleRef` handed to `bundleEntry`, as an opaque
    /// pointer so this header stays free of CoreFoundation.
    void* m_bundle = nullptr;
    Steinberg::IPluginFactory* m_factory = nullptr;
    bool (*m_exit)() = nullptr;
};

} // namespace daw::plugins
