#include "daw/time/TempoMap.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace daw::time {
namespace {

constexpr double kMinBpm = 1.0;
constexpr double kMaxBpm = 999.0;

double clampBpm(double bpm) {
    if (!(bpm > 0.0)) return 120.0;          // ловит и NaN
    return std::clamp(bpm, kMinBpm, kMaxBpm);
}

// Длительность в секундах участка [t0, t1) при постоянном темпе.
double constantDuration(Tick t0, Tick t1, double bpm) {
    return static_cast<double>(t1 - t0) * 60.0
         / (bpm * static_cast<double>(kTicksPerQuarter));
}

// То же для линейной рампы. bpm(u) = b0 + k·(u − t0), k = (b1 − b0)/(t1 − t0).
//
//   T = ∫ 60/(TPQ·bpm(u)) du = (60/(TPQ·k))·ln(b1/b0)
//
// Численное интегрирование здесь дало бы накапливающуюся ошибку: на проекте
// с десятком ускорений расхождение к концу измерялось бы в сэмплах.
double rampDuration(Tick t0, Tick t1, double b0, double b1) {
    const double span = static_cast<double>(t1 - t0);
    const double k    = (b1 - b0) / span;
    if (std::abs(k) < 1e-12)
        return constantDuration(t0, t1, b0);
    return 60.0 / (static_cast<double>(kTicksPerQuarter) * k) * std::log(b1 / b0);
}

} // namespace

TempoMap::TempoMap() : TempoMap(48000.0) {}

TempoMap::TempoMap(double sampleRate) : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0) {
    tempoPoints_ = {TempoPoint{0, 120.0, false}};
    sigPoints_   = {TimeSignaturePoint{1, 4, 4}};
    rebuild();
}

// ---------------------------------------------------------------------------
// Редактирование
// ---------------------------------------------------------------------------

