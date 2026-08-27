#pragma once

#include "Host/PluginTypes.hpp"
#include "model/Document.hpp"

namespace daw {

/// Bridges the document's `PluginFormat` and the host's `plugins::Format`.
///
/// The two enums are deliberately separate — `Document.hpp` is pure data and
/// must not depend on which formats this build compiled in — so this is the one
/// place they meet. The static_asserts below are what stop them drifting apart
/// silently when a format is added to one and not the other.
static_assert(int(PluginFormat::None) == int(plugins::Format::Unknown));
static_assert(int(PluginFormat::Clap) == int(plugins::Format::Clap));
static_assert(int(PluginFormat::Vst3) == int(plugins::Format::Vst3));
static_assert(int(PluginFormat::AudioUnit) == int(plugins::Format::AudioUnit));
static_assert(int(PluginFormat::Internal) == int(plugins::Format::Internal));
static_assert(int(PluginFormat::Vst) == int(plugins::Format::Vst));

inline plugins::Format toHostFormat(PluginFormat format) noexcept {
    return static_cast<plugins::Format>(format);
}

inline PluginFormat toDocumentFormat(plugins::Format format) noexcept {
    return static_cast<PluginFormat>(format);
}

/// Fill a slot from a scanned descriptor, keeping the slot's own identity.
/// The id is preserved on purpose: replacing the plugin in a slot must not
/// orphan its saved state or an editor window that is open on it.
inline void applyDescriptor(InsertModel& slot,
                            const plugins::PluginDescriptor& descriptor) {
    slot.format = toDocumentFormat(descriptor.format);
    slot.uid = descriptor.uid;
    slot.path = descriptor.path;
    slot.name = descriptor.name;
    slot.vendor = descriptor.vendor;
    // The old plugin's state and parameters mean nothing to the new one.
    slot.stateFile.clear();
    slot.rightStateFile.clear();
    slot.parameters.clear();
    slot.rightParameters.clear();
}

} // namespace daw
