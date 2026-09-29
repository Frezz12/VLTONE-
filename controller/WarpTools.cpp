#include "WarpTools.hpp"
#include <algorithm>
#include <cmath>

namespace daw::warptools {
Proposal align(const ClipWarpModel& baseline, const std::vector<analysis::WarpTransient>& attacks,
               const AlignParams& p) {
    Proposal out; out.map = baseline;
    if (!validWarp(baseline) || baseline.empty() || !baseline.enabled ||
        !std::isfinite(p.originBeats) || !std::isfinite(p.timing.gridBeats) || p.timing.gridBeats <= 0 ||
        !std::isfinite(p.timing.strength) || p.timing.strength <= 0) return out;
    const double begin = std::clamp(p.beginBeats, 0., baseline.markers.back().targetBeats);
    const double end = std::clamp(p.endBeats, begin, baseline.markers.back().targetBeats);
    if (end - begin < 1e-5) return out;
    auto& points = out.map.markers;
    const auto insert = [&](double source, double beat, bool locked) {
        if (points.size() >= 16384 || !std::isfinite(source) || !std::isfinite(beat)) return false;
        const auto at = std::lower_bound(points.begin(), points.end(), source,
            [](const auto& m, double s) { return m.sourceSeconds < s; });
        if (at == points.begin() || at == points.end() || source - (at - 1)->sourceSeconds < 1e-5 ||
            at->sourceSeconds - source < 1e-5 || beat - (at - 1)->targetBeats < 1e-5 || at->targetBeats - beat < 1e-5) return false;
        const std::string id = baseline.markers.front().id + ":warp:" + std::to_string(std::llround(source * 1e9));
        if (std::any_of(points.begin(), points.end(), [&](const auto& m) { return m.id == id; })) return false;
        points.insert(at, {id, source, beat, locked}); return true;
    };
    // Bound an assisted range so the timing outside it is untouched.
    insert(warpSourceAt(baseline, begin), begin, true);
    insert(warpSourceAt(baseline, end), end, true);
    if (p.addTransients) for (const auto& attack : attacks) {
        if (attack.strength < p.minimumStrength) continue;
        const double beat = warpBeatAt(baseline, attack.sourceSeconds);
        if (beat <= begin + 1e-5 || beat >= end - 1e-5) continue;
        if (attack.confidence < p.minimumConfidence) {
            ++out.uncertain;
            if (!p.includeUncertain) continue;
        }
        if (insert(attack.sourceSeconds, beat, false)) ++out.added;
    }
    // Bound each marker by its original neighbours as well as fixed anchors.
    // This preserves order even when swing/groove targets are non-monotonic.
    const auto original = points;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        auto& marker = points[i];
        if (marker.locked || marker.targetBeats <= begin || marker.targetBeats >= end ||
            (!p.selected.empty() && !p.selected.contains(marker.id))) continue;
        const double target = miditools::gridTarget(marker.targetBeats + p.originBeats, p.timing) - p.originBeats;
        if (!std::isfinite(target) || std::abs(target - marker.targetBeats) <= p.timing.toleranceBeats) continue;
        const double wanted = marker.targetBeats + (target - marker.targetBeats) * std::clamp(p.timing.strength, 0., 1.);
        const double leftGap = std::max(1e-5, (marker.sourceSeconds - points[i - 1].sourceSeconds) * p.tempo / 60. / 999.);
        const double rightGap = std::max(1e-5, (original[i + 1].sourceSeconds - marker.sourceSeconds) * p.tempo / 60. / 999.);
        const double low = std::max(begin, points[i - 1].targetBeats) + leftGap;
        const double high = std::min(end, original[i + 1].targetBeats) - rightGap;
        if (low > high) { ++out.constrained; continue; }
        marker.targetBeats = std::clamp(wanted, low, high);
        out.constrained += std::abs(marker.targetBeats - wanted) > 1e-8;
        const double shift = std::abs(marker.targetBeats - original[i].targetBeats);
        out.moved += shift > 1e-8;
        out.maximumShiftBeats = std::max(out.maximumShiftBeats, shift);
    }
    return out;
}

miditools::Groove extractGroove(const std::vector<double>& onsets, double length, double grid, const std::string& name) {
    miditools::Groove out; out.name = name;
    if (!std::isfinite(length) || !std::isfinite(grid) || length <= 0 || grid <= 0 || length / grid > 4096) return out;
    const auto slots = std::max<std::size_t>(1, std::size_t(std::llround(length / grid)));
    out.lengthBeats = length; out.offsets.resize(slots);
    std::vector<unsigned> counts(slots);
    const double step = length / slots;
    for (double beat : onsets) {
        if (!std::isfinite(beat) || beat < 0 || beat >= length) continue;
        const auto slot = std::llround(beat / step);
        const auto index = std::size_t(slot) % slots;
        out.offsets[index] += std::clamp(beat - slot * step, -.49 * step, .49 * step);
        ++counts[index];
    }
    for (std::size_t i = 0; i < slots; ++i) if (counts[i]) out.offsets[i] /= counts[i];
    return out;
}
}
