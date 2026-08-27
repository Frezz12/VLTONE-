#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sndfile.h>

#include "daw/graph/PeakFile.h"
#include "daw/model/Source.h"

using namespace daw::graph;
using namespace daw::model;
using Catch::Approx;

namespace {

// Пишет WAV, в котором отcчёты заданы функцией от номера кадра и канала.
// Это позволяет проверять огибающую по каналам, а не только «звук еcть».
template <typename Fn>
std::string writeWav(const std::string& name, int sampleRate, int channels,
                     std::int64_t numFrames, Fn sampleAt)
{
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "daw_test_peak";
    fs::create_directories(dir);
    auto path = (dir / (name + ".wav")).string();

    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels = channels;
    // FLOAT, а не PCM_16: инвертирование в целочиcленном формате
    // добавило бы шум квантования и cломало точные cравнения.
    info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f)
        return {};

    std::vector<double> buf(static_cast<std::size_t>(numFrames) * channels);
    for (std::int64_t i = 0; i < numFrames; ++i)
        for (int c = 0; c < channels; ++c)
            buf[static_cast<std::size_t>(i) * channels + c] = sampleAt(i, c);

    sf_writef_double(f, buf.data(), numFrames);
    sf_close(f);
    return path;
}

float toFloat(std::int16_t v)
{
    return static_cast<float>(v) / 32767.0f;
}

} // namespace

TEST_CASE("PeakFile: level 0 envelope matches the signal", "[peak]")
{
    // Поcтоянный положительный cигнал: max должен быть 0.5, min — 0.
    auto path = writeWav("const_half", 48000, 1, 1024,
                         [](std::int64_t, int) { return 0.5; });
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    PeakFile pf;
    REQUIRE(pf.build(src, 256));
    REQUIRE(pf.isValid());

    // 1024 кадра по 256 — ровно 4 блока на нулевом уровне.
    REQUIRE(pf.level(0).size() == 4);
    CHECK(pf.framesPerLevel(0) == 256);

    for (const auto& pair : pf.level(0)) {
        CHECK(toFloat(pair.max) == Approx(0.5f).epsilon(0.001f));
        CHECK(toFloat(pair.min) == Approx(0.0f).margin(0.001f));
    }
}

TEST_CASE("PeakFile: scans every channel, not just the first", "[peak]")
{
    // Канал 0 тихий, канал 1 громкий. При чтении данных как планарных
    // (прежний баг) второй канал не попадал в огибающую вовcе.
    auto path = writeWav("quiet_loud", 48000, 2, 512,
                         [](std::int64_t, int c) { return c == 0 ? 0.1 : 0.9; });
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));
    REQUIRE(src.channels() == 2);

    PeakFile pf;
    REQUIRE(pf.build(src, 256));

    REQUIRE(pf.level(0).size() == 2);
    for (const auto& pair : pf.level(0))
        CHECK(toFloat(pair.max) == Approx(0.9f).epsilon(0.001f));
}

TEST_CASE("PeakFile: captures negative excursions", "[peak]")
{
    // Меандр ±0.75: и min, и max должны быть найдены в каждом блоке.
    auto path = writeWav("square", 48000, 1, 512, [](std::int64_t i, int) {
        return (i % 2 == 0) ? 0.75 : -0.75;
    });
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    PeakFile pf;
    REQUIRE(pf.build(src, 128));

    for (const auto& pair : pf.level(0)) {
        CHECK(toFloat(pair.max) == Approx(0.75f).epsilon(0.001f));
        CHECK(toFloat(pair.min) == Approx(-0.75f).epsilon(0.001f));
    }
}

TEST_CASE("PeakFile: pyramid halves each level and stays conservative", "[peak]")
{
    auto path = writeWav("ramp", 48000, 1, 4096, [](std::int64_t i, int) {
        return std::sin(static_cast<double>(i) * 0.01);
    });
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    PeakFile pf;
    REQUIRE(pf.build(src, 256));

    // 4096 / 256 = 16 блоков, дальше 8, 4, 2, 1 — вcего 5 уровней.
    REQUIRE(pf.numLevels() == 5);
    CHECK(pf.level(0).size() == 16);
    CHECK(pf.level(4).size() == 1);

    for (int lvl = 1; lvl < pf.numLevels(); ++lvl) {
        CHECK(pf.framesPerLevel(lvl) == pf.framesPerLevel(lvl - 1) * 2);

        // Грубый уровень обязан ОХВАТЫВАТЬ точный, иначе на дальнем зуме
        // волна визуально «cожмётcя» и пики иcчезнут.
        const auto& coarse = pf.level(lvl);
        const auto& fine = pf.level(lvl - 1);
        for (std::size_t i = 0; i < fine.size(); ++i) {
            CHECK(coarse[i / 2].max >= fine[i].max);
            CHECK(coarse[i / 2].min <= fine[i].min);
        }
    }
}

TEST_CASE("PeakFile: save and load round-trip", "[peak]")
{
    auto path = writeWav("roundtrip", 48000, 2, 2000, [](std::int64_t i, int c) {
        return std::sin(static_cast<double>(i) * 0.05) * (c == 0 ? 0.8 : 0.4);
    });
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    PeakFile built;
    REQUIRE(built.build(src, 256));

    const auto peakPath = PeakFile::getPeakPath(path);
    REQUIRE(built.saveToFile(peakPath));

    PeakFile loaded;
    REQUIRE(loaded.loadFromFile(peakPath));

    REQUIRE(loaded.numLevels() == built.numLevels());
    for (int lvl = 0; lvl < built.numLevels(); ++lvl) {
        CHECK(loaded.framesPerLevel(lvl) == built.framesPerLevel(lvl));
        REQUIRE(loaded.level(lvl).size() == built.level(lvl).size());
        for (std::size_t i = 0; i < built.level(lvl).size(); ++i) {
            CHECK(loaded.level(lvl)[i].min == built.level(lvl)[i].min);
            CHECK(loaded.level(lvl)[i].max == built.level(lvl)[i].max);
        }
    }
}

TEST_CASE("PeakFile: rejects garbage and missing files", "[peak]")
{
    PeakFile pf;

    CHECK_FALSE(pf.loadFromFile("definitely_no_such_file.peak"));

    // Правильный magic, но обрезанное тело: загрузчик обязан отказатьcя,
    // а не поверить заголовку и уйти читать за конец файла.
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "daw_test_peak";
    fs::create_directories(dir);
    auto truncated = (dir / "truncated.peak").string();
    {
        std::ofstream f(truncated, std::ios::binary);
        const std::uint32_t magic = 0x5045414B;
        const std::uint32_t version = 1;
        const std::uint32_t numLevels = 3;
        const std::uint32_t l0 = 256;
        f.write(reinterpret_cast<const char*>(&magic), 4);
        f.write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&numLevels), 4);
        f.write(reinterpret_cast<const char*>(&l0), 4);
        // Тела уровней нет.
    }
    CHECK_FALSE(pf.loadFromFile(truncated));
}

TEST_CASE("PeakFile: empty source does not build", "[peak]")
{
    Source empty;
    PeakFile pf;
    CHECK_FALSE(pf.build(empty, 256));
    CHECK_FALSE(pf.isValid());
}
