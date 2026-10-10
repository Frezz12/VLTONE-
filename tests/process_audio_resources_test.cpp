#include "ProcessAudioResources.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using daw::ProcessAudioResources;
using daw::engine::SampleBuffer;

void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
    std::printf("PASS %s\n", what);
}
template<class F> bool rejects(F&& action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
struct TempDirectory {
    fs::path path = fs::temp_directory_path() / ("daw-process-pcm-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
float value(unsigned channel, unsigned frame) {
    return float((channel + 1) * 10) + float(frame % 8192) / 8192.f;
}

}

int main() try {
    TempDirectory temp;
    const auto root = temp.path / fs::path(u8"PCM-ресурсы");
    std::vector<ProcessAudioResources::Record> records;
    ProcessAudioResources::Samples loaded;
    std::weak_ptr<const SampleBuffer> original;
    std::string id;
    constexpr unsigned frames = 70003; // Crosses write and read-cache pages; odd planar stride.
    {
        ProcessAudioResources store(root);
        auto samples = std::make_shared<SampleBuffer>(3, frames, 44100);
        for (unsigned ch = 0; ch < 3; ++ch)
            for (unsigned i = 0; i < frames; ++i) samples->writableChannel(ch)[i] = value(ch, i);
        original = samples;
        id = store.put(samples);
        require(!id.empty() && store.put(samples) == id && store.records().size() == 1,
                "shared sources produce one resource and stable ID");
        require(store.put({}).empty() && store.records().size() == 1, "null resource has no file");
        records = store.records();
        loaded = ProcessAudioResources::load(root, records);
        samples.reset();
        require(!original.expired(), "writer pins resource identity until session transfer ends");
    }
    require(original.expired(), "child mapping has no dependency on original PCM allocation");
    const auto mapped = loaded.at(id);
    require(mapped->fileBacked() && mapped->readOnly() && mapped->channels() == 3 &&
            mapped->frames() == frames && mapped->sampleRate() == 44100,
            "reader maps immutable PCM with exact shape and sample rate");
    for (unsigned ch = 0; ch < 3; ++ch)
        for (unsigned i = 0; i < frames; ++i)
            if (mapped->readSample(ch, i) != value(ch, i)) throw std::runtime_error("planar PCM differs");
    require(mapped->readSample(3, 7) == value(0, 7), "mapped buffer retains channel fallback semantics");
    mapped->prepareRead(4090);
    {
        daw::engine::PcmReadScope realtime(true);
        require(mapped->readSample(2, 4095) == value(2, 4095) &&
                mapped->readSample(2, 4096) == value(2, 4096),
                "prepared readonly mapping uses realtime PCM cache across a page boundary");
    }
    {
        daw::engine::SampleStorage storage(root / records.front().fileName, 3u * frames);
        const auto& read = storage;
        require(storage.data() == nullptr && read.data()[frames + 13] == value(1, 13),
                "readonly storage does not expose writable sample pointers");
    }
    auto invalid = records;
    invalid.front().fileName = "../outside.f32";
    require(rejects([&] { ProcessAudioResources::load(root, invalid); }), "resource paths cannot escape job directory");
    invalid = records; invalid.push_back(records.front());
    require(rejects([&] { ProcessAudioResources::load(root, invalid); }), "duplicate manifest IDs are rejected");
    invalid = records; invalid.front().channels = daw::engine::kMaxChannels + 1;
    require(rejects([&] { ProcessAudioResources::load(root, invalid); }), "oversized channel metadata is rejected");
    invalid = records; invalid.front().sampleRate = std::numeric_limits<double>::quiet_NaN();
    require(rejects([&] { ProcessAudioResources::load(root, invalid); }), "invalid sample rates are rejected");

    const auto truncated = temp.path / "truncated";
    fs::create_directories(truncated);
    fs::copy_file(root / records.front().fileName, truncated / records.front().fileName);
    fs::resize_file(truncated / records.front().fileName, 3ull * frames * sizeof(float) - sizeof(float));
    require(rejects([&] { ProcessAudioResources::load(truncated, records); }), "truncated PCM is rejected before mapping");
    require(rejects([&] { SampleBuffer::mapReadOnly(truncated / records.front().fileName, 3, frames, 44100); }),
            "mapping independently validates the opened file size");

    const auto linkRoot = temp.path / "linked";
    fs::create_directories(linkRoot);
    std::error_code linkError;
    fs::create_symlink(root / records.front().fileName, linkRoot / records.front().fileName, linkError);
    if (!linkError) require(rejects([&] { ProcessAudioResources::load(linkRoot, records); }),
                            "resource symlinks are rejected");
    else std::puts("SKIP symlink creation is unavailable on this host");

    unsigned checks = 0;
    bool cancelling = true;
    ProcessAudioResources interrupted(temp.path / "cancelled", [&] { return cancelling && ++checks >= 4; });
    require(rejects([&] { interrupted.put(mapped); }) && interrupted.records().empty() &&
            fs::is_empty(temp.path / "cancelled"), "mid-transfer cancellation removes partial PCM and manifest entry");
    cancelling = false;
    require(!interrupted.put(mapped).empty(), "cancelled resource writer can retry without stale files");
    require(rejects([&] { ProcessAudioResources::load(root, records, [] { return true; }); }),
            "reader observes cancellation before mapping");
    auto invalidPcm = std::make_shared<SampleBuffer>(1, 1, 48000);
    invalidPcm->writableChannel(0)[0] = std::numeric_limits<float>::infinity();
    ProcessAudioResources nonfinite(temp.path / "nonfinite");
    require(rejects([&] { nonfinite.put(invalidPcm); }) && fs::is_empty(temp.path / "nonfinite"),
            "non-finite PCM is rejected without leaving a partial file");
    require(mapped->readSample(2, frames - 1) == value(2, frames - 1),
            "mapped source remains usable after writer and temporary readers are destroyed");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
