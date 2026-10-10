#include "ProcessAudioResources.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace daw {
namespace {
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(std::endian::native == std::endian::little,
              "Process audio resource format requires little-endian float32");
constexpr engine::FrameCount kWriteFrames = 64 * 1024;

void checkCancelled(const ProcessAudioResources::Cancelled& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("audio resource transfer cancelled");
}

void validate(const ProcessAudioResources::Record& record) {
    if (!record.channels || record.channels > engine::kMaxChannels || !record.frames ||
        !std::isfinite(record.sampleRate) || record.sampleRate <= 0 ||
        record.frames > std::numeric_limits<std::size_t>::max() / (record.channels * sizeof(float)))
        throw std::invalid_argument("invalid audio resource shape");
    if (!record.id.starts_with("pcm-") || record.id.size() <= 4 || record.id.size() > 24 ||
        record.id[4] == '0' ||
        !std::all_of(record.id.begin() + 4, record.id.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        record.fileName != record.id + ".f32")
        throw std::invalid_argument("invalid audio resource identity or file name");
}

std::filesystem::path resourceRoot(const std::filesystem::path& path) {
    if (!std::filesystem::is_directory(path)) throw std::runtime_error("audio resource directory is unavailable");
    return std::filesystem::canonical(path);
}
}

ProcessAudioResources::ProcessAudioResources(std::filesystem::path root, Cancelled cancelled)
    : m_cancelled(std::move(cancelled)) {
    checkCancelled(m_cancelled);
    std::filesystem::create_directories(root);
    m_root = resourceRoot(root);
}

std::string ProcessAudioResources::put(const std::shared_ptr<const engine::SampleBuffer>& samples) {
    if (!samples) return {};
    checkCancelled(m_cancelled);
    if (const auto found = m_ids.find(samples.get()); found != m_ids.end()) return found->second;
    if (m_records.size() >= 65536) throw std::runtime_error("audio generation resource capacity exceeded");
    if (m_nextId == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("audio resource ID capacity exceeded");
    Record record;
    record.id = "pcm-" + std::to_string(++m_nextId);
    record.fileName = record.id + ".f32";
    record.channels = samples->channels(); record.frames = samples->frames(); record.sampleRate = samples->sampleRate();
    validate(record);
    const auto destination = m_root / record.fileName;
    const auto temporary = m_root / (record.fileName + ".partial");
    if (std::filesystem::exists(destination) || std::filesystem::exists(temporary))
        throw std::runtime_error("audio resource file already exists");
    try {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create audio resource");
        for (engine::ChannelCount ch = 0; ch < samples->channels(); ++ch) {
            for (engine::FrameCount first = 0; first < samples->frames();) {
                checkCancelled(m_cancelled);
                const auto count = std::min(kWriteFrames, samples->frames() - first);
                // Control-thread streaming reads avoid the realtime cache's
                // deliberate silence-on-miss policy when preparing an export.
                const float* values = samples->channel(ch) + first;
                if (!std::all_of(values, values + count, [](float value) { return std::isfinite(value); }))
                    throw std::runtime_error("audio resource contains non-finite samples");
                out.write(reinterpret_cast<const char*>(values), std::streamsize(count) * sizeof(float));
                if (!out) throw std::runtime_error("cannot write audio resource");
                first += count;
            }
        }
        out.close();
        if (!out) throw std::runtime_error("cannot finish audio resource");
        checkCancelled(m_cancelled);
        std::filesystem::rename(temporary, destination);
    } catch (...) {
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        throw;
    }
    try {
        m_records.push_back(record);
        m_ids.emplace(samples.get(), record.id);
        m_pinned.push_back(samples);
    } catch (...) {
        if (!m_records.empty() && m_records.back().id == record.id) m_records.pop_back();
        if (const auto found = m_ids.find(samples.get()); found != m_ids.end() && found->second == record.id)
            m_ids.erase(found);
        std::error_code ignored; std::filesystem::remove(destination, ignored);
        throw;
    }
    return record.id;
}

ProcessAudioResources::Samples ProcessAudioResources::load(const std::filesystem::path& root,
    const std::vector<Record>& records, Cancelled cancelled) {
    checkCancelled(cancelled);
    const auto directory = resourceRoot(root);
    std::unordered_set<std::string> ids;
    ids.reserve(records.size());
    for (const auto& record : records) {
        checkCancelled(cancelled);
        validate(record);
        if (!ids.insert(record.id).second) throw std::invalid_argument("duplicate audio resource ID");
        const auto file = directory / record.fileName;
        if (std::filesystem::symlink_status(file).type() != std::filesystem::file_type::regular ||
            std::filesystem::file_size(file) != std::uint64_t(record.channels) * record.frames * sizeof(float))
            throw std::runtime_error("invalid audio resource file or size");
    }
    Samples loaded;
    loaded.reserve(records.size());
    for (const auto& record : records) {
        checkCancelled(cancelled);
        loaded.emplace(record.id, engine::SampleBuffer::mapReadOnly(directory / record.fileName,
            engine::ChannelCount(record.channels), record.frames, record.sampleRate));
    }
    return loaded;
}

} // namespace daw
