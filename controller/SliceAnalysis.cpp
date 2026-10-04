#include "SliceAnalysis.hpp"

#include "WarpAnalysis.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <limits>
#include <cmath>
#include <vector>

namespace daw::slicer {
namespace {

using plugins::slicer::Slice;
using plugins::slicer::SliceTable;
namespace params = plugins::slicer;

/// Drops anything that is not strictly increasing, so no chop can end where it
/// started. Sorting first is what turns a jittered or detection-ordered list
/// back into the ascending run the chop model assumes.
void tighten(std::vector<engine::FrameCount>& bounds) {
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());
}

/// `count` pieces covering [0, frames) with no rounding drift: each boundary is
/// `frames * i / count` in integer arithmetic, so the last chop runs to the
/// final frame however the division lands.
std::vector<engine::FrameCount> gridStarts(engine::FrameCount frames,
                                           const SliceSettings& settings) {
    int count = std::clamp(settings.targetCount, 1, int(params::kMaxSlices));
    std::vector<engine::FrameCount> out;
    out.reserve(std::size_t(count));
    for (int i = 0; i < count; ++i) {
        out.push_back(engine::FrameCount(
            std::uint64_t(frames) * std::uint64_t(i) / std::uint64_t(count)));
    }
    return out;
}

/// Boundaries moved off the even split by up to 45% of a slot each. The
/// even split stays underneath as the nominal spacing, which is what stops two
/// jittered boundaries from ever meeting: the most they can approach each
/// other is 10% of a slot apart, and neither can leave the sample.
///
/// The jitter is drawn bit-exactly from the generator rather than through
/// `std::uniform_real_distribution`, whose output the standard does not pin
/// down — a project reopened on another machine should rebuild the chops it
/// was saved with.
std::vector<engine::FrameCount> randomStarts(engine::FrameCount frames,
                                             const SliceSettings& settings) {
    std::vector<engine::FrameCount> out = gridStarts(frames, settings);
    if (out.size() < 2) return out;

    std::mt19937_64 rng(settings.seed);
    const double spacing = double(frames) / double(out.size());
    for (std::size_t i = 1; i < out.size(); ++i) {
        // 53 bits of mantissa: exactly the uniform [0,1) a double can hold.
        const double unit = double(rng() >> 11) * (1.0 / 9007199254740992.0);
        const double shifted = double(out[i]) + (unit - 0.5) * 2.0 * std::clamp(settings.randomSpread, 0.0, 0.49) * spacing;
        const auto bounded = std::clamp(std::llround(shifted), 0LL,
                                        std::int64_t(frames) - 1);
        out[i] = engine::FrameCount(bounded);
    }
    tighten(out);
    return out;
}

/// Onset positions, in frames, always starting at 0. An empty return means the
/// run was cancelled; "no onsets found" comes back as a single entry.
std::vector<engine::FrameCount> transientStarts(const engine::SampleBuffer& audio,
                                                const SliceSettings& settings,
                                                const std::function<bool()>& keepGoing) {
    const double rate = audio.sampleRate() > 0.0 ? audio.sampleRate() : 48000.0;
    const auto begin = std::min(settings.rangeStart, audio.frames() - 1);
    const auto end = settings.rangeEnd > begin ? std::min(settings.rangeEnd, audio.frames()) : audio.frames();
    auto onsets = daw::analysis::detectWarpTransients(audio, double(begin) / rate,
                                                     double(end) / rate, keepGoing);
    // The detector answers a cancelled run and a fruitless one with the same
    // empty vector, and they must not be confused: a cancel leaves the previous
    // table alone, a fruitless run falls back to an even split.
    if (keepGoing && !keepGoing()) return {};

    // `strength` is the square root of a hit's flux against the loudest in the
    // file, so a floor of zero is a floor of "keep everything it reported".
    const double floor = std::clamp(settings.sensitivity, 0.0, 1.0) * 0.9;
    std::erase_if(onsets, [floor](const daw::analysis::WarpTransient& onset) {
        return onset.strength < floor;
    });

    // More onsets than asked for means the strongest win, not the earliest —
    // that is what the knob's ceiling is for.
    const int limit = std::clamp(settings.targetCount, 1, int(params::kMaxSlices)) - 1;
    if (int(onsets.size()) > limit) {
        std::partial_sort(onsets.begin(), onsets.begin() + limit, onsets.end(),
                          [](const auto& a, const auto& b) {
                              return a.strength > b.strength;
                          });
        onsets.resize(std::size_t(limit));
    }

    // Back up the shared boundary before the detected attack. This preserves
    // the onset without gaps or overlapping neighbouring slices.
    const auto pre = engine::FrameCount(
        std::max(0.0, settings.preAttackMs * 0.001 * rate));

    std::vector<engine::FrameCount> out;
    out.reserve(onsets.size() + 1);
    out.push_back(0);
    for (const daw::analysis::WarpTransient& onset : onsets) {
        auto frame = std::llround(onset.sourceSeconds * rate);
        if (frame <= std::int64_t(begin) || frame >= std::int64_t(end)) continue;
        frame = std::max(std::int64_t(begin), frame - std::int64_t(pre));
        out.push_back(engine::FrameCount(frame - begin));
    }
    tighten(out);
    return out;
}

} // namespace

