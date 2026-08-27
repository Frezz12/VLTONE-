#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daw/model/Source.h"

namespace daw::graph {

class PeakFile {
public:
    bool build(const model::Source& source, int level0Frames = 256);

    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;

    static std::string getPeakPath(const std::string& audioPath);

    int numLevels() const noexcept { return static_cast<int>(levels_.size()); }
    int framesPerLevel(int level) const noexcept;

    struct PeakPair {
        std::int16_t min;
        std::int16_t max;
    };

    const std::vector<PeakPair>& level(int idx) const noexcept
    {
        return levels_[idx];
    }

    bool isValid() const noexcept { return !levels_.empty(); }

private:
    static std::uint64_t hashPath(const std::string& path);

    // Формат файла. Границы нужны, чтобы битый кэш не превратилcя
    // в попытку выделить неcколько гигабайт.
    static constexpr std::uint32_t kMagic = 0x5045414B;   // 'PEAK'
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kMaxLevels = 64;
    static constexpr std::uint32_t kMaxPairsPerLevel = 1u << 28;

    std::vector<std::vector<PeakPair>> levels_;
    int level0Frames_ = 256;
};

} // namespace daw::graph
