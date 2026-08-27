#pragma once

#include <aeffectx.h>

#include <memory>
#include <string>

namespace daw::plugins {

namespace vst {

struct HostContext;
using HostDispatch = VstIntPtr (*)(HostContext&, AEffect*, VstInt32, VstInt32,
                                   VstIntPtr, void*, float) noexcept;

/// The state needed while a plugin calls back into its host. During the module
/// entry point there is no AEffect yet, so VstModule exposes this through a
/// thread-local creation scope; afterwards AEffect::user points here.
struct HostContext {
    void* owner = nullptr;
    HostDispatch dispatch = nullptr;
    VstInt32 currentId = 0;
};

VstIntPtr VSTCALLBACK hostCallback(AEffect* effect, VstInt32 opcode,
                                   VstInt32 index, VstIntPtr value, void* ptr,
                                   float opt) noexcept;

} // namespace vst

/// One resident legacy VST module. Each call to create() produces an AEffect;
/// the library itself stays mapped for the process lifetime, matching the
/// existing CLAP/VST3 module ownership rule.
class VstModule {
public:
    ~VstModule();

    VstModule(const VstModule&) = delete;
    VstModule& operator=(const VstModule&) = delete;

    static std::shared_ptr<VstModule> open(const std::string& path);
    AEffect* create(vst::HostContext& host) const noexcept;

    const std::string& path() const noexcept { return m_path; }

private:
    using EntryProc = AEffect* (*)(audioMasterCallback);
    VstModule() = default;

    std::string m_path;
    void* m_handle = nullptr;
    void* m_bundle = nullptr;
    EntryProc m_entry = nullptr;
};

} // namespace daw::plugins
