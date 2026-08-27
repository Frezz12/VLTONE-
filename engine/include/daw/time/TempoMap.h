#pragma once
//
// Музыкальное время: карта темпа, размеры, конверсии сэмплы ↔ тики ↔ такты.
// Реализация ARCHITECTURE.md §6.
//
// Договорённости, от которых зависит всё остальное:
//
//   * Tick — целочисленное музыкальное время, 15360 тиков на четверть.
//     Делится на 2,3,4,5,6,8,12,16,32,64 — то есть на триоли, квинтоли и
//     128-е без остатка. Стандартных MIDI-шных 960 для этого не хватает.
//
//   * BPM всегда означает ЧЕТВЕРТИ в минуту, независимо от размера.
//     В 6/8 при 120 BPM доля (восьмая) идёт вдвое чаще. Так считают Logic,
//     Cubase и Ableton; иначе смена размера меняла бы реальную скорость.
//
//   * Темп между точками либо постоянен, либо линейно нарастает. Позиция
//     на рампе считается аналитически (интеграл от 1/bpm), а не численно —
//     иначе накапливается ошибка и материал уезжает к концу проекта.
//
//   * Все конверсии RT-safe: бинарный поиск по сегментам плюс арифметика,
//     без аллокаций.
//
#include <cstdint>
#include <vector>

namespace daw::time {

using Tick        = std::int64_t;
using SampleCount = std::int64_t;

inline constexpr Tick kTicksPerQuarter = 15360;
inline constexpr Tick kMaxTick         = (std::int64_t{1} << 60);

// Точка карты темпа. rampToNext означает линейный переход к следующей точке.
struct TempoPoint {
    Tick   tick       = 0;
    double bpm        = 120.0;
    bool   rampToNext = false;
};

// Смена размера происходит на границе такта, поэтому позиция задаётся тактом.
struct TimeSignaturePoint {
    int bar         = 1;   // с единицы, как показывает линейка
    int numerator   = 4;
    int denominator = 4;
};

struct BarBeat {
    int  bar        = 1;   // с единицы
    int  beat       = 1;   // с единицы
    Tick tickInBeat = 0;
};

enum class GridResolution {
    Bar,
    Half,
    Quarter,
    Eighth,
    Sixteenth,
    ThirtySecond,
    QuarterTriplet,
    EighthTriplet,
    SixteenthTriplet,
};

class TempoMap {
public:
    TempoMap();
    explicit TempoMap(double sampleRate);

    // ---- редактирование (UI-поток) ----------------------------------------
    // Карта не меняется на месте: UI правит копию и публикует её через
    // RcuPublisher. Поэтому сеттеры не обязаны быть потокобезопасными.

    void setSampleRate(double sampleRate);
    void setConstantTempo(double bpm);
    void setTempoPoints(std::vector<TempoPoint> points);
    void setTimeSignature(int numerator, int denominator);
    void setTimeSignaturePoints(std::vector<TimeSignaturePoint> points);

    double sampleRate() const noexcept { return sampleRate_; }
    const std::vector<TempoPoint>&         tempoPoints()         const noexcept { return tempoPoints_; }
    const std::vector<TimeSignaturePoint>& timeSignaturePoints() const noexcept { return sigPoints_; }

    // ---- конверсии (RT-safe) ----------------------------------------------

    double      bpmAtTick(Tick tick) const noexcept;
    double      tickToSeconds(Tick tick) const noexcept;
    Tick        secondsToTick(double seconds) const noexcept;
    SampleCount tickToSample(Tick tick) const noexcept;
    Tick        sampleToTick(SampleCount sample) const noexcept;

    // ---- такты и доли (RT-safe) -------------------------------------------

    BarBeat tickToBarBeat(Tick tick) const noexcept;
    Tick    barBeatToTick(int bar, int beat = 1, Tick tickInBeat = 0) const noexcept;

    Tick ticksPerBeatAt(Tick tick) const noexcept;
    Tick ticksPerBarAt(Tick tick) const noexcept;

    // Первая доля с позицией >= tick. Нужна метроному, чтобы найти клики,
    // попадающие в текущий блок обработки.
    Tick nextBeatAtOrAfter(Tick tick) const noexcept;

    // Приходится ли тик ровно на первую долю такта.
    bool isDownbeat(Tick tick) const noexcept;

    // ---- сетка ------------------------------------------------------------

    Tick gridTicks(GridResolution resolution, Tick at) const noexcept;
    Tick snap(Tick tick, GridResolution resolution) const noexcept;

private:
    void rebuild();

    struct TempoSegment {
        Tick   startTick    = 0;
        Tick   endTick      = kMaxTick;
        double bpmStart     = 120.0;
        double bpmEnd       = 120.0;
        double startSeconds = 0.0;
        bool   isRamp       = false;
    };

    struct SigSegment {
        int  startBar     = 1;
        int  endBar       = 0;        // не включая; 0 означает «до конца»
        int  numerator    = 4;
        int  denominator  = 4;
        Tick startTick    = 0;
        Tick ticksPerBeat = kTicksPerQuarter;
        Tick ticksPerBar  = kTicksPerQuarter * 4;
    };

    const TempoSegment& tempoSegmentAtTick(Tick tick) const noexcept;
    const TempoSegment& tempoSegmentAtSeconds(double seconds) const noexcept;
    const SigSegment&   sigSegmentAtTick(Tick tick) const noexcept;
    const SigSegment&   sigSegmentAtBar(int bar) const noexcept;

    std::vector<TempoPoint>         tempoPoints_;
    std::vector<TimeSignaturePoint> sigPoints_;
    std::vector<TempoSegment>       tempoSegments_;
    std::vector<SigSegment>         sigSegments_;
    double                          sampleRate_ = 48000.0;
};

} // namespace daw::time
