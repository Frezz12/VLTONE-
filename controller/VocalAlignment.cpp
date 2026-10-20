#include "VocalAlignment.hpp"
#include "WarpAnalysis.hpp"
#include "DSP/WarpPlayback.hpp"
#include "analysis/Signal.hpp"
#include <array>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace daw::alignment {
namespace {
constexpr double rate = 16000, hopSeconds = .005;
constexpr int hop = 80, windowSize = 512, bands = 20;
struct Frame {
    std::array<float, bands> spectrum{};
    float energy = 0, onset = 0, level = 0;
    bool active = false;
};
struct Features { std::vector<Frame> frames; std::vector<analysis::WarpTransient> attacks; };
void checkpoint(const Progress& progress, double value) {
    if (progress && !progress(std::clamp(value, 0., 1.)))
        throw std::runtime_error("Alignment cancelled.");
}
void validate(const AlignmentSource& source) {
    if (!source.audio || !source.audio->channels() || source.audio->channels() > 2 ||
        source.audio->sampleRate() <= 0 || !validWarp(source.baseline) || source.baseline.empty() ||
        !std::isfinite(source.startSeconds) || source.startSeconds < 0 ||
        !std::isfinite(source.durationSeconds) || source.durationSeconds <= 0 ||
        !std::isfinite(source.tempo) || source.tempo <= 0 ||
        !std::isfinite(source.pitch) || !std::isfinite(source.formant) ||
        warpMaximumRatio(source.baseline, source.tempo) > 1000)
        throw std::runtime_error("The clip has no supported readable audio.");
}
Features extract(const AlignmentSource& source, const Progress& progress) {
    validate(source);
    Features out;
    const auto count = std::size_t(std::ceil(source.durationSeconds / hopSeconds));
    out.frames.reserve(count);
    engine::SampleBuffer audible(source.audio->channels(), engine::FrameCount(std::ceil(source.durationSeconds * rate)), rate);
    engine::dsp::WarpPlayback playback(source.baseline, source.tempo, rate);
    const auto window = analysis::detail::hann(windowSize);
    std::array<std::array<float, windowSize>, 2> ring{};
    std::array<std::array<float, hop>, 2> rendered{};
    std::vector<std::complex<double>> fft(windowSize);
    std::array<double, bands> previous{};
    std::array<int, windowSize / 2 + 1> bandIndex{};
    for (int i = 0; i <= windowSize / 2; ++i) {
        const double mel = 2595 * std::log10(1 + (i * rate / windowSize) / 700.);
        bandIndex[i] = std::clamp(int(mel / (2595 * std::log10(1 + 8000. / 700)) * bands), 0, bands - 1);
    }
    double maxEnergy = 0, maxOnset = 0;
    for (std::size_t f = 0; f < count; ++f) {
        if ((f & 63) == 0) checkpoint(progress, double(f) / std::max<std::size_t>(1, count));
        playback.render(*source.audio, f * hopSeconds, source.pitch, source.formant,
                        rendered[0].data(), rendered[1].data(), hop);
        for (int ch = 0; ch < source.audio->channels(); ++ch)
            std::copy_n(rendered[ch].data(), std::min<std::size_t>(hop, audible.frames() - f * hop), audible.writableChannel(ch) + f * hop);
        std::array<double, bands> power{};
        double energy = 0;
        for (int ch = 0; ch < source.audio->channels(); ++ch) {
            std::move(ring[ch].begin() + hop, ring[ch].end(), ring[ch].begin());
            std::copy(rendered[ch].begin(), rendered[ch].end(), ring[ch].end() - hop);
            for (int i = 0; i < windowSize; ++i) {
                const double sample = std::isfinite(ring[ch][i]) ? ring[ch][i] : 0;
                fft[i] = sample * window[i];
                energy += sample * sample;
            }
            analysis::detail::fft(fft);
            // Average powers, never samples: antiphase stereo remains audible
            // to the detector and both playback channels share the same map.
            for (int i = 1; i <= windowSize / 2; ++i)
                power[bandIndex[i]] += std::norm(fft[i]);
        }
        Frame frame;
        frame.energy = float(std::sqrt(energy / (windowSize * source.audio->channels())));
        double mean = 0;
        for (int b = 0; b < bands; ++b) {
            const double value = std::log1p(power[b]);
            frame.onset += float(std::max(0., value - previous[b]));
            previous[b] = value;
            frame.spectrum[b] = float(value); mean += value / bands;
        }
        double norm = 0;
        for (auto& value : frame.spectrum) { value -= float(mean); norm += value * value; }
        norm = std::sqrt(norm);
        if (norm > 1e-8) for (auto& value : frame.spectrum) value /= float(norm);
        maxEnergy = std::max(maxEnergy, double(frame.energy));
        maxOnset = std::max(maxOnset, double(frame.onset));
        out.frames.push_back(frame);
    }
    std::vector<float> energies;
    energies.reserve(count);
    for (const auto& frame : out.frames) energies.push_back(frame.energy);
    const auto quiet = energies.begin() + energies.size() / 10;
    std::nth_element(energies.begin(), quiet, energies.end());
    const double floor = std::max(1e-6, std::min(maxEnergy * .1, std::max(maxEnergy * .015, double(*quiet) * 2)));
    for (auto& frame : out.frames) {
        frame.active = frame.energy > floor;
        frame.level = float(std::clamp(std::log(std::max(1e-8f, frame.energy) / std::max(1e-8, maxEnergy)) / 5 + 1, 0., 1.));
        frame.energy = float(std::sqrt(frame.energy / std::max(maxEnergy, 1e-8)));
        frame.onset /= float(std::max(maxOnset, 1e-8));
    }
    out.attacks = analysis::detectWarpTransients(audible, 0, source.durationSeconds, [&] { checkpoint(progress, .99); return true; });
    checkpoint(progress, 1);
    return out;
}
double distance(const Frame& a, const Frame& b) {
    if (!a.active && !b.active) return .05;
    if (a.active != b.active) return 1.5;
    const double cosine = std::inner_product(a.spectrum.begin(), a.spectrum.end(), b.spectrum.begin(), 0.);
    return .65 * (1 - std::clamp(cosine, -1., 1.)) + .2 * std::abs(a.level - b.level) + .15 * std::abs(a.onset - b.onset);
}
std::pair<int, double> globalShift(const Features& guide, const Features& target, int offset, int limit,
                                  const Progress& progress) {
    int best = 0;
    double bestScore = -1;
    for (int lag = -limit; lag <= limit; ++lag) {
        if ((lag + limit) % 32 == 0) checkpoint(progress, double(lag + limit) / std::max(1, 2 * limit));
        double dot = 0, aa = 0, bb = 0, similarity = 0, count = 0, active = 0;
        for (int i = 0; i < int(target.frames.size()); i += 2) {
            const auto& a = target.frames[i];
            if (!a.active) continue;
            ++active;
            const int j = i + offset + lag;
            if (j < 0 || j >= int(guide.frames.size())) continue;
            const auto& b = guide.frames[j];
            const double x = a.onset + .2 * a.energy, y = b.onset + .2 * b.energy;
            dot += x * y; aa += x * x; bb += y * y;
            similarity += std::max(0., 1 - distance(a, b)); ++count;
        }
        const double score = count > 3 && aa * bb > 1e-10
            ? (.6 * dot / std::sqrt(aa * bb) + .4 * similarity / count) * count / std::max(1., active) : 0;
        if (score > bestScore + 1e-9 || (std::abs(score - bestScore) < 1e-9 && std::abs(lag) < std::abs(best))) {
            best = lag; bestScore = score;
        }
    }
    return {best, bestScore};
}

// Banded DP with (1,1), (1,2), (2,1) steps. Only three cost rows live at
// once; traceback stores one byte per cell, never a full N*M cost matrix.
std::vector<double> path(const Features& guide, const Features& target, int offset, int lag, int limit,
                         int stride, const std::vector<double>& coarse, const Progress& progress) {
    struct Row { int low = 0; std::vector<unsigned char> trace; };
    struct Costs {
        int low = 0;
        std::vector<double> values;
        double at(int j) const { return j >= low && j - low < int(values.size()) ? values[j - low] : 1e30; }
    };
    const int n = int(target.frames.size()), m = int(guide.frames.size());
    const int first = std::max(0, (-offset - limit + stride - 1) / stride);
    const int end = std::min((n + stride - 1) / stride, (m - offset + limit + stride - 1) / stride);
    std::vector<double> result(n, std::numeric_limits<double>::quiet_NaN());
    if (end <= first) return result;
    std::vector<Row> rows(end - first);
    std::array<Costs, 3> costs;
    int last = first - 1, lastJ = -1;
    for (int row = first; row < end; ++row) {
        if ((row & 63) == 0) checkpoint(progress, double(row - first) / (end - first));
        const int i = row * stride;
        double center = i + offset + lag;
        int radius = limit;
        if (!coarse.empty() && std::isfinite(coarse[i])) { center = coarse[i]; radius = 10; }
        const int lo = std::max({0, int(std::ceil((center - radius) / stride)), int(std::ceil(double(i + offset - limit) / stride))});
        const int hi = std::min({(m - 1) / stride, int(std::floor((center + radius) / stride)), int(std::floor(double(i + offset + limit) / stride))});
        if (hi < lo) continue;
        auto& current = costs[(row - first) % 3];
        const auto& prev = costs[(row - first + 2) % 3];
        const auto& prev2 = costs[(row - first + 1) % 3];
        current.low = lo; current.values.assign(hi - lo + 1, 1e30);
        auto& stored = rows[row - first]; stored.low = lo; stored.trace.resize(hi - lo + 1);
        double minimum = 1e30; int rowBest = -1;
        for (int j = lo; j <= hi; ++j) {
            const double local = distance(target.frames[i], guide.frames[j * stride]);
            double best = row == first ? local + .02 * std::abs(j * stride - center) / std::max(1, limit) : prev.at(j - 1) + local;
            unsigned char step = 1;
            if (row > first) {
                const double wide = prev.at(j - 2) + local * 1.5 + .07;
                if (wide < best) { best = wide; step = 2; }
            }
            if (row > first + 1) {
                const double tall = prev2.at(j - 1) + local * 1.5 + .07;
                if (tall < best) { best = tall; step = 3; }
            }
            current.values[j - lo] = best; stored.trace[j - lo] = step;
            if (best < minimum) { minimum = best; rowBest = j; }
        }
        if (minimum < 1e29) { last = row; lastJ = rowBest; }
    }
    if (last < first || lastJ < 0) return result;
    std::vector<std::pair<int, int>> points;
    for (int row = last, j = lastJ; row >= first;) {
        const auto& stored = rows[row - first];
        if (j < stored.low || j - stored.low >= int(stored.trace.size())) break;
        points.emplace_back(row * stride, j * stride);
        const int step = stored.trace[j - stored.low];
        row -= step == 3 ? 2 : 1; j -= step == 2 ? 2 : 1;
    }
    std::reverse(points.begin(), points.end());
    for (std::size_t p = 1; p < points.size(); ++p) {
        const auto [a, x] = points[p - 1]; const auto [b, y] = points[p];
        for (int i = a; i <= b; ++i) result[i] = x + double(i - a) * (y - x) / (b - a);
    }
    return result;
}

// Refine only near-identical recordings by waveform correlation. Independent
// takes use feature alignment; their pitch cycles are not timing anchors.
double refineShift(const AlignmentSource& guide, const AlignmentSource& target, double shift) {
    if (guide.audio->sampleRate() != target.audio->sampleRate() || guide.pitch != target.pitch ||
        guide.formant != target.formant || guide.baseline.markers.size() != 2 || target.baseline.markers.size() != 2 ||
        std::abs(warpSpeedAt(guide.baseline, 0, guide.tempo) - 1) > 1e-8 ||
        std::abs(warpSpeedAt(target.baseline, 0, target.tempo) - 1) > 1e-8) return shift;
    const double sr = guide.audio->sampleRate();
    const int radius = int(std::ceil(hopSeconds * sr));
    const auto& a = *target.audio; const auto& b = *guide.audio;
    const double origin = target.startSeconds - guide.startSeconds + guide.baseline.markers.front().sourceSeconds - target.baseline.markers.front().sourceSeconds;
    const auto first = std::int64_t(target.baseline.markers.front().sourceSeconds * sr);
    const auto end = std::min<std::int64_t>(a.frames(), first + std::int64_t(std::min(target.durationSeconds, 3.) * sr));
    double best = .985; int bestOffset = int(std::llround((origin + shift) * sr));
    const int centre = bestOffset;
    for (int offset = centre - radius; offset <= centre + radius; ++offset) {
        double aa = 0, bb = 0, ab = 0;
        for (auto i = first; i < end; i += 16) {
            const auto j = i + offset;
            if (j < 0 || j >= b.frames()) continue;
            const double x = a.channel(0)[i], y = b.channel(0)[j];
            aa += x*x; bb += y*y; ab += x*y;
        }
        const double corr = aa * bb > 1e-10 ? ab / std::sqrt(aa * bb) : 0;
        if (corr > best) { best = corr; bestOffset = offset; }
    }
    return best > .985 ? bestOffset / sr - origin : shift;
}

AlignmentProposal propose(const AlignmentSource& guide, const Features& gf, const AlignmentSource& source,
                           const Features& tf, const AlignmentSettings& settings, const Progress& progress) {
    AlignmentProposal result;
    result.map = source.baseline; result.startSeconds = source.startSeconds; result.durationSeconds = source.durationSeconds;
    if (settings.strength == 0) { result.reason = "Zero density: unchanged."; return result; }
    const int offset = int(std::llround((source.startSeconds - guide.startSeconds) / hopSeconds));
    const int limit = int(std::ceil(settings.maximumShiftSeconds / hopSeconds));
    const auto [lag, score] = globalShift(gf, tf, offset, limit, [&](double p) { checkpoint(progress, p * .2); return true; });
    if (score < .35) { result.reason = "No reliable matching phrase nearby."; return result; }
    if (settings.method == AlignmentMethod::Shift) {
        double shift = refineShift(guide, source, lag * hopSeconds);
        shift = std::clamp(shift, -settings.maximumShiftSeconds, settings.maximumShiftSeconds);
        if (std::abs(shift) <= settings.toleranceSeconds) { result.reason = "Already within tolerance."; return result; }
        for (std::size_t i = 1; i + 1 < source.baseline.markers.size(); ++i)
            if (source.baseline.markers[i].locked) { result.reason = "Locked markers prevent a whole-clip shift."; return result; }
        result.startSeconds = std::max(0., source.startSeconds + shift * settings.strength);
        result.maximumShiftSeconds = std::abs(result.startSeconds - source.startSeconds);
        result.matched = 1; result.status = AlignmentStatus::Aligned;
        result.rigidShift = true;
        return result;
    }
    std::vector<double> mapping;
    if (settings.method == AlignmentMethod::Hybrid) {
        auto coarse = path(gf, tf, offset, lag, limit, 4, {}, [&](double p) { checkpoint(progress, .2 + .25*p); return true; });
        mapping = path(gf, tf, offset, lag, limit, 1, coarse, [&](double p) { checkpoint(progress, .45 + .35*p); return true; });
    } else {
        mapping.resize(tf.frames.size());
        for (std::size_t i = 0; i < mapping.size(); ++i) mapping[i] = i + offset + lag;
    }
    struct Anchor { double time, shift; bool fixed; };
    std::vector<Anchor> anchors{{0, 0, true}, {source.durationSeconds, 0, true}};
    for (std::size_t k = 1; k + 1 < source.baseline.markers.size(); ++k) {
        const auto& marker = source.baseline.markers[k];
        if (marker.locked) anchors.push_back({marker.targetBeats * 60 / source.tempo, 0, true});
    }
    // Analysis windows trail by half a window. Use the same offset for both
    // sides, then refine each onset in a small neighborhood of the DP path.
    constexpr double windowDelay = (windowSize / 2. - hop) / rate;
    const auto attackTime = [](const Features& features, double approximate) {
        double result = approximate, delta = .025;
        for (const auto& transient : features.attacks) {
            if (transient.sourceSeconds > approximate + .025) break;
            const double difference = std::abs(transient.sourceSeconds - approximate);
            if (difference < delta) { result = transient.sourceSeconds; delta = difference; }
        }
        return result;
    };
    double lastRegular = -1;
    for (int i = 1; i + 1 < int(tf.frames.size()); ++i) {
        const auto& frame = tf.frames[i];
        if (!frame.active) continue;
        const bool attack = frame.onset > .08 && frame.onset >= tf.frames[i - 1].onset && frame.onset > tf.frames[i + 1].onset;
        const double time = attack ? attackTime(tf, i * hopSeconds - windowDelay) : i * hopSeconds - windowDelay;
        if (time < .02 || time > source.durationSeconds - .02) continue;
        if (!attack && (settings.method == AlignmentMethod::Attacks || time - lastRegular < .2)) continue;
        int j = std::isfinite(mapping[i]) ? int(std::llround(mapping[i])) : -1;
        if (attack && j >= 0) {
            double strongest = -1; int match = -1;
            const int radius = settings.method == AlignmentMethod::Attacks ? std::min(limit, 24) : 6;
            for (int k = std::max(1, j - radius); k <= std::min(int(gf.frames.size()) - 2, j + radius); ++k) {
                const auto& candidate = gf.frames[k];
                if (!candidate.active || candidate.onset < .06 || candidate.onset < gf.frames[k - 1].onset || candidate.onset <= gf.frames[k + 1].onset) continue;
                const double strength = candidate.onset * std::max(0., 1 - distance(frame, candidate)) / (1 + .07 * std::abs(k - j));
                if (strength > strongest) { strongest = strength; match = k; }
            }
            j = match;
        }
        if (j < 0 || j >= int(gf.frames.size()) || distance(frame, gf.frames[j]) > .55) {
            anchors.push_back({time, 0, true}); ++result.skipped; continue;
        }
        const double destination = guide.startSeconds + (attack ? attackTime(gf, j * hopSeconds - windowDelay) : j * hopSeconds - windowDelay);
        double shift = destination - (source.startSeconds + time);
        if (std::abs(shift) > settings.maximumShiftSeconds + 1e-9) {
            anchors.push_back({time, 0, true}); ++result.skipped; continue;
        }
        shift = std::abs(shift) <= settings.toleranceSeconds ? 0 : shift * settings.strength;
        anchors.push_back({time, shift, false}); lastRegular = time;
    }
    std::sort(anchors.begin(), anchors.end(), [](auto a, auto b) { return a.time != b.time ? a.time < b.time : a.fixed > b.fixed; });
    std::vector<Anchor> sparse;
    for (const auto& anchor : anchors) {
        if (!sparse.empty() && anchor.time - sparse.back().time < .035 && !anchor.fixed) continue;
        if (!sparse.empty() && anchor.time - sparse.back().time < 1e-7) continue;
        sparse.push_back(anchor);
    }
    // Spend timing changes inside the longest pause between anchors. On each
    // side, the sounding material keeps a constant offset instead of being
    // stretched merely because its neighboring phrase arrived late.
    std::vector<Anchor> withPauses;
    for (std::size_t k = 1; k < sparse.size(); ++k) {
        const auto& a = sparse[k-1]; const auto& b = sparse[k];
        withPauses.push_back(a);
        if (a.shift == b.shift) continue;
        int begin = -1, bestBegin = -1, bestEnd = -1;
        for (int f = std::max(0, int(std::ceil((a.time + .025) / hopSeconds))); f < std::min(int(tf.frames.size()), int((b.time - .025) / hopSeconds)); ++f) {
            if (!tf.frames[f].active) { if (begin < 0) begin = f; }
            else if (begin >= 0) { if (f - begin > bestEnd - bestBegin) { bestBegin = begin; bestEnd = f; } begin = -1; }
        }
        const int end = std::min(int(tf.frames.size()), int((b.time - .025) / hopSeconds));
        if (begin >= 0 && end - begin > bestEnd - bestBegin) { bestBegin = begin; bestEnd = end; }
        if (bestEnd - bestBegin >= 12) {
            withPauses.push_back({(bestBegin + 2) * hopSeconds, a.shift, a.fixed});
            withPauses.push_back({(bestEnd - 2) * hopSeconds, b.shift, b.fixed});
        }
    }
    withPauses.push_back(sparse.back()); sparse = std::move(withPauses);
    // Reject unsafe corrections instead of clamping them into a plausible
    // looking but unrelated match. Fixed anchors fence off uncertain regions.
    bool changed = true;
    while (changed) {
        changed = false;
        if (sparse.size() > 2) {
            sparse.front().shift = std::max(-source.startSeconds, sparse[1].shift);
            sparse.back().shift = sparse[sparse.size() - 2].shift;
        }
        for (std::size_t i = 1; i < sparse.size(); ++i) {
            auto& a = sparse[i - 1]; auto& b = sparse[i];
            const double duration = b.time - a.time;
            const double ratio = (duration + b.shift - a.shift) / duration;
            bool voiced = false;
            for (int f = std::max(0, int(a.time / hopSeconds)); f < std::min(int(tf.frames.size()), int(b.time / hopSeconds) + 1); ++f)
                voiced |= tf.frames[f].active;
            const double ceiling = voiced ? settings.maximumStretch : 8.;
            if (ratio >= 1 / ceiling && ratio <= ceiling) continue;
            if (!a.fixed && a.shift != 0) { a.shift = 0; a.fixed = true; ++result.skipped; changed = true; }
            if (!b.fixed && b.shift != 0) { b.shift = 0; b.fixed = true; ++result.skipped; changed = true; }
        }
    }
    const auto shiftAt = [&](double time) {
        auto next = std::upper_bound(sparse.begin(), sparse.end(), time, [](double t, const auto& a) { return t < a.time; });
        const auto index = std::clamp<std::ptrdiff_t>(next - sparse.begin() - 1, 0, sparse.size() - 2);
        const auto& a = sparse[index]; const auto& b = sparse[index + 1];
        return a.shift + (time - a.time) * (b.shift - a.shift) / (b.time - a.time);
    };
    result.map = source.baseline;
    const double startShift = sparse.front().shift;
    result.startSeconds = source.startSeconds + startShift;
    result.durationSeconds = source.durationSeconds + sparse.back().shift - startShift;
    for (auto& marker : result.map.markers) {
        const double time = marker.targetBeats * 60 / source.tempo;
        marker.targetBeats = (time + shiftAt(time) - startShift) * source.tempo / 60;
    }
    std::size_t nextId = 0;
    for (const auto& anchor : sparse) {
        if (std::abs(anchor.shift) > 1e-9) { ++result.matched; result.maximumShiftSeconds = std::max(result.maximumShiftSeconds, std::abs(anchor.shift)); }
        const double originalBeat = anchor.time * source.tempo / 60;
        const double src = warpSourceAt(source.baseline, originalBeat);
        if (std::any_of(result.map.markers.begin(), result.map.markers.end(), [&](const auto& p) { return std::abs(p.sourceSeconds - src) < 1e-7; })) continue;
        std::string id;
        do { id = "alignment-" + std::to_string(nextId++); }
        while (std::any_of(result.map.markers.begin(), result.map.markers.end(), [&](const auto& p) { return p.id == id; }));
        result.map.markers.push_back({id, src, (anchor.time + anchor.shift - startShift) * source.tempo / 60, false});
    }
    if (!result.matched) {
        result.map = source.baseline; result.startSeconds = source.startSeconds; result.durationSeconds = source.durationSeconds;
        result.reason = result.skipped ? "Uncertain or excessive changes were left untouched." : "Already within tolerance."; return result;
    }
    std::sort(result.map.markers.begin(), result.map.markers.end(), [](const auto& a, const auto& b) { return a.sourceSeconds < b.sourceSeconds; });
    result.map.mode = 3; result.map.preservePitch = true; result.map.enabled = true;
    if (!validWarp(result.map)) throw std::runtime_error("The alignment could not produce a valid Warp map.");
    result.status = result.skipped ? AlignmentStatus::Partial : AlignmentStatus::Aligned;
    if (result.skipped) result.reason = "Uncertain or excessive changes were left untouched.";
    return result;
}
} // namespace

