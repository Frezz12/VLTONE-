#pragma once
//
// Автоматическая проверка того, что в аудио-потоке нет запрещённых операций.
//
// Принцип №2 из ARCHITECTURE.md звучит как «в аудио-потоке нет аллокаций,
// локов и файлов». Проверяться он должен машиной, а не внимательностью:
// человек нарушит его на третьем месяце и заметит через полгода по щелчкам.
//
// При DAW_RT_CHECKS=ON движок подменяет глобальные operator new/delete и,
// если аллокация случилась в потоке, помеченном как аудио, фиксирует нарушение.
//
// Само нарушение НЕ логируется на месте — вывод в консоль из аудио-потока сам
// по себе нарушение. Пишется только атомарный счётчик и указатель на строковый
// литерал; UI показывает это в статусной строке.
//
#include <atomic>
#include <cstdint>

namespace daw::rt {

// Помечен ли текущий поток как аудио-поток реального времени.
bool isAudioThread() noexcept;

// Пометить/снять пометку. Обычно через ScopedAudioThread.
void setAudioThread(bool value) noexcept;

// RAII-пометка на время аудио-callback'а.
class ScopedAudioThread {
public:
    ScopedAudioThread() noexcept  { setAudioThread(true); }
    ~ScopedAudioThread() noexcept { setAudioThread(false); }
    ScopedAudioThread(const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator=(const ScopedAudioThread&) = delete;
};

// Накопленная статистика нарушений.
struct Violations {
    std::uint64_t count = 0;
    const char*   last  = nullptr;   // строковый литерал, жив всегда
};

Violations violations() noexcept;
void       resetViolations() noexcept;

// Зафиксировать нарушение. RT-safe: только атомарные записи.
void reportViolation(const char* what) noexcept;

// Ронять процесс сразу при первом нарушении (по умолчанию нет: некоторые
// backend'ы аудио-драйверов аллоцируют внутри собственного callback'а,
// и падать из-за чужого кода на старте неудобно).
void setAbortOnViolation(bool value) noexcept;

// Скомпилированы ли проверки в этой сборке.
bool checksEnabled() noexcept;

// Выставить в аудио-потоке flush-to-zero / denormals-are-zero и вернуть как было.
// Денормалы в хвостах фильтров и ревербераций роняют производительность
// в десятки раз, поэтому это не опция, а обязательный пролог callback'а.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept;
    ~ScopedNoDenormals() noexcept;
    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;
private:
    std::uint64_t saved_;
};

} // namespace daw::rt
