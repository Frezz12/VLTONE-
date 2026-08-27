#pragma once
//
// Тестовый генератор синуса. На M0 это единственный источник звука; его задача —
// доказать, что тракт «UI → движок → драйвер → колонки» работает.
//
// Даже здесь соблюдаются два правила, которые дальше будут везде:
//   * фаза считается в double — при float на 48 кГц она заметно уплывает;
//   * громкость и частота сглаживаются, иначе движение слайдера даёт щелчки.
//
#include <atomic>
#include <cmath>

#include "daw/audio/AudioBuffer.h"

namespace daw::audio {

class ToneSource {
public:
    void prepare(double sampleRate, int /*maxBlockSize*/) noexcept {
        sampleRate_ = sampleRate;
        phase_      = 0.0;
        // Одно-полюсное сглаживание с постоянной времени ~20 мс.
        smoothing_  = static_cast<float>(std::exp(-1.0 / (sampleRate * 0.020)));
        currentGain_ = 0.0f;
        currentFreq_ = targetFreq_.load(std::memory_order_relaxed);
    }

    // Все три — из UI-потока.
    void setFrequency(float hz) noexcept { targetFreq_.store(hz, std::memory_order_relaxed); }
    void setGain(float linear)  noexcept { targetGain_.store(linear, std::memory_order_relaxed); }
    void setActive(bool on)     noexcept { active_.store(on, std::memory_order_relaxed); }

    bool isActive() const noexcept { return active_.load(std::memory_order_relaxed); }

    // Аудио-поток. Подмешивается поверх содержимого буфера.
    void process(AudioBufferView& out, int numFrames) noexcept {
        const float targetGain = active_.load(std::memory_order_relaxed)
                               ? targetGain_.load(std::memory_order_relaxed)
                               : 0.0f;
        const float targetFreq = targetFreq_.load(std::memory_order_relaxed);

        const int    channels = out.numChannels();
        const double twoPiOverSr = 6.283185307179586476 / sampleRate_;

        for (int i = 0; i < numFrames; ++i) {
            currentGain_ = targetGain + (currentGain_ - targetGain) * smoothing_;
            currentFreq_ = targetFreq + (currentFreq_ - targetFreq) * smoothing_;

            const float s = static_cast<float>(std::sin(phase_)) * currentGain_;

            phase_ += static_cast<double>(currentFreq_) * twoPiOverSr;
            if (phase_ >= 6.283185307179586476)
                phase_ -= 6.283185307179586476;

            for (int c = 0; c < channels; ++c)
                out.channel(c)[i] += s;
        }
    }

private:
    double sampleRate_  = 48000.0;
    double phase_       = 0.0;
    float  smoothing_   = 0.999f;
    float  currentGain_ = 0.0f;
    float  currentFreq_ = 440.0f;

    std::atomic<float> targetFreq_{440.0f};
    std::atomic<float> targetGain_{0.2f};
    std::atomic<bool>  active_{false};
};

} // namespace daw::audio
