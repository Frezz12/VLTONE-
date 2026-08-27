#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "daw/rt/RtGuard.h"
#include "daw/time/TempoMap.h"

using namespace daw::time;
using Catch::Approx;

namespace {
// Один такт 4/4 = 4 четверти.
constexpr Tick kBar44 = kTicksPerQuarter * 4;
}

TEST_CASE("TempoMap: defaults to 120 bpm 4/4", "[time]") {
    TempoMap map(48000.0);

    REQUIRE(map.bpmAtTick(0) == Approx(120.0));
    REQUIRE(map.ticksPerBeatAt(0) == kTicksPerQuarter);
    REQUIRE(map.ticksPerBarAt(0) == kBar44);
    REQUIRE(map.tickToSample(0) == 0);
}

TEST_CASE("TempoMap: bar length at 120 bpm is exactly two seconds", "[time]") {
    TempoMap map(48000.0);

    // 4 четверти при 120 BPM = 2 c = 96000 сэмплов на 48 кГц.
    REQUIRE(map.tickToSeconds(kBar44) == Approx(2.0));
    REQUIRE(map.tickToSample(kBar44) == 96000);

    // Начало пятого такта — ровно 8 секунд.
    REQUIRE(map.tickToSample(map.barBeatToTick(5, 1)) == 384000);
}

TEST_CASE("TempoMap: tempo scales duration inversely", "[time]") {
    TempoMap map(48000.0);

    map.setConstantTempo(60.0);
    REQUIRE(map.tickToSeconds(kBar44) == Approx(4.0));

    map.setConstantTempo(240.0);
    REQUIRE(map.tickToSeconds(kBar44) == Approx(1.0));
}

TEST_CASE("TempoMap: sample and tick round-trip", "[time]") {
    TempoMap map(48000.0);
    map.setConstantTempo(120.0);

    // При 120 BPM на 48 кГц тик короче сэмпла, поэтому обратная конверсия
    // обязана попадать точно.
    for (Tick t : {Tick{0}, Tick{1}, kTicksPerQuarter, kBar44, kBar44 * 37 + 991}) {
        const SampleCount s = map.tickToSample(t);
        REQUIRE(map.sampleToTick(s) == t);
    }
}

TEST_CASE("TempoMap: conversion is monotonic", "[time]") {
    TempoMap map(48000.0);
    map.setTempoPoints({
        TempoPoint{0,           90.0,  true},
        TempoPoint{kBar44 * 4,  150.0, false},
        TempoPoint{kBar44 * 8,  70.0,  false},
    });

    SampleCount previous = -1;
    for (Tick t = 0; t < kBar44 * 12; t += kTicksPerQuarter / 8) {
        const SampleCount s = map.tickToSample(t);
        REQUIRE(s > previous);
        previous = s;
    }
}

TEST_CASE("TempoMap: ramp matches numerical integration", "[time]") {
    // Самый важный тест модуля. Аналитическая формула проверяется НЕЗАВИСИМЫМ
    // способом — численным интегрированием по определению. Если бы обе стороны
    // считались одной формулой, тест не поймал бы ничего.
    constexpr Tick   kStart = 0;
    constexpr Tick   kEnd   = kBar44 * 4;
    constexpr double kBpm0  = 60.0;
    constexpr double kBpm1  = 180.0;

    TempoMap map(48000.0);
    map.setTempoPoints({
        TempoPoint{kStart, kBpm0, true},
        TempoPoint{kEnd,   kBpm1, false},
    });

    // seconds = ∫ 60/(TPQ·bpm(u)) du, метод средней точки.
    constexpr int kSteps = 200000;
    const double  step   = static_cast<double>(kEnd - kStart) / kSteps;

    double numeric = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double u   = kStart + step * (i + 0.5);
        const double bpm = kBpm0 + (kBpm1 - kBpm0) * (u - kStart) / (kEnd - kStart);
        numeric += step * 60.0 / (static_cast<double>(kTicksPerQuarter) * bpm);
    }

    REQUIRE(map.tickToSeconds(kEnd) == Approx(numeric).epsilon(1e-6));

    // И в середине рампы тоже, а не только на границе.
    const Tick mid = kEnd / 2;
    double numericMid = 0.0;
    const double stepMid = static_cast<double>(mid - kStart) / kSteps;
    for (int i = 0; i < kSteps; ++i) {
        const double u   = kStart + stepMid * (i + 0.5);
        const double bpm = kBpm0 + (kBpm1 - kBpm0) * (u - kStart) / (kEnd - kStart);
        numericMid += stepMid * 60.0 / (static_cast<double>(kTicksPerQuarter) * bpm);
    }
    REQUIRE(map.tickToSeconds(mid) == Approx(numericMid).epsilon(1e-6));
}

