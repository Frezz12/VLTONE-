#include "DSP/WarpPlayback.hpp"
#include "Nodes/PlaybackNodes.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>

static thread_local bool watch = false;
static int allocations = 0;
void* operator new(std::size_t size) {
    if (watch) ++allocations;
    if (auto* value = std::malloc(std::max<std::size_t>(1, size))) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }

int main() {
    using namespace daw;
    int failed = 0;
    const auto check = [&](bool pass, const char* label) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", label); failed += !pass; };
    auto audio = std::make_shared<engine::SampleBuffer>(2, 96000, 48000);
    ClipWarpModel map; map.enabled = true; map.baselineDurationSeconds = 2;
    map.markers = {{"a", 0, 0, true}, {"b", .5, 1.5, false}, {"c", 2, 4, true}};
    {
        engine::ClipPlayerNode player;
        player.prepare({48000, 128, 2});
        auto second = std::make_shared<engine::SampleBuffer>(2, 64, 48000);
        auto clips = std::make_shared<engine::ClipPlayerNode::ClipList>();
        engine::ClipPlacement first; first.clipId = "shared-comp-id"; first.audio = audio;
        first.warp = std::make_shared<const ClipWarpModel>(map); first.lengthSamples = 96000;
        clips->push_back(first);
        auto other = first; other.audio = second; other.startSample = 100000;
        clips->push_back(other);
        auto duplicate = first; duplicate.startSample = 200000;
        clips->push_back(duplicate);
        for (int i = 0; i < 7; ++i) {
            duplicate.startSample = 300000 + i * 10000;
            clips->push_back(duplicate);
        }
        for (int i = 0; i < 32; ++i) {
            engine::ClipPlacement empty;
            empty.clipId = std::to_string(i); empty.startSample = 500000 + i * 1000;
            clips->push_back(empty);
        }
        player.setClips(clips);
        const auto before = player.clips();
        std::reverse(clips->begin(), clips->end());
        player.setClips(clips);
        const auto after = player.clips();
        check((*after)[0].warpPlayback == (*before)[0].warpPlayback &&
              (*after)[1].warpPlayback == (*before)[1].warpPlayback &&
              (*after)[2].warpPlayback == (*before)[0].warpPlayback &&
              (*after)[9].warpPlayback == (*before)[0].warpPlayback,
              "warp reuse retains first matching clip id and source across reordered comp slices");
    }
    for (int kind = 0; kind < 2; ++kind) {
        for (unsigned i = 0; i < audio->frames(); ++i) {
            const int phase = int(i) % 24000;
            const float value = kind ? float(.5 * std::sin(2 * std::numbers::pi * 220 * i / 48000.)) :
                phase < 400 ? float(.8 * std::exp(-phase / 70.)) : 0;
            audio->writableChannel(0)[i] = value; audio->writableChannel(1)[i] = -.7f * value;
        }
        engine::dsp::WarpPlayback player(map, 120, 48000);
        std::vector<float> left(96000), right(96000);
        watch = true;
        for (int i = 0; i < 96000; i += 157)
            player.render(*audio, i / 48000., 0, 0, left.data() + i, right.data() + i, std::min(157, 96000 - i));
        watch = false;
        check(!allocations, "Warp render and initial seek allocate nothing");
        if (!kind) for (double source : {.5, 1., 1.5}) {
            const int wanted = int(std::lround(warpBeatAt(map, source) * .5 * 48000));
            auto peak = std::max_element(left.begin() + wanted - 3000, left.begin() + wanted + 3000);
            const int offset = int(peak - left.begin()) - wanted;
            std::printf("source=%.3f expected=%d offset=%d peak=%.3f\n", source, wanted, offset, *peak);
            check(std::abs(offset) < 480 && *peak > .3f, "every warped attack follows the map within 10ms");
        }
        double error = 0; int where = 0;
        for (int i = 0; i < 96000; ++i) if (double e = std::abs(right[i] + .7f * left[i]); e > error) { error = e; where = i; }
        std::printf("stereo error %.9f at %d\n", error, where);
        // Match the existing TimeStretch stereo tolerance, including the
        // initial seek transient rather than measuring only steady state.
        check(error < .003, "Warp preserves correlated stereo through rate changes");
        if (kind) {
            auto playing = std::make_shared<engine::dsp::WarpPlayback>(map, 120, 48000);
            engine::dsp::WarpPlayback reference(map, 120, 48000);
            std::array<float, 2048> l{}, r{}, expected{}, ignored{};
            playing->render(*audio, 0, 0, 0, l.data(), r.data(), 157);
            reference.render(*audio, 0, 0, 0, expected.data(), ignored.data(), 157);
            auto edit = map; edit.markers[1].targetBeats = 1.25;
            engine::dsp::WarpPlayback changed(edit, 120, 48000, playing);
            reference.render(*audio, 157 / 48000., 0, 0, expected.data(), ignored.data(), 2048);
            watch = true;
            changed.render(*audio, 157 / 48000., 0, 0, l.data(), r.data(), 2048);
            watch = false;
            check(l[0] == expected[0], "live edit starts at the next buffered audio sample");
            double jump = 0;
            for (int i = 1; i < 2048; ++i) jump = std::max(jump, double(std::abs(l[i] - l[i - 1])));
            check(jump < .12 && !allocations, "live edit crossfade is continuous and allocation-free");
            changed.render(*audio, 1.1, 0, 0, l.data(), r.data(), 2048);
            const auto seek = l;
            changed.render(*audio, 0, 0, 0, l.data(), r.data(), 2048);
            changed.render(*audio, 1.1, 0, 0, l.data(), r.data(), 2048);
            check(l == seek, "transport seek and loop retrigger the map consistently");
        }
    }
    return failed ? 1 : 0;
}