bool assignKeys(SliceTable& table, const SliceSettings& settings) {
    const int root = std::clamp(settings.rootNote, 0, 127);
    std::vector<int> keys;
    const auto degrees = params::scaleDegrees(settings.scale);
    for (int step = 0; step < 128; ++step) {
        const int key = (root + step) % 128;
        const int degree = ((key - root) % 12 + 12) % 12;
        if (std::find(degrees.begin(), degrees.end(), degree) != degrees.end()) keys.push_back(key);
    }
    const bool fallback = keys.size() < table.count;
    if (fallback) {
        keys.clear();
        for (int step = 0; step < 128; ++step) keys.push_back((root + step) % 128);
    }
    for (std::uint32_t i = 0; i < table.count; ++i)
        table.slices[i].key = std::int16_t(keys[settings.descending ? table.count - 1 - i : i]);
    table.chromaticFallback = fallback;
    table.rebuild();
    return fallback;
}

engine::FrameCount snapToZero(const engine::SampleBuffer& audio, engine::FrameCount frame,
    engine::FrameCount low, engine::FrameCount high) {
    if (!audio.frames() || low > high) return frame;
    const auto radius = engine::FrameCount(std::max(1.0, audio.sampleRate() * .005));
    const auto lo = std::max(low, frame > radius ? frame - radius : 0);
    const auto hi = std::min({high, audio.frames() - 1, engine::FrameCount(std::min<std::uint64_t>(UINT32_MAX, std::uint64_t(frame) + radius))});
    float best = std::numeric_limits<float>::max();
    for (auto f = lo; f <= hi; ++f) {
        const float a = audio.readSample(0, f ? f - 1 : f), b = audio.readSample(0, f);
        const float score = std::abs(b) + (a * b <= 0 ? 0.0f : 2.0f);
        if (score < best) { best = score; frame = f; }
    }
    return frame;
}

bool moveBoundary(SliceTable& table, int right, engine::FrameCount frame, engine::FrameCount minimum) {
    if (right <= 0 || right >= int(table.count)) return false;
    auto& a = table.slices[right - 1]; auto& b = table.slices[right];
    minimum = std::max(1u, minimum);
    const auto span = b.end - a.start;
    minimum = std::min(minimum, span / 2);
    frame = std::clamp(frame, a.start + minimum, b.end - minimum);
    if (frame == b.start) return false;
    a.end = b.start = frame;
    return true;
}