std::vector<AlignmentProposal> align(const AlignmentSource& guide, const std::vector<AlignmentSource>& targets,
                                     const AlignmentSettings& settings, const Progress& progress) {
    if (!std::isfinite(settings.strength) || settings.strength < 0 || settings.strength > 1 ||
        !std::isfinite(settings.toleranceSeconds) || settings.toleranceSeconds < 0 || settings.toleranceSeconds > .2 ||
        !std::isfinite(settings.maximumShiftSeconds) || settings.maximumShiftSeconds < .001 || settings.maximumShiftSeconds > 2 ||
        !std::isfinite(settings.maximumStretch) || settings.maximumStretch < 1 || settings.maximumStretch > 1.25 ||
        int(settings.method) < 0 || int(settings.method) > 2)
        throw std::runtime_error("Invalid alignment settings.");
    checkpoint(progress, 0);
    if (settings.strength == 0) {
        validate(guide);
        std::vector<AlignmentProposal> unchanged;
        for (const auto& target : targets) {
            validate(target);
            AlignmentProposal proposal; proposal.map = target.baseline;
            proposal.startSeconds = target.startSeconds; proposal.durationSeconds = target.durationSeconds;
            proposal.reason = "Zero density: unchanged."; unchanged.push_back(std::move(proposal));
        }
        checkpoint(progress, 1); return unchanged;
    }
    const auto gf = extract(guide, [&](double p) { checkpoint(progress, p / (1 + targets.size())); return true; });
    std::vector<AlignmentProposal> results; results.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto report = [&](double p) { checkpoint(progress, (1 + i + p) / (1 + targets.size())); return true; };
        const auto features = extract(targets[i], [&](double p) { return report(p * .4); });
        results.push_back(propose(guide, gf, targets[i], features, settings, [&](double p) { return report(.4 + .6*p); }));
    }
    checkpoint(progress, 1);
    return results;
}
} // namespace daw::alignment