TEST_CASE("TempoMap: ramp inverts correctly", "[time]") {
    TempoMap map(48000.0);
    map.setTempoPoints({
        TempoPoint{0,          60.0,  true},
        TempoPoint{kBar44 * 4, 180.0, false},
    });

    for (Tick t = 0; t <= kBar44 * 4; t += kTicksPerQuarter) {
        const double seconds = map.tickToSeconds(t);
        // Допуск в один тик: конверсия проходит через double.
        REQUIRE(std::abs(map.secondsToTick(seconds) - t) <= 1);
    }
}

TEST_CASE("TempoMap: bar and beat mapping in 4/4", "[time]") {
    TempoMap map(48000.0);

    BarBeat bb = map.tickToBarBeat(0);
    REQUIRE(bb.bar == 1);
    REQUIRE(bb.beat == 1);
    REQUIRE(bb.tickInBeat == 0);

    bb = map.tickToBarBeat(kTicksPerQuarter * 2);
    REQUIRE(bb.bar == 1);
    REQUIRE(bb.beat == 3);

    bb = map.tickToBarBeat(kBar44 * 3 + kTicksPerQuarter + 77);
    REQUIRE(bb.bar == 4);
    REQUIRE(bb.beat == 2);
    REQUIRE(bb.tickInBeat == 77);

    REQUIRE(map.barBeatToTick(4, 2, 77) == kBar44 * 3 + kTicksPerQuarter + 77);
}

TEST_CASE("TempoMap: beat length follows the denominator", "[time]") {
    TempoMap map(48000.0);

    map.setTimeSignature(3, 4);
    REQUIRE(map.ticksPerBeatAt(0) == kTicksPerQuarter);
    REQUIRE(map.ticksPerBarAt(0) == kTicksPerQuarter * 3);

    // В 6/8 доля — восьмая, поэтому такт короче трёх четвертей ровно на ноль:
    // 6 восьмых = 3 четверти. BPM по-прежнему считается по четвертям.
    map.setTimeSignature(6, 8);
    REQUIRE(map.ticksPerBeatAt(0) == kTicksPerQuarter / 2);
    REQUIRE(map.ticksPerBarAt(0) == kTicksPerQuarter * 3);
    REQUIRE(map.tickToSeconds(map.ticksPerBarAt(0)) == Approx(1.5));
}

TEST_CASE("TempoMap: time signature change realigns bars", "[time]") {
    TempoMap map(48000.0);
    map.setTimeSignaturePoints({
        TimeSignaturePoint{1, 4, 4},
        TimeSignaturePoint{5, 3, 4},
    });

    // Такты 1..4 по 4/4, дальше по 3/4.
    const Tick bar5 = map.barBeatToTick(5, 1);
    REQUIRE(bar5 == kBar44 * 4);

    BarBeat bb = map.tickToBarBeat(bar5);
    REQUIRE(bb.bar == 5);
    REQUIRE(bb.beat == 1);

    // Шестой такт наступает через три четверти, а не через четыре.
    REQUIRE(map.barBeatToTick(6, 1) == bar5 + kTicksPerQuarter * 3);

    bb = map.tickToBarBeat(bar5 + kTicksPerQuarter * 3);
    REQUIRE(bb.bar == 6);
    REQUIRE(bb.beat == 1);
}

