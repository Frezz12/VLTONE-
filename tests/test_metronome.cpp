#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "daw/audio/AudioBuffer.h"
#include "daw/audio/Metronome.h"
#include "daw/rt/RtGuard.h"
#include "daw/time/TempoMap.h"

using daw::audio::AudioBuffer;
using daw::audio::Metronome;
using daw::time::SampleCount;
using daw::time::TempoMap;
using Catch::Approx;

namespace {

constexpr double kSampleRate = 48000.0;

// Прогоняет метроном блоками заданного размера и склеивает результат.
std::vector<float> render(const TempoMap& map, int totalFrames, int blockSize) {
    Metronome metronome;
    metronome.prepare(kSampleRate);
    metronome.setEnabled(true);
    metronome.setGain(1.0f);

    AudioBuffer buffer(1, blockSize);
    std::vector<float> output;
    output.reserve(static_cast<std::size_t>(totalFrames));

    SampleCount position = 0;
    while (static_cast<int>(output.size()) < totalFrames) {
        const int frames = std::min(blockSize, totalFrames - static_cast<int>(output.size()));

        auto view = buffer.view(frames);
        view.clear();
        metronome.process(view, frames, map, position);

        const float* p = view.channel(0);
        output.insert(output.end(), p, p + frames);
        position += frames;
    }
    return output;
}

// Позиции начал кликов.
std::vector<int> onsets(const std::vector<float>& signal, float threshold = 0.01f) {
    std::vector<int> result;
    bool inClick = false;
    int  silence = 0;

    for (std::size_t i = 0; i < signal.size(); ++i) {
        if (std::abs(signal[i]) > threshold) {
            if (!inClick) {
                result.push_back(static_cast<int>(i));
                inClick = true;
            }
            silence = 0;
        } else if (inClick && ++silence > 200) {
            inClick = false;
        }
    }
    return result;
}

float peakBetween(const std::vector<float>& signal, int from, int to) {
    float m = 0.0f;
    for (int i = from; i < to && i < static_cast<int>(signal.size()); ++i)
        m = std::max(m, std::abs(signal[i]));
    return m;
}

} // namespace

TEST_CASE("Metronome: silent when disabled", "[audio][metronome]") {
    TempoMap map(kSampleRate);

    Metronome metronome;
    metronome.prepare(kSampleRate);
    metronome.setEnabled(false);

    AudioBuffer buffer(1, 512);
    auto view = buffer.view();
    view.clear();
    metronome.process(view, 512, map, 0);

    REQUIRE(view.peak(0) == Approx(0.0f).margin(1e-9));
}

TEST_CASE("Metronome: clicks land on beats at 120 bpm", "[audio][metronome]") {
    TempoMap map(kSampleRate);
    map.setConstantTempo(120.0);

    // Доля при 120 BPM = 0.5 c = 24000 сэмплов. Четыре такта.
    const auto signal = render(map, 24000 * 8, 512);
    const auto found  = onsets(signal);

    REQUIRE(found.size() == 8);
    for (std::size_t i = 0; i < found.size(); ++i) {
        const int expected = static_cast<int>(i) * 24000;
        // Допуск в два сэмпла: первый сэмпл синуса равен нулю, поэтому порог
        // срабатывает на следующем.
        INFO("клик " << i << " ожидался на " << expected << ", найден на " << found[i]);
        REQUIRE(std::abs(found[i] - expected) <= 2);
    }
}

TEST_CASE("Metronome: onsets do not depend on block size", "[audio][metronome]") {
    // Главный тест сэмпл-точности. Если клик привязан к началу блока, а не к
    // точному сэмплу, позиции при разных размерах буфера разойдутся: доля на
    // 24000 при буфере 512 попадает внутрь блока со смещением 448, а при
    // буфере 1024 — со смещением 448 от другой границы.
    TempoMap map(kSampleRate);
    map.setConstantTempo(120.0);

    const auto reference = onsets(render(map, 24000 * 8, 64));

    REQUIRE_FALSE(reference.empty());

    for (int blockSize : {128, 256, 512, 1024, 2048, 480}) {
        const auto found = onsets(render(map, 24000 * 8, blockSize));
        INFO("размер блока: " << blockSize);
        REQUIRE(found == reference);
    }
}

TEST_CASE("Metronome: accents the downbeat", "[audio][metronome]") {
    TempoMap map(kSampleRate);
    map.setConstantTempo(120.0);

    const auto signal = render(map, 24000 * 5, 512);

    // Первая доля такта должна быть заметно громче остальных, иначе на слух
    // не отличить, где начинается такт.
    const float downbeat = peakBetween(signal, 0, 2000);
    const float beat2    = peakBetween(signal, 24000, 26000);
    const float bar2     = peakBetween(signal, 96000, 98000);

    REQUIRE(downbeat > beat2 * 1.3f);
    REQUIRE(bar2 > beat2 * 1.3f);
    REQUIRE(bar2 == Approx(downbeat).epsilon(0.05));
}

