#include "GpuTiming.hpp"
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <rhi/qrhi.h>
#include <cmath>

namespace ui::graphics {
bool configureGpuTiming(QQuickWindow* window) {
    if (qEnvironmentVariableIsSet("VLT_GPU_TIMESTAMPS") &&
        !qEnvironmentVariableIntValue("VLT_GPU_TIMESTAMPS")) return false;
    auto config = window->graphicsConfiguration();
    config.setTimestamps(true);
    window->setGraphicsConfiguration(config);
    return true;
}
double completedGpuTimeMs(QQuickWindow* window) {
    auto* rhi = window->rhi();
    auto* swapchain = window->swapChain();
    if (!rhi || !swapchain || !rhi->isFeatureSupported(QRhi::Timestamps)) return -1;
    auto* commands = swapchain->currentFrameCommandBuffer();
    if (!commands) return -1;
    const double seconds = commands->lastCompletedGpuTime();
    // Zero also means unavailable/initial frames/resizing; never report it as
    // proof that the GPU did no work. Reading does not flush or finish the GPU.
    return std::isfinite(seconds) && seconds > 0 ? seconds * 1000. : -1;
}
}
