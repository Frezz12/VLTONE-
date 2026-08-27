#pragma once

#include <clap/clap.h>

#include <memory>
#include <string>

namespace daw::plugins {

/// One loaded CLAP DSO, with its entry point initialised.
///
/// Ownership matters more here than it looks: the descriptors a factory hands
/// out stay valid only until `deinit()`, and a plugin instance keeps calling
/// into the DSO until it is destroyed. So the module is shared and every
/// instance holds one, which is what keeps the library mapped for exactly as
/// long as something is using it.
class ClapModule {
public:
    ~ClapModule();

    ClapModule(const ClapModule&) = delete;
    ClapModule& operator=(const ClapModule&) = delete;

    /// Open a `.clap` bundle or shared object. Returns null when it is not a
    /// CLAP module, when the ABI is too new, or when `init()` refuses.
    static std::shared_ptr<ClapModule> open(const std::string& path);

    const clap_plugin_factory_t* factory() const noexcept { return m_factory; }
    const std::string& path() const noexcept { return m_path; }

private:
    ClapModule() = default;

    std::string m_path;
    void* m_handle = nullptr;
    const clap_plugin_entry_t* m_entry = nullptr;
    const clap_plugin_factory_t* m_factory = nullptr;
};

} // namespace daw::plugins
