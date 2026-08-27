#include "Host/PluginInstance.hpp"

#include "Internal/InternalFactory.hpp"

#if defined(DAW_ENABLE_CLAP)
#include "Clap/ClapFactory.hpp"
#endif
#if defined(DAW_ENABLE_VST3)
#include "Vst3/Vst3Factory.hpp"
#endif
#if defined(DAW_ENABLE_VST)
#include "Vst/VstFactory.hpp"
#endif
#if defined(DAW_ENABLE_AU)
#include "Au/AuFactory.hpp"
#endif

namespace daw::plugins {

std::string_view toString(Format format) noexcept {
    switch (format) {
        case Format::Clap: return "clap";
        case Format::Vst3: return "vst3";
        case Format::Vst: return "vst";
        case Format::AudioUnit: return "au";
        case Format::Internal: return "internal";
        case Format::Unknown: break;
    }
    return "unknown";
}

Format formatFromString(std::string_view name) noexcept {
    if (name == "clap") return Format::Clap;
    if (name == "vst3") return Format::Vst3;
    if (name == "vst") return Format::Vst;
    if (name == "au") return Format::AudioUnit;
    if (name == "internal") return Format::Internal;
    return Format::Unknown;
}

namespace {

/// The built-ins, kept out of `availableFactories()` on purpose.
///
/// That list means "formats a scan walks the disk for", and everything reading
/// it — the search-path editor, the format filters, `collectCandidates` — would
/// otherwise offer the user folders to search for plugins that live in this
/// binary. `factoryFor` still resolves it, which is all instantiation needs.
InternalFactory& internalFactory() noexcept {
    static InternalFactory factory;
    return factory;
}

} // namespace

std::vector<PluginFactory*> availableFactories() {
    // Function-local statics: constructed on first use, never destroyed before
    // the plugins that came out of them, and no static initialisation order to
    // reason about.
    std::vector<PluginFactory*> factories;
#if defined(DAW_ENABLE_CLAP)
    static ClapFactory clap;
    factories.push_back(&clap);
#endif
#if defined(DAW_ENABLE_VST3)
    static Vst3Factory vst3;
    factories.push_back(&vst3);
#endif
#if defined(DAW_ENABLE_VST)
    static VstFactory vst;
    factories.push_back(&vst);
#endif
#if defined(DAW_ENABLE_AU)
    static AuFactory au;
    factories.push_back(&au);
#endif
    return factories;
}

PluginFactory* factoryFor(Format format) noexcept {
    if (format == Format::Internal) return &internalFactory();
    for (PluginFactory* factory : availableFactories()) {
        if (factory->format() == format) return factory;
    }
    return nullptr;
}

} // namespace daw::plugins
