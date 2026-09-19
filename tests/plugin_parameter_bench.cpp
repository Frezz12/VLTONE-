// Opt-in diagnostic, not a timing-gated test. VST3 automation queue hot path.
#include "Vst3/Vst3Support.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace daw::plugins::vst3;
    for (unsigned count : {128u, 1024u, 4096u}) {
        ParameterChanges changes;
        changes.reserve(count);
        std::vector<double> elapsed;
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            for (int block = 0; block < 256; ++block) {
                changes.clear();
                for (unsigned i = 0; i < count; ++i) {
                    Steinberg::int32 index = -1;
                    auto* queue = changes.addParameterData(i * 17 + 1, index);
                    if (!queue || index != i) throw std::runtime_error("queue identity");
                    Steinberg::int32 point = -1;
                    if (queue->addPoint(0, 0.5, point) != Steinberg::kResultOk)
                        throw std::runtime_error("point");
                }
            }
            elapsed.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::cout << "parameters=" << count << " blocks=256 median_ms=" << elapsed[1] << std::endl;
    }
}