TEST_CASE("Metronome: no drift at fractional beat length", "[audio][metronome]") {
    // При 140 BPM доля равна 20571.43 сэмпла — не целому числу. Если позиция
    // клика вычисляется накоплением, ошибка будет расти; если каждый раз
    // считается от карты темпа, интервалы останутся вокруг точного значения.
    TempoMap map(kSampleRate);
    map.setConstantTempo(140.0);

    const int    totalFrames = 48000 * 60;   // минута
    const double exact       = 60.0 / 140.0 * kSampleRate;

    const auto found = onsets(render(map, totalFrames, 512));
    REQUIRE(found.size() > 130);

    for (std::size_t i = 1; i < found.size(); ++i) {
        const double delta = found[i] - found[i - 1];
        INFO("интервал " << i << ": " << delta << " против " << exact);
        REQUIRE(std::abs(delta - exact) <= 1.0);
    }

    // И абсолютная позиция последнего клика тоже не должна уехать.
    const int    last     = static_cast<int>(found.size()) - 1;
    const double expected = last * exact;
    REQUIRE(std::abs(found[static_cast<std::size_t>(last)] - expected) <= 2.0);
}

TEST_CASE("Metronome: follows a tempo ramp", "[audio][metronome]") {
    // На ускорении интервалы между кликами обязаны сокращаться.
    TempoMap map(kSampleRate);
    map.setTempoPoints({
        daw::time::TempoPoint{0, 60.0, true},
        daw::time::TempoPoint{daw::time::kTicksPerQuarter * 32, 180.0, false},
    });

    const auto found = onsets(render(map, 48000 * 20, 512));
    REQUIRE(found.size() > 8);

    for (std::size_t i = 2; i < found.size(); ++i) {
        const int previous = found[i - 1] - found[i - 2];
        const int current  = found[i]     - found[i - 1];
        INFO("интервалы: " << previous << " → " << current);
        REQUIRE(current <= previous);
    }
}

TEST_CASE("Metronome: keeps clicking after a tempo change mid-playback", "[audio][metronome]") {
    // Регрессия. Защита от двойного клика построена на том, что тик растёт.
    // Но смена темпа переразмечает время: на 30-й секунде при 120 BPM это тик
    // 460800, а после перехода на 60 BPM тот же сэмпл — уже 230400. Раньше
    // метроном замолкал на всё время, пока позиция не догонит старый тик.
    TempoMap fast(kSampleRate);
    fast.setConstantTempo(120.0);

    TempoMap slow(kSampleRate);
    slow.setConstantTempo(60.0);

    Metronome metronome;
    metronome.prepare(kSampleRate);
    metronome.setEnabled(true);
    metronome.setGain(1.0f);

    constexpr int kBlock = 512;
    AudioBuffer buffer(1, kBlock);

    auto run = [&](const TempoMap& map, int frames, std::vector<float>& into) {
        SampleCount position = static_cast<SampleCount>(into.size());
        int produced = 0;
        while (produced < frames) {
            const int n = std::min(kBlock, frames - produced);
            auto view = buffer.view(n);
            view.clear();
            metronome.process(view, n, map, position);
            const float* p = view.channel(0);
            into.insert(into.end(), p, p + n);
            position += n;
            produced += n;
        }
    };

    std::vector<float> signal;
    run(fast, 48000 * 10, signal);        // 10 c на 120 BPM
    const int switchPoint = static_cast<int>(signal.size());
    run(slow, 48000 * 10, signal);        // и ещё 10 c на 60 BPM

    const auto found = onsets(signal);

    int before = 0, after = 0;
    for (int index : found)
        (index < switchPoint ? before : after) += 1;

    INFO("кликов до смены темпа: " << before << ", после: " << after);
    REQUIRE(before >= 18);   // ~20 долей за 10 c на 120 BPM
    REQUIRE(after  >= 8);    // ~10 долей за 10 c на 60 BPM — главное, не ноль
}

TEST_CASE("Metronome: allocates nothing while processing", "[audio][metronome][rt]") {
    if (!daw::rt::checksEnabled())
        return;

    TempoMap map(kSampleRate);
    map.setConstantTempo(174.0);

    Metronome metronome;
    metronome.prepare(kSampleRate);
    metronome.setEnabled(true);

    AudioBuffer buffer(2, 512);

    daw::rt::resetViolations();
    {
        daw::rt::ScopedAudioThread audioThread;
        SampleCount position = 0;
        for (int block = 0; block < 2000; ++block) {
            auto view = buffer.view(512);
            view.clear();
            metronome.process(view, 512, map, position);
            position += 512;
        }
    }

    const auto v = daw::rt::violations();
    INFO("последнее нарушение: " << (v.last ? v.last : "нет"));
    REQUIRE(v.count == 0);
}
