#pragma once

#include "UiConstants.hpp"
#include "model/Document.hpp"

#include <algorithm>
#include <map>
#include <string>

// Geometry of the inline comp editor, shared by the timeline lanes and the track
// header column.
//
// An expanded clip cannot simply grow taller than its lane — it would draw over
// the next track. So expanding *grows the lane*, and because both the lanes and
// the headers size themselves through `laneHeightForTrack` below, the two
// columns stay aligned with no extra plumbing.
//
// The expansion factor is animated, and both widgets have to read the same value
// on the same frame or the header would visibly lag its lane. That makes it
// genuinely shared view state rather than either widget's property, so it lives
// here — written by whoever drives the animation (the timeline), read by both.
namespace ui {

/// Height of one take's sub-lane inside an expanded clip.
inline constexpr int kTakeRowHeight = 38;
/// How long the expand/collapse animation runs, and how much of it is spent
/// staggering the take rows in after the lane has started growing.
inline constexpr int kCompAnimMs = 250;
inline constexpr double kTakeStagger = 0.55;

namespace detail {
inline std::map<std::string, std::string>& pendingTakeClips() {
    static std::map<std::string, std::string> pending;
    return pending;
}
}

/// The clip on this track that is growing a take right now, or empty. A layer
/// being punched into an expanded clip is drawn as the next row of that clip's
/// stack, and the lane has to make room for that row while the take runs — so
/// this is layout state, read by `expandedTakeCount` below and therefore by
/// both the lanes and the header column. Written once a frame by the window's
/// refresh, which is the only place that knows a take is running.
inline void setPendingTakeClip(const std::string& trackId,
                               const std::string& clipId) {
    if (clipId.empty()) detail::pendingTakeClips().erase(trackId);
    else detail::pendingTakeClips()[trackId] = clipId;
}

inline std::string pendingTakeClip(const daw::TrackModel& track) {
    const auto& pending = detail::pendingTakeClips();
    const auto it = pending.find(track.id);
    return it == pending.end() ? std::string() : it->second;
}

/// The tallest take stack among the expanded clips on a track. A track grows
/// once, and every expanded clip on it draws into the same rows. A take being
/// recorded into one of them counts as a row of its own, so the stack visibly
/// grows by the layer while it is being played rather than when it lands.
inline int expandedTakeCount(const daw::TrackModel& track) {
    size_t most = 0;
    const std::string pending = pendingTakeClip(track);
    for (const auto& clip : track.clips) {
        if (clip.expanded && !clip.takes.empty()) {
            most = std::max(most,
                            clip.takes.size() + (clip.id == pending ? 1u : 0u));
        }
    }
    return int(most);
}

namespace detail {
inline std::map<std::string, double>& compFactors() {
    static std::map<std::string, double> factors;
    return factors;
}
}

/// How far through its expand animation a track is: 0 = collapsed, 1 = open.
/// A track nobody has animated is simply wherever its document state says.
inline double compFactor(const daw::TrackModel& track) {
    const auto& factors = detail::compFactors();
    const auto it = factors.find(track.id);
    if (it != factors.end()) return std::clamp(it->second, 0.0, 1.0);
    return expandedTakeCount(track) > 0 ? 1.0 : 0.0;
}

inline void setCompFactor(const std::string& trackId, double factor) {
    detail::compFactors()[trackId] = std::clamp(factor, 0.0, 1.0);
}

inline void clearCompFactor(const std::string& trackId) {
    detail::compFactors().erase(trackId);
}

/// Extra lane height the comp editor needs right now, animation included.
inline int compExtraHeight(const daw::TrackModel& track) {
    const int rows = expandedTakeCount(track);
    if (rows == 0) return 0;
    return int(std::lround(double(rows * kTakeRowHeight) * compFactor(track)));
}

/// The on-screen height of a track's lane: its own height plus whatever the comp
/// editor is currently claiming. The single answer both columns use.
inline int laneHeightForTrack(const daw::TrackModel& track) {
    return laneHeightFor(track.height) + compExtraHeight(track);
}

/// Opacity of take row `index` as the cascade slides in — the rows arrive one
/// after another rather than all at once, so the eye can follow the stack
/// building up. Fully opaque once the animation has finished.
inline double takeRowReveal(const daw::TrackModel& track, int index, int count) {
    const double factor = compFactor(track);
    if (factor >= 1.0) return 1.0;
    if (count <= 0) return 0.0;
    const double slot = kTakeStagger / double(count);
    const double begin = (1.0 - kTakeStagger) * 0.0 + slot * double(index);
    const double t = (factor - begin) / std::max(0.05, 1.0 - kTakeStagger);
    return std::clamp(t, 0.0, 1.0);
}

} // namespace ui
