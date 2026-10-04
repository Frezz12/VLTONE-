#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace daw {

struct WarpMarker {
    std::string id;
    double sourceSeconds = 0;
    double targetBeats = 0;
    bool locked = false;
    friend bool operator==(const WarpMarker&, const WarpMarker&) = default;
};

// Source coordinates refer to the processed source, just like clip trim.
// Musical coordinates are relative to the clip start, never project seconds.
struct ClipWarpModel {
    bool enabled = false;
    bool preservePitch = true;
    int mode = 4;
    double baselineDurationSeconds = 0;
    double sensitivity = 50;
    std::vector<WarpMarker> markers;
    bool empty() const noexcept { return markers.empty(); }
    friend bool operator==(const ClipWarpModel&, const ClipWarpModel&) = default;
};

inline double normalizeWarpSourceSeconds(double seconds) noexcept {
    // Inverting a trim boundary at source zero can leave a negative rounding
    // residue. Recover sub-nanosecond noise without hiding invalid coordinates.
    return seconds >= -1e-9 && seconds <= 0 ? 0.0 : seconds;
}

inline bool validWarp(const ClipWarpModel& warp) {
    if (warp.empty()) return !warp.enabled;
    if (warp.markers.size() < 2 || warp.markers.size() > 16384 ||
        !std::isfinite(warp.baselineDurationSeconds) || warp.baselineDurationSeconds <= 0 ||
        !std::isfinite(warp.sensitivity) || warp.sensitivity < 0 || warp.sensitivity > 100 ||
        warp.mode < 1 || warp.mode > 4 || std::abs(warp.markers.front().targetBeats) > 1e-9 ||
        !warp.markers.front().locked || !warp.markers.back().locked) return false;
    std::unordered_set<std::string> ids;
    for (std::size_t i = 0; i < warp.markers.size(); ++i) {
        const auto& point = warp.markers[i];
        if (point.id.empty() || !ids.insert(point.id).second ||
            !std::isfinite(point.sourceSeconds) || point.sourceSeconds < 0 ||
            !std::isfinite(point.targetBeats) || point.targetBeats < 0) return false;
        if (i && (point.sourceSeconds - warp.markers[i - 1].sourceSeconds < 1e-7 ||
                  point.targetBeats - warp.markers[i - 1].targetBeats < 1e-7)) return false;
    }
    return true;
}

inline std::size_t warpSegment(const ClipWarpModel& warp, double beats) noexcept {
    auto next = std::upper_bound(warp.markers.begin(), warp.markers.end(), beats,
        [](double value, const WarpMarker& marker) { return value < marker.targetBeats; });
    return std::clamp<std::ptrdiff_t>(next - warp.markers.begin() - 1, 0,
                                    std::ptrdiff_t(warp.markers.size()) - 2);
}

inline double warpSourceAt(const ClipWarpModel& warp, double beats) noexcept {
    if (warp.markers.size() < 2) return 0;
    const auto i = warpSegment(warp, beats);
    const auto& a = warp.markers[i]; const auto& b = warp.markers[i + 1];
    return normalizeWarpSourceSeconds(a.sourceSeconds + (beats - a.targetBeats) *
        (b.sourceSeconds - a.sourceSeconds) / (b.targetBeats - a.targetBeats));
}

inline double warpBeatAt(const ClipWarpModel& warp, double seconds) noexcept {
    if (warp.markers.size() < 2) return 0;
    auto next = std::upper_bound(warp.markers.begin(), warp.markers.end(), seconds,
        [](double value, const WarpMarker& marker) { return value < marker.sourceSeconds; });
    const auto i = std::clamp<std::ptrdiff_t>(next - warp.markers.begin() - 1, 0,
                                            std::ptrdiff_t(warp.markers.size()) - 2);
    const auto& a = warp.markers[i]; const auto& b = warp.markers[i + 1];
    return a.targetBeats + (seconds - a.sourceSeconds) *
        (b.targetBeats - a.targetBeats) / (b.sourceSeconds - a.sourceSeconds);
}

inline double warpSpeedAt(const ClipWarpModel& warp, double beats, double tempo) noexcept {
    const auto i = warpSegment(warp, beats);
    return (warp.markers[i + 1].sourceSeconds - warp.markers[i].sourceSeconds) /
           (warp.markers[i + 1].targetBeats - warp.markers[i].targetBeats) * tempo / 60.;
}

inline double warpMaximumRatio(const ClipWarpModel& warp, double tempo) noexcept {
    double ratio = 1;
    for (std::size_t i = 0; i + 1 < warp.markers.size(); ++i) {
        const double speed = warpSpeedAt(warp, warp.markers[i].targetBeats, tempo);
        ratio = std::max({ratio, speed, 1 / speed});
    }
    return ratio;
}

inline bool sameWarpAudio(const ClipWarpModel& a, const ClipWarpModel& b) noexcept {
    if (a.preservePitch != b.preservePitch || a.mode != b.mode || a.markers.size() != b.markers.size()) return false;
    for (std::size_t i = 0; i < a.markers.size(); ++i)
        if (a.markers[i].sourceSeconds != b.markers[i].sourceSeconds || a.markers[i].targetBeats != b.markers[i].targetBeats) return false;
    return true;
}

} // namespace daw
