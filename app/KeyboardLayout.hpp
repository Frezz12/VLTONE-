#pragma once

class QKeyEvent;

namespace ui {

/// US-QWERTY key at the physical position reported by a native key event.
/// Returns 0 when the platform supplied neither a usable native code nor a
/// known textual fallback. The result is independent of the active layout.
int physicalUsKey(const QKeyEvent* event);

} // namespace ui
