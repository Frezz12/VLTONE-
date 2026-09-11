#include "Transport/AudioPresentationClock.hpp"
#include "Transport/Transport.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
using namespace daw::engine;
int main() {
    AudioPresentationClock clock;
    int failures = 0;
    auto check = [&](bool ok, const char* text) { if (!ok) { ++failures; std::cerr << text << '\n'; } };
    AudioPresentationSnapshot result;
    check(!clock.readAt(0, 0, result), "empty history has no invented audio");
    for (int i = 0; i < 20; ++i)
        clock.publish({i * 480, 480, 48000, 1000000000LL + i * 10000000LL, 1});
    check(clock.readAt(1045000000LL, 1, result) && std::abs(result.secondsAt(1045000000LL) - .045) < 1e-9,
          "read the audible old block, not the newest future block");
    check(clock.readAt(500000000LL, 1, result) && result.secondsAt(500000000LL) == 0,
          "before DAC start show earliest queued sample");
    check(clock.readAt(4000000000LL, 1, result) && result.secondsAt(4000000000LL) == .2,
          "stalled device clamps to last rendered sample");
    check(!clock.readAt(1050000000LL, 2, result), "seek/restart rejects an old generation");
    AudioPresentationReader reader;
    check(reader.readAt(clock, 1045000000LL, 1, result) && result.blockStart == 1920,
          "reader samples one complete audible tuple");
    clock.publish({7777, 480, 48000, 1040000000LL, 1});
    check(reader.readAt(clock, 1045000000LL, 1, result) && result.blockStart == 1920,
          "all editor reads in one frame retain the same tuple");
    check(reader.readAt(clock, 1045000001LL, 1, result) && result.blockStart == 7777,
          "next frame receives new audio publication");
    clock.publish({9999, 480, 48000, 1060000000LL, 2});
    check(reader.readAt(clock, 1050000000LL, 1, result) && result.blockStart == 7777,
          "unavailable history retains the last valid tuple for that reader generation");
    check(!reader.readAt(clock, 1050000000LL, 3, result), "new generation never inherits the reader fallback");
    AudioPresentationClock replacement;
    check(!reader.readAt(replacement, 1050000000LL, 1, result), "a replacement clock never inherits another device history");
    AudioPresentationSnapshot loop{950, 300, 1000, 1000000000LL, 2, 100, 1000, true};
    check(std::abs(loop.secondsAt(1100000000LL) - .15) < 1e-9, "interpolation wraps at loop edge");
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) clock.publish({i * 32, 32, 48000, i * 1000000LL, 3});
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        if (clock.readAt(9000000000LL, 3, result))
            check(result.blockStart == result.outputTimeNs / 1000000 * 32 && result.frames == 32,
                  "concurrent slot reuse never tears fields");
    }
    writer.join();
    Transport transport;
    transport.play();
    transport.setPresentationTiming(1000000000LL, PresentationClockSource::DeviceTimestamp);
    transport.seek(48000);
    transport.advance(32);
    check(!transport.presentationSnapshot(1000000000LL, result),
          "a block begun before seek cannot publish under the new generation");
    transport.setPresentationTiming(1001000000LL, PresentationClockSource::DeviceTimestamp);
    transport.advance(32);
    check(transport.presentationSnapshot(1001000000LL, result) &&
          result.generation == transport.presentationGeneration(), "new callback publishes the new generation");
    Transport delayed;
    delayed.play();
    delayed.setPresentationTiming(1000000000LL, PresentationClockSource::DeviceTimestamp);
    delayed.advance(480, true);
    check(!delayed.presentationSnapshot(1000000000LL, result), "unfinished output is not published");
    delayed.finishPresentationBlock(4800);
    check(delayed.presentationSnapshot(1105000000LL, result) &&
          result.outputTimeNs == 1100000000LL && std::abs(result.secondsAt(1105000000LL) - .005) < 1e-9,
          "graph latency shifts DAC timestamp once without shifting project samples");
    return failures != 0;
}
