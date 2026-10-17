#pragma once
#include <QtGlobal>
#include <algorithm>

namespace ui::detail {
// Keep the requested cadence on an absolute time line. Re-basing on every
// delivered frame accumulates timer/vsync jitter and can skip alternate frames.
class FrameCadence {
public:
    qint64 deadlineNs() const { return m_deadlineNs; }
    void reset() { m_deadlineNs = 0; }
    bool due(qint64 now, qint64 period) const {
        // Native GUI delivery can drift by a few ms around a display tick. An
        // early delivery is still that frame, not a reason to wait another
        // vsync. Absolute deadlines keep this tolerance from accumulating
        // into a higher average frame rate.
        // At 240 Hz, alternating +/-1.25 ms delivery jitter makes successive
        // ticks 2.5 ms early relative to one another. Half a period (2.08 ms)
        // rejects every second tick. The absolute deadline still caps the
        // average rate when the tolerance extends to a whole period.
        const auto tolerance = std::min<qint64>(3000000, period);
        return !m_deadlineNs || !period || now + tolerance >= m_deadlineNs;
    }
    void advance(qint64 now, qint64 period) {
        if (!period) { reset(); return; }
        if (!m_deadlineNs || now >= m_deadlineNs + period)
            m_deadlineNs = now + period; // resume without a burst of catch-up frames
        else
            m_deadlineNs += period;
    }
private:
    qint64 m_deadlineNs = 0;
};
} // namespace ui::detail
