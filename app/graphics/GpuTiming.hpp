#pragma once
class QQuickWindow;
namespace ui::graphics {
// Optional diagnostics. QRhi's limited-compatibility API stays in one .cpp.
bool configureGpuTiming(QQuickWindow* window);
// Render thread, inside a frame after beginFrame. Never waits for completion.
// This is an older completed frame, not the current frame or a display timestamp.
double completedGpuTimeMs(QQuickWindow* window);
}