void TempoMap::setSampleRate(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void TempoMap::setConstantTempo(double bpm) {
    tempoPoints_ = {TempoPoint{0, clampBpm(bpm), false}};
    rebuild();
}

void TempoMap::setTempoPoints(std::vector<TempoPoint> points) {
    tempoPoints_ = std::move(points);
    rebuild();
}

void TempoMap::setTimeSignature(int numerator, int denominator) {
    sigPoints_ = {TimeSignaturePoint{1, numerator, denominator}};
    rebuild();
}

void TempoMap::setTimeSignaturePoints(std::vector<TimeSignaturePoint> points) {
    sigPoints_ = std::move(points);
    rebuild();
}

// ---------------------------------------------------------------------------
// Пересборка кэша сегментов
// ---------------------------------------------------------------------------

void TempoMap::rebuild() {
    // --- темп ---
    if (tempoPoints_.empty())
        tempoPoints_ = {TempoPoint{0, 120.0, false}};

    std::sort(tempoPoints_.begin(), tempoPoints_.end(),
              [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });

    // Карта обязана начинаться в нуле: иначе позиция до первой точки
    // не определена, а такое состояние всплывёт в самый неудобный момент.
    if (tempoPoints_.front().tick != 0)
        tempoPoints_.insert(tempoPoints_.begin(), TempoPoint{0, tempoPoints_.front().bpm, false});

    for (auto& p : tempoPoints_)
        p.bpm = clampBpm(p.bpm);

    tempoSegments_.clear();
    tempoSegments_.reserve(tempoPoints_.size());

    double seconds = 0.0;
    for (std::size_t i = 0; i < tempoPoints_.size(); ++i) {
        const bool hasNext = (i + 1) < tempoPoints_.size();

        TempoSegment seg;
        seg.startTick    = tempoPoints_[i].tick;
        seg.endTick      = hasNext ? tempoPoints_[i + 1].tick : kMaxTick;
        seg.bpmStart     = tempoPoints_[i].bpm;
        seg.isRamp       = hasNext && tempoPoints_[i].rampToNext;
        seg.bpmEnd       = seg.isRamp ? tempoPoints_[i + 1].bpm : seg.bpmStart;
        seg.startSeconds = seconds;

        if (seg.endTick > seg.startTick && hasNext) {
            seconds += seg.isRamp
                     ? rampDuration(seg.startTick, seg.endTick, seg.bpmStart, seg.bpmEnd)
                     : constantDuration(seg.startTick, seg.endTick, seg.bpmStart);
        }
        tempoSegments_.push_back(seg);
    }

    // --- размер ---
    if (sigPoints_.empty())
        sigPoints_ = {TimeSignaturePoint{1, 4, 4}};

    std::sort(sigPoints_.begin(), sigPoints_.end(),
              [](const TimeSignaturePoint& a, const TimeSignaturePoint& b) { return a.bar < b.bar; });

    if (sigPoints_.front().bar != 1)
        sigPoints_.insert(sigPoints_.begin(), TimeSignaturePoint{1, 4, 4});

    for (auto& s : sigPoints_) {
        if (s.numerator   < 1)  s.numerator   = 4;
        if (s.denominator < 1)  s.denominator = 4;
    }

    sigSegments_.clear();
    sigSegments_.reserve(sigPoints_.size());

    Tick tick = 0;
    for (std::size_t i = 0; i < sigPoints_.size(); ++i) {
        const bool hasNext = (i + 1) < sigPoints_.size();

        SigSegment seg;
        seg.startBar     = sigPoints_[i].bar;
        seg.endBar       = hasNext ? sigPoints_[i + 1].bar : 0;
        seg.numerator    = sigPoints_[i].numerator;
        seg.denominator  = sigPoints_[i].denominator;
        // Доля — это нота, равная знаменателю: в 6/8 доля — восьмая.
        seg.ticksPerBeat = kTicksPerQuarter * 4 / seg.denominator;
        seg.ticksPerBar  = seg.ticksPerBeat * seg.numerator;
        seg.startTick    = tick;

        if (hasNext)
            tick += seg.ticksPerBar * (seg.endBar - seg.startBar);

        sigSegments_.push_back(seg);
    }
}

// ---------------------------------------------------------------------------
// Поиск сегментов
// ---------------------------------------------------------------------------

const TempoMap::TempoSegment& TempoMap::tempoSegmentAtTick(Tick tick) const noexcept {
    // upper_bound по startTick, шаг назад — первый сегмент, начавшийся не позже.
    auto it = std::upper_bound(tempoSegments_.begin(), tempoSegments_.end(), tick,
                               [](Tick t, const TempoSegment& s) { return t < s.startTick; });
    if (it == tempoSegments_.begin())
        return tempoSegments_.front();
    return *(it - 1);
}

const TempoMap::TempoSegment& TempoMap::tempoSegmentAtSeconds(double seconds) const noexcept {
    auto it = std::upper_bound(tempoSegments_.begin(), tempoSegments_.end(), seconds,
                               [](double s, const TempoSegment& seg) { return s < seg.startSeconds; });
    if (it == tempoSegments_.begin())
        return tempoSegments_.front();
    return *(it - 1);
}

const TempoMap::SigSegment& TempoMap::sigSegmentAtTick(Tick tick) const noexcept {
    auto it = std::upper_bound(sigSegments_.begin(), sigSegments_.end(), tick,
                               [](Tick t, const SigSegment& s) { return t < s.startTick; });
    if (it == sigSegments_.begin())
        return sigSegments_.front();
    return *(it - 1);
}

const TempoMap::SigSegment& TempoMap::sigSegmentAtBar(int bar) const noexcept {
    auto it = std::upper_bound(sigSegments_.begin(), sigSegments_.end(), bar,
                               [](int b, const SigSegment& s) { return b < s.startBar; });
    if (it == sigSegments_.begin())
        return sigSegments_.front();
    return *(it - 1);
}

// ---------------------------------------------------------------------------
// Конверсии
// ---------------------------------------------------------------------------

double TempoMap::bpmAtTick(Tick tick) const noexcept {
    if (tick < 0) tick = 0;
    const TempoSegment& seg = tempoSegmentAtTick(tick);
    if (!seg.isRamp || seg.endTick <= seg.startTick)
        return seg.bpmStart;

    const double t = static_cast<double>(std::min(tick, seg.endTick) - seg.startTick)
                   / static_cast<double>(seg.endTick - seg.startTick);
    return seg.bpmStart + (seg.bpmEnd - seg.bpmStart) * t;
}

double TempoMap::tickToSeconds(Tick tick) const noexcept {
    if (tick <= 0) return 0.0;
    const TempoSegment& seg = tempoSegmentAtTick(tick);

    if (!seg.isRamp)
        return seg.startSeconds + constantDuration(seg.startTick, tick, seg.bpmStart);

    const double bpmHere = bpmAtTick(tick);
    return seg.startSeconds + rampDuration(seg.startTick, tick, seg.bpmStart, bpmHere);
}

Tick TempoMap::secondsToTick(double seconds) const noexcept {
    if (!(seconds > 0.0)) return 0;
    const TempoSegment& seg = tempoSegmentAtSeconds(seconds);
    const double dt = seconds - seg.startSeconds;

    if (!seg.isRamp) {
        const double ticks = dt * seg.bpmStart * static_cast<double>(kTicksPerQuarter) / 60.0;
        return seg.startTick + static_cast<Tick>(std::llround(ticks));
    }

    // Обращение формулы рампы: bpm(T) = b0·exp(T·TPQ·k/60), затем t = t0 + (bpm − b0)/k.
    const double span = static_cast<double>(seg.endTick - seg.startTick);
    const double k    = (seg.bpmEnd - seg.bpmStart) / span;
    if (std::abs(k) < 1e-12) {
        const double ticks = dt * seg.bpmStart * static_cast<double>(kTicksPerQuarter) / 60.0;
        return seg.startTick + static_cast<Tick>(std::llround(ticks));
    }

    const double bpmHere = seg.bpmStart * std::exp(dt * static_cast<double>(kTicksPerQuarter) * k / 60.0);
    return seg.startTick + static_cast<Tick>(std::llround((bpmHere - seg.bpmStart) / k));
}

SampleCount TempoMap::tickToSample(Tick tick) const noexcept {
    return static_cast<SampleCount>(std::llround(tickToSeconds(tick) * sampleRate_));
}

Tick TempoMap::sampleToTick(SampleCount sample) const noexcept {
    if (sample <= 0) return 0;
    return secondsToTick(static_cast<double>(sample) / sampleRate_);
}

// ---------------------------------------------------------------------------
// Такты и доли
// ---------------------------------------------------------------------------

Tick TempoMap::ticksPerBeatAt(Tick tick) const noexcept {
    return sigSegmentAtTick(tick < 0 ? 0 : tick).ticksPerBeat;
}

Tick TempoMap::ticksPerBarAt(Tick tick) const noexcept {
    return sigSegmentAtTick(tick < 0 ? 0 : tick).ticksPerBar;
}

BarBeat TempoMap::tickToBarBeat(Tick tick) const noexcept {
    if (tick < 0) tick = 0;
    const SigSegment& seg = sigSegmentAtTick(tick);

    const Tick offset = tick - seg.startTick;
    const Tick bars   = offset / seg.ticksPerBar;
    const Tick inBar  = offset % seg.ticksPerBar;

    BarBeat bb;
    bb.bar        = seg.startBar + static_cast<int>(bars);
    bb.beat       = static_cast<int>(inBar / seg.ticksPerBeat) + 1;
    bb.tickInBeat = inBar % seg.ticksPerBeat;
    return bb;
}

Tick TempoMap::barBeatToTick(int bar, int beat, Tick tickInBeat) const noexcept {
    if (bar  < 1) bar  = 1;
    if (beat < 1) beat = 1;

    const SigSegment& seg = sigSegmentAtBar(bar);
    return seg.startTick
         + static_cast<Tick>(bar - seg.startBar) * seg.ticksPerBar
         + static_cast<Tick>(beat - 1) * seg.ticksPerBeat
         + tickInBeat;
}

Tick TempoMap::nextBeatAtOrAfter(Tick tick) const noexcept {
    if (tick <= 0) return 0;
    const SigSegment& seg = sigSegmentAtTick(tick);

    const Tick offset = tick - seg.startTick;
    const Tick beats  = (offset + seg.ticksPerBeat - 1) / seg.ticksPerBeat;   // округление вверх
    const Tick result = seg.startTick + beats * seg.ticksPerBeat;

    // Если округление вверх вынесло нас за границу сегмента, следующая доля —
    // это начало следующего сегмента: смена размера всегда попадает на долю.
    if (seg.endBar != 0) {
        const Tick segEnd = seg.startTick
                          + static_cast<Tick>(seg.endBar - seg.startBar) * seg.ticksPerBar;
        if (result > segEnd)
            return segEnd;
    }
    return result;
}

bool TempoMap::isDownbeat(Tick tick) const noexcept {
    const BarBeat bb = tickToBarBeat(tick);
    return bb.beat == 1 && bb.tickInBeat == 0;
}

// ---------------------------------------------------------------------------
// Сетка
// ---------------------------------------------------------------------------

Tick TempoMap::gridTicks(GridResolution resolution, Tick at) const noexcept {
    switch (resolution) {
    case GridResolution::Bar:              return ticksPerBarAt(at);
    case GridResolution::Half:             return kTicksPerQuarter * 2;
    case GridResolution::Quarter:          return kTicksPerQuarter;
    case GridResolution::Eighth:           return kTicksPerQuarter / 2;
    case GridResolution::Sixteenth:        return kTicksPerQuarter / 4;
    case GridResolution::ThirtySecond:     return kTicksPerQuarter / 8;
    case GridResolution::QuarterTriplet:   return kTicksPerQuarter * 2 / 3;
    case GridResolution::EighthTriplet:    return kTicksPerQuarter / 3;
    case GridResolution::SixteenthTriplet: return kTicksPerQuarter / 6;
    }
    return kTicksPerQuarter;
}

Tick TempoMap::snap(Tick tick, GridResolution resolution) const noexcept {
    if (tick < 0) tick = 0;

    const SigSegment& seg  = sigSegmentAtTick(tick);
    const Tick        step = gridTicks(resolution, tick);
    if (step <= 0) return tick;

    // Сетка отсчитывается от начала сегмента размера, а не от нуля проекта:
    // после смены размера сетка обязана заново выровняться по тактовой черте.
    const Tick offset = tick - seg.startTick;
    const Tick snapped = ((offset + step / 2) / step) * step;
    return seg.startTick + snapped;
}

} // namespace daw::time
