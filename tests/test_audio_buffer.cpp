#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "daw/audio/AudioBuffer.h"
#include "daw/audio/ToneSource.h"

using daw::audio::AudioBuffer;
using daw::audio::ToneSource;
using Catch::Approx;

TEST_CASE("AudioBuffer: allocates and zeroes", "[audio][buffer]") {
    AudioBuffer buf(2, 512);

    REQUIRE(buf.numChannels() == 2);
    REQUIRE(buf.numFrames() == 512);

    auto view = buf.view();
    for (int c = 0; c < view.numChannels(); ++c)
        for (int i = 0; i < view.numFrames(); ++i)
            REQUIRE(view.channel(c)[i] == 0.0f);
}

TEST_CASE("AudioBuffer: channels do not overlap", "[audio][buffer]") {
    AudioBuffer buf(2, 64);
    auto view = buf.view();

    view.channel(0)[10] = 1.0f;
    REQUIRE(view.channel(1)[10] == 0.0f);

    view.channel(1)[20] = -1.0f;
    REQUIRE(view.channel(0)[20] == 0.0f);
}

TEST_CASE("AudioBuffer: partial block view", "[audio][buffer]") {
    // Размер буфера выделен под максимум, но драйвер может отдать меньше.
    AudioBuffer buf(2, 1024);
    auto view = buf.view(128);

    REQUIRE(view.numFrames() == 128);
    REQUIRE(view.numChannels() == 2);
}

TEST_CASE("AudioBuffer: peak uses absolute value", "[audio][buffer]") {
    AudioBuffer buf(1, 16);
    auto view = buf.view();

    view.channel(0)[3] = -0.75f;
    view.channel(0)[9] =  0.5f;

    REQUIRE(view.peak(0) == Approx(0.75f));
}

TEST_CASE("ToneSource: silent until activated", "[audio][tone]") {
    ToneSource tone;
    tone.prepare(48000.0, 512);
    tone.setGain(1.0f);

    AudioBuffer buf(2, 512);
    auto view = buf.view();
    tone.process(view, 512);

    REQUIRE(view.peak(0) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("ToneSource: generates requested frequency", "[audio][tone]") {
    constexpr double kSampleRate = 48000.0;
    constexpr int    kFrames     = 4800;   // 0.1 c

    ToneSource tone;
    // Частоту задаём ДО prepare(): она тоже сглаживается, и если менять её
    // после, генератор поедет от прежнего значения к новому и за короткое окно
    // не наберёт нужного числа периодов. prepare() фиксирует стартовое значение.
    tone.setFrequency(1000.0f);
    tone.setGain(1.0f);
    tone.prepare(kSampleRate, kFrames);
    tone.setActive(true);

    AudioBuffer buf(1, kFrames);
    auto view = buf.view();
    tone.process(view, kFrames);

    // Считаем переходы через ноль. За 0.1 c синус 1000 Гц даёт 100 периодов,
    // то есть около 200 переходов. Допуск на сглаживание громкости в начале.
    int crossings = 0;
    const float* p = view.channel(0);
    for (int i = 1; i < kFrames; ++i)
        if ((p[i - 1] < 0.0f) != (p[i] < 0.0f))
            ++crossings;

    REQUIRE(crossings >= 195);
    REQUIRE(crossings <= 205);
}

TEST_CASE("ToneSource: gain ramps smoothly", "[audio][tone]") {
    // Скачок громкости даёт щелчок. Первые сэмплы обязаны быть тише целевых.
    ToneSource tone;
    tone.prepare(48000.0, 512);
    tone.setFrequency(1000.0f);
    tone.setGain(1.0f);
    tone.setActive(true);

    AudioBuffer buf(1, 512);
    auto view = buf.view();
    tone.process(view, 512);

    const float* p = view.channel(0);
    float firstMs = 0.0f;    // пик за первые 48 сэмплов (1 мс)
    for (int i = 0; i < 48; ++i)
        firstMs = std::max(firstMs, std::abs(p[i]));

    REQUIRE(firstMs < 0.2f);          // сглаживание работает
    REQUIRE(view.peak(0) > 0.3f);     // но звук всё-таки появился
}
