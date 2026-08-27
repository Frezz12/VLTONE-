#pragma once
//
// Планарные (не interleaved) буферы 32-битных float.
//
// Планарность — принципиальный выбор: она нужна для SIMD и позволяет
// маршрутизировать каналы перестановкой указателей, без копирования данных.
// Конвертация в interleaved происходит ровно один раз, на границе с драйвером.
//
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

namespace daw::audio {

// Невладеющий вид на набор канальных буферов. Именно он ходит по аудио-графу.
class AudioBufferView {
public:
    AudioBufferView() = default;

    AudioBufferView(float* const* channels, int numChannels, int numFrames) noexcept
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames) {}

    float*       channel(int i)       noexcept { assert(i >= 0 && i < numChannels_); return channels_[i]; }
    const float* channel(int i) const noexcept { assert(i >= 0 && i < numChannels_); return channels_[i]; }

    int numChannels() const noexcept { return numChannels_; }
    int numFrames()   const noexcept { return numFrames_; }
    bool isEmpty()    const noexcept { return numChannels_ == 0 || numFrames_ == 0; }

    void clear() noexcept {
        for (int c = 0; c < numChannels_; ++c)
            std::memset(channels_[c], 0, static_cast<std::size_t>(numFrames_) * sizeof(float));
    }

    // Урезать вид по числу фреймов — нужно при обработке блока по кускам
    // между сэмпл-точными событиями.
    AudioBufferView slice(int offset, int count) const noexcept {
        assert(offset >= 0 && offset + count <= numFrames_);
        // Указатели на каналы нужно сдвинуть, поэтому view со сдвигом делается
        // вызывающим через отдельный массив указателей. Здесь — только обрезка длины.
        assert(offset == 0 && "сдвиг начала требует собственного массива указателей");
        return AudioBufferView(channels_, numChannels_, count);
    }

    float peak(int channel) const noexcept {
        const float* p = this->channel(channel);
        float m = 0.0f;
        for (int i = 0; i < numFrames_; ++i) {
            const float a = p[i] < 0.0f ? -p[i] : p[i];
            if (a > m) m = a;
        }
        return m;
    }

private:
    float* const* channels_    = nullptr;
    int           numChannels_ = 0;
    int           numFrames_   = 0;
};

// Владеющий буфер. Выделяется ТОЛЬКО вне аудио-потока (в prepare()).
class AudioBuffer {
public:
    AudioBuffer() = default;
    AudioBuffer(int numChannels, int numFrames) { resize(numChannels, numFrames); }

    void resize(int numChannels, int numFrames) {
        storage_.assign(static_cast<std::size_t>(numChannels) * numFrames, 0.0f);
        pointers_.resize(static_cast<std::size_t>(numChannels));
        for (int c = 0; c < numChannels; ++c)
            pointers_[static_cast<std::size_t>(c)] = storage_.data() + static_cast<std::size_t>(c) * numFrames;
        numChannels_ = numChannels;
        numFrames_   = numFrames;
    }

    // Вид на первые `frames` фреймов — размер блока меняется от callback'а
    // к callback'у, а память выделена под максимальный.
    AudioBufferView view(int frames) noexcept {
        assert(frames <= numFrames_);
        return AudioBufferView(pointers_.data(), numChannels_, frames);
    }
    AudioBufferView view() noexcept { return view(numFrames_); }

    int numChannels() const noexcept { return numChannels_; }
    int numFrames()   const noexcept { return numFrames_; }

private:
    std::vector<float>  storage_;
    std::vector<float*> pointers_;
    int numChannels_ = 0;
    int numFrames_   = 0;
};

} // namespace daw::audio