bool split(SliceTable& table, engine::FrameCount frame, engine::FrameCount minimum) {
    if (table.count >= params::kMaxSlices) return false;
    for (std::uint32_t i = 0; i < table.count; ++i) {
        if (frame <= table.slices[i].start || frame >= table.slices[i].end) continue;
        if (frame - table.slices[i].start < std::max(1u,minimum) || table.slices[i].end - frame < std::max(1u,minimum)) return false;
        Slice right = table.slices[i];
        right.start = frame; right.id = 0;
        std::array<bool, 128> used{};
        for (std::uint32_t j = 0; j < table.count; ++j)
            if (table.slices[j].key >= 0 && table.slices[j].key < 128) used[table.slices[j].key] = true;
        for (int j = 1; j <= 128; ++j) {
            const int key = (int(right.key) + j) % 128;
            if (!used[key]) { right.key = std::int16_t(key); break; }
        }
        for (std::uint32_t j = table.count; j > i + 1; --j) table.slices[j] = table.slices[j - 1];
        table.slices[i].end = frame; table.slices[i + 1] = right;
        ++table.count; table.rebuild(); return true;
    }
    return false;
}

bool merge(SliceTable& table, int right) {
    if (right <= 0 || right >= int(table.count)) return false;
    table.slices[right - 1].end = table.slices[right].end;
    for (int i = right; i + 1 < int(table.count); ++i) table.slices[i] = table.slices[i + 1];
    --table.count; table.rebuild(); return true;
}

SliceTable cut(const engine::SampleBuffer& audio, const SliceSettings& settings,
               const std::function<bool()>& keepGoing) {
    SliceTable table;
    const engine::FrameCount frames = audio.frames();
    if (frames == 0) return table;
    table.frames = frames;

    const auto begin = std::min(settings.rangeStart, frames - 1);
    const auto end = settings.rangeEnd > begin ? std::min(settings.rangeEnd, frames) : frames;
    const auto span = end - begin;
    const double rate = audio.sampleRate() > 0.0 ? audio.sampleRate() : 48000.0;
    SliceSettings effective = settings;
    const auto minimum = engine::FrameCount(std::clamp(settings.minimumMs * rate / 1000.0, 1.0, double(span)));
    effective.targetCount = std::min(std::clamp(settings.targetCount, 1, 128), int(std::min<engine::FrameCount>(128, span / minimum)));
    if (settings.mode == SliceMode::Grid && settings.gridBeats > 0.0) {
        const double step = rate * 60.0 * settings.gridBeats / std::clamp(settings.sourceBpm, 20.0, 999.0);
        effective.targetCount = int(std::clamp(std::ceil(double(span) / std::max(step, double(minimum))), 1.0, 128.0));
    }
    std::vector<engine::FrameCount> starts;
    switch (settings.mode) {
        case SliceMode::Transients:
            starts = transientStarts(audio, effective, keepGoing);
            if (keepGoing && !keepGoing()) return {};
            if (starts.size() < 2) starts = gridStarts(span, effective);
            break;
        case SliceMode::Random: starts = randomStarts(span, effective); break;
        case SliceMode::Grid:
            if (settings.gridBeats > 0.0) {
                const double step = std::max(double(minimum), rate * 60.0 * settings.gridBeats / std::clamp(settings.sourceBpm, 20.0, 999.0));
                for (int i = 0; i < effective.targetCount; ++i) starts.push_back(engine::FrameCount(double(i) * step));
            } else starts = gridStarts(span, effective);
            break;
        case SliceMode::Manual: starts.push_back(0); break;
    }
    if (starts.empty()) starts.push_back(0);
    tighten(starts);
    std::vector<engine::FrameCount> bounds{begin};
    for (auto relative : starts) {
        if (keepGoing && !keepGoing()) return {};
        auto frame = begin + std::min(relative, span - 1);
        if (frame <= bounds.back() || frame - bounds.back() < minimum || end - frame < minimum) continue;
        if (settings.zeroCrossing) {
            frame = snapToZero(audio, frame, bounds.back()+minimum, end-minimum);
        }
        bounds.push_back(frame);
    }
    bounds.push_back(end);
    for (std::size_t i = 0; i + 1 < bounds.size() && table.count < params::kMaxSlices; ++i) {
        Slice& slice = table.slices[table.count++];
        slice.start = bounds[i]; slice.end = bounds[i + 1];
    }
    assignKeys(table, settings);
    table.rebuild();
    return table;
}

} // namespace daw::slicer
