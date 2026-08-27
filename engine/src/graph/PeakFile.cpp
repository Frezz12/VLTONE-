#include "daw/graph/PeakFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace daw::graph {

std::uint64_t PeakFile::hashPath(const std::string& path)
{
    std::uint64_t h = 14695981039346656037ULL;
    for (char c : path) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string PeakFile::getPeakPath(const std::string& audioPath)
{
    namespace fs = std::filesystem;
    auto p = fs::path(audioPath);
    auto hash = hashPath(fs::absolute(p).string());
    return (p.parent_path() / (p.stem().string() + "_" + std::to_string(hash) + ".peak")).string();
}

bool PeakFile::build(const model::Source& source, int level0Frames)
{
    level0Frames_ = level0Frames;

    const std::int64_t totalFrames = source.frames();
    const int channels = source.channels();

    if (totalFrames <= 0 || channels <= 0)
        return false;

    levels_.clear();

    const std::int64_t numBlocks = (totalFrames + level0Frames - 1) / level0Frames;
    std::vector<PeakPair> l0;
    l0.reserve(static_cast<std::size_t>(numBlocks));

    for (std::int64_t block = 0; block < numBlocks; ++block) {
        const std::int64_t start = block * level0Frames;
        const std::int64_t end = std::min(start + level0Frames, totalFrames);
        const int count = static_cast<int>(end - start);

        float maxVal = 0.0f;
        float minVal = 0.0f;

        // Данные иcточника лежат ЧЕРЕДУЯCЬ: frame * channels + channel.
        // Указатель уже cмещён на start, поэтому кадр i начинаетcя
        // c индекcа i * channels.
        const float* data = source.readFrames(start, count);
        if (data) {
            const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(channels);
            for (int i = 0; i < count; ++i) {
                const float* frame = data + static_cast<std::ptrdiff_t>(i) * stride;
                for (int c = 0; c < channels; ++c) {
                    const float sample = frame[c];
                    maxVal = std::max(maxVal, sample);
                    minVal = std::min(minVal, sample);
                }
            }
        }

        PeakPair pair;
        pair.max = static_cast<std::int16_t>(std::clamp(maxVal * 32767.0f, -32768.0f, 32767.0f));
        pair.min = static_cast<std::int16_t>(std::clamp(minVal * 32767.0f, -32768.0f, 32767.0f));
        l0.push_back(pair);
    }

    levels_.push_back(std::move(l0));

    while (levels_.back().size() > 1) {
        const auto& prev = levels_.back();
        std::vector<PeakPair> next;
        next.reserve((prev.size() + 1) / 2);

        for (std::size_t i = 0; i < prev.size(); i += 2) {
            PeakPair p;
            p.min = std::min(prev[i].min,
                i + 1 < prev.size() ? prev[i + 1].min : prev[i].min);
            p.max = std::max(prev[i].max,
                i + 1 < prev.size() ? prev[i + 1].max : prev[i].max);
            next.push_back(p);
        }
        levels_.push_back(std::move(next));
    }

    return true;
}

int PeakFile::framesPerLevel(int level) const noexcept
{
    if (level < 0 || level >= numLevels())
        return 0;
    return level0Frames_ * (1 << level);
}

bool PeakFile::saveToFile(const std::string& path) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;

    std::uint32_t magic = kMagic;
    std::uint32_t version = kVersion;
    std::uint32_t numLvls = static_cast<std::uint32_t>(levels_.size());
    std::uint32_t l0Frames = static_cast<std::uint32_t>(level0Frames_);

    f.write(reinterpret_cast<const char*>(&magic), 4);
    f.write(reinterpret_cast<const char*>(&version), 4);
    f.write(reinterpret_cast<const char*>(&numLvls), 4);
    f.write(reinterpret_cast<const char*>(&l0Frames), 4);

    for (const auto& lvl : levels_) {
        std::uint32_t count = static_cast<std::uint32_t>(lvl.size());
        f.write(reinterpret_cast<const char*>(&count), 4);
        f.write(reinterpret_cast<const char*>(lvl.data()),
            static_cast<std::streamsize>(count * sizeof(PeakPair)));
    }

    return f.good();
}

bool PeakFile::loadFromFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t numLvls = 0;
    std::uint32_t l0Frames = 0;

    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&numLvls), 4);
    f.read(reinterpret_cast<char*>(&l0Frames), 4);

    if (!f || magic != kMagic || version != kVersion)
        return false;

    // Пик-файл — регенерируемый кэш, ему нельзя доверять на cлово: битый или
    // обрезанный файл должен приводить к перепоcтроению, а не к падению.
    if (numLvls == 0 || numLvls > kMaxLevels || l0Frames == 0)
        return false;

    std::vector<std::vector<PeakPair>> levels;
    levels.reserve(numLvls);

    for (std::uint32_t lvl = 0; lvl < numLvls; ++lvl) {
        std::uint32_t count = 0;
        f.read(reinterpret_cast<char*>(&count), 4);
        if (!f)
            return false;

        if (count > kMaxPairsPerLevel)
            return false;

        std::vector<PeakPair> pairs(count);
        if (count > 0) {
            f.read(reinterpret_cast<char*>(pairs.data()),
                   static_cast<std::streamsize>(count * sizeof(PeakPair)));
            if (!f)
                return false;
        }

        levels.push_back(std::move(pairs));
    }

    levels_ = std::move(levels);
    level0Frames_ = static_cast<int>(l0Frames);
    return true;
}

} // namespace daw::graph
