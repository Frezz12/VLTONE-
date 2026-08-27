#pragma once
//
// Метроном. Помимо очевидной пользы это первая настоящая проверка принципа №4
// из ARCHITECTURE.md: события обязаны иметь смещение ВНУТРИ блока обработки.
//
// Если щёлкать в начале того блока, куда попала доля, при буфере 512 сэмплов
// клик уедет до 10.7 мс — это отчётливо слышно и делает метроном непригодным.
// Поэтому каждый клик получает точное смещение в сэмплах, и именно так дальше
// будут работать MIDI-события и точки автоматизации.
//
#include <atomic>
#include <cstdint>

#include "daw/audio/AudioBuffer.h"
#include "daw/time/TempoMap.h"

namespace daw::audio {

class Metronome {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;   // при остановке и перемотке

    void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
    void setGain(float linear) noexcept { gain_.store(linear, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // Аудио-поток. blockStartSample — позиция транспорта на начало блока.
    void process(AudioBufferView& out, int numFrames,
                 const time::TempoMap& map,
                 time::SampleCount blockStartSample) noexcept;

private:
    static constexpr int kMaxVoices = 8;

    struct Voice {
        bool   active    = false;
        int    startOffset = 0;   // смещение первого сэмпла в текущем блоке
        int    remaining = 0;
        double phase     = 0.0;
        double phaseInc  = 0.0;
        float  amplitude = 0.0f;
        float  decay     = 0.0f;
    };

    void trigger(int offsetInBlock, bool accent) noexcept;

    Voice  voices_[kMaxVoices]{};
    double sampleRate_ = 48000.0;

    // Последняя отщёлканная доля. Защищает от двойного клика, когда доля
    // из-за округления попадает на границу двух блоков.
    time::Tick lastTriggeredTick_ = -1;

    std::atomic<bool>  enabled_{false};
    std::atomic<float> gain_{0.5f};
};

} // namespace daw::audio
