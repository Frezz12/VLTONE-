#pragma once

#include "RenderSpec.hpp"

#include <QString>

/// What the export dialog remembers between runs.
///
/// Everything here is a *choice*, not a destination-specific detail: the format,
/// the range mode, the tail, the processing switches and the folder last written
/// to. The selected stem channels are deliberately not persisted — they belong
/// to a project, not to the application.
namespace ui::exportprefs {

/// Load the remembered settings into a spec, leaving anything this namespace
/// does not own (the stem list, the custom range) at its default.
daw::rendering::Spec load();
/// Remember the parts of `spec` that are choices rather than project details.
void save(const daw::rendering::Spec& spec);

/// The folder the last render was written to, or the user's music folder the
/// first time round.
QString lastFolder();
void setLastFolder(const QString& folder);

} // namespace ui::exportprefs