TEST_CASE("TempoMap: nextBeatAtOrAfter finds beat boundaries", "[time]") {
    TempoMap map(48000.0);

    REQUIRE(map.nextBeatAtOrAfter(0) == 0);
    REQUIRE(map.nextBeatAtOrAfter(1) == kTicksPerQuarter);
    REQUIRE(map.nextBeatAtOrAfter(kTicksPerQuarter) == kTicksPerQuarter);
    REQUIRE(map.nextBeatAtOrAfter(kTicksPerQuarter + 1) == kTicksPerQuarter * 2);
}

TEST_CASE("TempoMap: downbeats land on bar starts", "[time]") {
    TempoMap map(48000.0);

    REQUIRE(map.isDownbeat(0));
    REQUIRE(map.isDownbeat(kBar44));
    REQUIRE(map.isDownbeat(kBar44 * 7));
    REQUIRE_FALSE(map.isDownbeat(kTicksPerQuarter));
    REQUIRE_FALSE(map.isDownbeat(kBar44 + 1));
}

TEST_CASE("TempoMap: snapping to grid", "[time]") {
    TempoMap map(48000.0);

    REQUIRE(map.snap(100, GridResolution::Quarter) == 0);
    REQUIRE(map.snap(kTicksPerQuarter - 100, GridResolution::Quarter) == kTicksPerQuarter);
    REQUIRE(map.snap(kTicksPerQuarter + 100, GridResolution::Quarter) == kTicksPerQuarter);
    REQUIRE(map.snap(kBar44 - 100, GridResolution::Bar) == kBar44);

    // Триоли обязаны делиться нацело — ради этого и выбрано 15360 тиков.
    REQUIRE(kTicksPerQuarter % 3 == 0);
    REQUIRE(map.gridTicks(GridResolution::EighthTriplet, 0) * 3 == kTicksPerQuarter);
    REQUIRE(map.gridTicks(GridResolution::SixteenthTriplet, 0) * 6 == kTicksPerQuarter);
}

TEST_CASE("TempoMap: rejects broken input instead of misbehaving", "[time]") {
    TempoMap map(48000.0);

    // Карта без точки в нуле должна дополниться сама, иначе позиция до первой
    // точки не определена.
    map.setTempoPoints({TempoPoint{kBar44 * 4, 140.0, false}});
    REQUIRE(map.tempoPoints().front().tick == 0);
    REQUIRE(map.tickToSample(0) == 0);

    // Нулевой и отрицательный темп не должны приводить к делению на ноль.
    map.setConstantTempo(0.0);
    REQUIRE(map.bpmAtTick(0) > 0.0);
    REQUIRE(std::isfinite(map.tickToSeconds(kBar44)));

    map.setConstantTempo(-5.0);
    REQUIRE(map.bpmAtTick(0) > 0.0);

    // Отрицательные позиции прижимаются к нулю, а не уезжают в мусор.
    REQUIRE(map.tickToSample(-1000) == 0);
    REQUIRE(map.sampleToTick(-1000) == 0);
}

TEST_CASE("TempoMap: conversions allocate nothing", "[time][rt]") {
    // Прямая проверка принципа №2: карта темпа читается из аудио-потока,
    // поэтому конверсии обязаны обходиться без единой аллокации.
    if (!daw::rt::checksEnabled())
        return;

    TempoMap map(48000.0);
    map.setTempoPoints({
        TempoPoint{0,          90.0,  true},
        TempoPoint{kBar44 * 4, 150.0, false},
    });
    map.setTimeSignaturePoints({
        TimeSignaturePoint{1, 4, 4},
        TimeSignaturePoint{5, 7, 8},
    });

    daw::rt::resetViolations();
    {
        daw::rt::ScopedAudioThread audioThread;
        volatile SampleCount sink = 0;
        for (Tick t = 0; t < kBar44 * 8; t += 1013) {
            sink += map.tickToSample(t);
            sink += map.sampleToTick(t);
            sink += map.nextBeatAtOrAfter(t);
            sink += map.tickToBarBeat(t).bar;
            sink += map.snap(t, GridResolution::Sixteenth);
            sink += static_cast<SampleCount>(map.isDownbeat(t));
        }
    }

    const auto v = daw::rt::violations();
    INFO("последнее нарушение: " << (v.last ? v.last : "нет"));
    REQUIRE(v.count == 0);
}
