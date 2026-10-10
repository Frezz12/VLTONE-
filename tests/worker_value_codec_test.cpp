#include "WorkerValueCodec.hpp"

#include <chrono>
#include <cstdio>
#include <limits>

namespace {
using namespace daw;
using namespace worker_value;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}
template<class F> bool rejects(F&& f) {
    try { f(); return false; } catch (const std::exception&) { return true; }
}
struct Packet {
    std::string name;
    std::vector<double> values;
    std::shared_ptr<const engine::SampleBuffer> audio, sameAudio;
};
template<class A> void fields(A& a, Packet& v) { a(v.name, v.values, v.audio, v.sameAudio); }
// A small malicious count must not allocate a huge native element array.
struct LargeValue { std::array<std::uint8_t, 4096> data{}; };
template<class A> void fields(A& a, LargeValue& v) { a(v.data); }
}
int main() try {
    struct Directory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("vlt-worker-codec-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } directory;
    auto pcm = std::make_shared<engine::SampleBuffer>(2, 64, 44100);
    pcm->writableChannel(0)[31] = .25f;
    pcm->writableChannel(1)[63] = -.5f;
    ProcessAudioResources resources(directory.path);
    Packet input{"source", {120, .75}, pcm, pcm};
    const auto bytes = encodeResources(resources, input);
    const auto [copy] = decodeResources<Packet>(bytes, directory.path);
    require(copy.name == input.name && copy.values == input.values && copy.audio == copy.sameAudio &&
            copy.audio->readOnly() && copy.audio->sampleRate() == 44100 &&
            copy.audio->readSample(0, 31) == .25f && copy.audio->readSample(1, 63) == -.5f && resources.records().size() == 1,
            "worker payload preserves values and reuses one immutable PCM resource");
    auto truncated = bytes; truncated.pop_back();
    require(rejects([&] { decodeResources<Packet>(truncated, directory.path); }), "truncated worker payload is rejected");
    auto trailing = bytes; trailing.push_back(0);
    require(rejects([&] { decodeResources<Packet>(trailing, directory.path); }), "trailing worker payload is rejected");
    require(rejects([&] { decode<bool>(std::array<std::uint8_t, 1>{2}); }), "noncanonical boolean is rejected");
    const auto nonfinite = encode(std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()));
    require(rejects([&] { decode<double>(nonfinite); }), "nonfinite worker numbers are rejected");
    require(rejects([&] { decode<std::string>(encode(std::uint32_t(maxString + 1))); }), "oversized string is rejected before allocation");
    auto oversized = encode(std::uint32_t(maxElements + 1));
    require(rejects([&] { decode<std::vector<double>>(oversized); }), "oversized collection is rejected before allocation");
    auto expanded = encode(std::uint32_t(maxDecodedBytes / sizeof(LargeValue) + 1));
    expanded.resize(expanded.size() + maxDecodedBytes / sizeof(LargeValue) + 1);
    require(rejects([&] { decode<std::vector<LargeValue>>(expanded); }), "native expansion respects the cumulative memory budget");
    require(rejects([&] { decode<std::shared_ptr<const engine::SampleBuffer>>(encode(std::string{"pcm-1"})); }),
            "a PCM identity cannot decode without its resource manifest");
    std::filesystem::remove(directory.path / resources.records()[0].fileName);
    require(rejects([&] { decodeResources<Packet>(bytes, directory.path); }), "missing PCM rejects the complete worker payload");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
}
