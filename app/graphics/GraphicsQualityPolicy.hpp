#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui::graphics {
enum class Quality { Automatic, Maximum, Medium, Low };
enum class QualityLevel { Maximum, Medium, Low };

struct FrameStats {
    std::uint64_t windowId = 0;
    double preparationMs = 0;
    double synchronizationMs = 0;
    double renderCpuMs = 0;
    double gpuMs = -1; // unavailable is not zero
    double presentIntervalMs = -1;
    double displayHz = 60;
    double frameBudgetMs = 0; // 0 = uncapped; use the display budget for quality
    std::uint64_t audioXruns = 0;
    bool active = true;
};

// A deterministic policy, separate from settings, timers and graphics APIs.
// It never changes editor resolution, audio configuration or the FPS preference.
class QualityPolicy {
public:
    void setQuality(Quality quality) noexcept {
        m_quality = quality;
        m_level = quality == Quality::Low ? QualityLevel::Low :
                  quality == Quality::Medium ? QualityLevel::Medium : QualityLevel::Maximum;
        resetObservation();
        m_lastChange = -1;
    }
    Quality quality() const noexcept { return m_quality; }
    QualityLevel level() const noexcept { return m_level; }
    static double scale(QualityLevel level) noexcept {
        return level == QualityLevel::Low ? .5 : level == QualityLevel::Medium ? .75 : 1.;
    }
    static int mediaFps(QualityLevel level) noexcept {
        return level == QualityLevel::Low ? 15 : level == QualityLevel::Medium ? 30 : 0;
    }
    bool observe(const FrameStats& frame, std::int64_t nowMs) noexcept {
        if (!frame.active || nowMs < 0 || (m_lastSample >= 0 &&
                (nowMs < m_lastSample || nowMs - m_lastSample > 1000))) {
            if (m_lastSample >= 0 && nowMs < m_lastSample) m_lastChange = -1;
            resetObservation();
            if (!frame.active || nowMs < 0) return false;
        }
        const bool xrun = m_haveXruns && frame.audioXruns > m_lastXruns;
        m_lastXruns = frame.audioXruns;
        m_haveXruns = true;
        m_lastSample = nowMs;
        if (m_quality != Quality::Automatic) return false;
        const double hz = std::isfinite(frame.displayHz) && frame.displayHz > 0 ? frame.displayHz : 60.;
        const double budget = std::isfinite(frame.frameBudgetMs) && frame.frameBudgetMs > 0 ? frame.frameBudgetMs : 1000. / hz;
        const double cpu = frame.preparationMs + frame.synchronizationMs + frame.renderCpuMs;
        const double cost = std::max(cpu, frame.gpuMs);
        if (!std::isfinite(cost) || cost < 0) return false;
        if (cost > budget * .8 || xrun) {
            m_goodSince = -1;
            if (m_badSince < 0) m_badSince = nowMs;
            m_xrunPending |= xrun;
            // At the floor an xrun cannot request another downgrade. Keeping
            // it pending would permanently prevent recovery after load ends.
            if (m_level == QualityLevel::Low) m_xrunPending = false;
            if ((m_xrunPending || nowMs - m_badSince >= 2000) && canChange(nowMs) &&
                    m_level != QualityLevel::Low) {
                m_level = static_cast<QualityLevel>(static_cast<int>(m_level) + 1);
                changed(nowMs);
                return true;
            }
        } else {
            m_badSince = -1;
            if (m_xrunPending && canChange(nowMs) && m_level != QualityLevel::Low) {
                m_level = static_cast<QualityLevel>(static_cast<int>(m_level) + 1);
                changed(nowMs);
                return true;
            }
            if (cost < budget * .6 && !m_xrunPending) {
                if (m_goodSince < 0) m_goodSince = nowMs;
                if (nowMs - m_goodSince >= 10000 && canChange(nowMs) &&
                        m_level != QualityLevel::Maximum) {
                    m_level = static_cast<QualityLevel>(static_cast<int>(m_level) - 1);
                    changed(nowMs);
                    return true;
                }
            } else m_goodSince = -1;
        }
        return false;
    }
private:
    bool canChange(std::int64_t now) const noexcept { return m_lastChange < 0 || now - m_lastChange >= 2000; }
    void changed(std::int64_t now) noexcept {
        m_lastChange = now; m_badSince = m_goodSince = -1; m_xrunPending = false;
    }
    void resetObservation() noexcept {
        m_lastSample = m_badSince = m_goodSince = -1;
        m_haveXruns = m_xrunPending = false;
    }
    Quality m_quality = Quality::Automatic;
    QualityLevel m_level = QualityLevel::Maximum;
    std::int64_t m_lastSample = -1, m_badSince = -1, m_goodSince = -1, m_lastChange = -1;
    std::uint64_t m_lastXruns = 0;
    bool m_haveXruns = false, m_xrunPending = false;
};
} // namespace ui::graphics
