#include "daw/audio/Metronome.h"

#include <algorithm>
#include <cmath>

namespace daw::audio {
namespace {

constexpr double kTwoPi = 6.283185307179586476;

// Акцент на первой доле такта выше и громче — так на слух сразу понятно,
// где начинается такт, без взгляда на экран.
constexpr double kAccentHz = 1568.0;   // ~G6
constexpr double kBeatHz   = 1046.5;   // ~C6

constexpr double kClickSeconds = 0.040;
constexpr double kDecayTau     = 0.007;   // резкая атака и быстрый спад = «тик»

// Больше 64 долей на один блок обработки не бывает ни при каком разумном
// темпе. Ограничение защищает аудио-поток от бесконечного цикла, если карта
// темпа окажется испорченной.
constexpr int kMaxBeatsPerBlock = 64;

} // namespace

void Metronome::prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
}

void Metronome::reset() noexcept {
    for (auto& v : voices_)
        v = Voice{};
    lastTriggeredTick_ = -1;
}

void Metronome::trigger(int offsetInBlock, bool accent) noexcept {
    // Ищем свободный голос, иначе забираем самый тихий: пропущенный клик
    // хуже, чем оборванный хвост предыдущего.
    Voice* target  = nullptr;
    float  quietest = 1e9f;

    for (auto& v : voices_) {
        if (!v.active) { target = &v; break; }
        if (v.amplitude < quietest) { quietest = v.amplitude; target = &v; }
    }
    if (!target)
        return;

    target->active      = true;
    target->startOffset = offsetInBlock;
    target->remaining   = static_cast<int>(kClickSeconds * sampleRate_);
    target->phase       = 0.0;
    target->phaseInc    = kTwoPi * (accent ? kAccentHz : kBeatHz) / sampleRate_;
    target->amplitude   = accent ? 1.0f : 0.55f;
    target->decay       = static_cast<float>(std::exp(-1.0 / (sampleRate_ * kDecayTau)));
}

void Metronome::process(AudioBufferView& out, int numFrames,
                        const time::TempoMap& map,
                        time::SampleCount blockStartSample) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) {
        reset();
        return;
    }

    // ---- 1. Найти доли, попадающие в этот блок ----------------------------

    const time::SampleCount blockEnd = blockStartSample + numFrames;
    const time::Tick blockStartTick = map.sampleToTick(blockStartSample);

    // Смена темпа переопределяет отображение сэмплов в тики целиком. Транспорт
    // продолжает идти вперёд по сэмплам, но тик может уехать НАЗАД: на 30-й
    // секунде при 120 BPM это тик 460800, а после перехода на 60 BPM — уже
    // 230400. Защита от двойного клика (tick > lastTriggeredTick_) тогда
    // заглушила бы метроном на всё время, пока позиция не догонит старый тик.
    // Уход назад означает не повтор, а переразметку времени — сбрасываем.
    if (blockStartTick < lastTriggeredTick_)
        lastTriggeredTick_ = -1;

    time::Tick tick = map.nextBeatAtOrAfter(blockStartTick);

    for (int i = 0; i < kMaxBeatsPerBlock; ++i) {
        const time::SampleCount beatSample = map.tickToSample(tick);
        if (beatSample >= blockEnd)
            break;

        if (tick > lastTriggeredTick_) {
            // Смещение может оказаться отрицательным из-за округления при
            // конверсии тиков в сэмплы. Прижимаем к нулю, а не отбрасываем:
            // потерянный клик заметнее, чем сдвиг на один сэмпл.
            const int offset = static_cast<int>(
                std::clamp<time::SampleCount>(beatSample - blockStartSample, 0, numFrames - 1));
            trigger(offset, map.isDownbeat(tick));
            lastTriggeredTick_ = tick;
        }

        const time::Tick next = map.nextBeatAtOrAfter(tick + 1);
        if (next <= tick)
            break;
        tick = next;
    }

    // ---- 2. Синтез ---------------------------------------------------------

    const float gain     = gain_.load(std::memory_order_relaxed);
    const int   channels = out.numChannels();

    for (auto& v : voices_) {
        if (!v.active)
            continue;

        const int from = v.startOffset;
        v.startOffset = 0;   // в следующих блоках голос продолжается с начала

        for (int i = from; i < numFrames && v.remaining > 0; ++i) {
            const float s = static_cast<float>(std::sin(v.phase)) * v.amplitude * gain;

            v.phase += v.phaseInc;
            if (v.phase >= kTwoPi)
                v.phase -= kTwoPi;
            v.amplitude *= v.decay;
            --v.remaining;

            for (int c = 0; c < channels; ++c)
                out.channel(c)[i] += s;
        }

        if (v.remaining <= 0)
            v.active = false;
    }
}

} // namespace daw::audio
