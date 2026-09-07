#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace daw::engine {
// Shared admission control for sampler preparation. Never called to submit or
// wait from DSP. Playback changes only an atomic; workers observe it on wake.
class BackgroundExecutor {
public:
    struct Task {
        std::function<void()> run;
        bool cancelled = false, running = false, complete = false;
    };
    using Handle = std::shared_ptr<Task>;
    static BackgroundExecutor& instance();
    Handle submit(std::function<void()> work);
    void cancelAndWait(const Handle& task);
    void addPlayback() noexcept { m_playbacks.fetch_add(1, std::memory_order_relaxed); }
    void removePlayback() noexcept { m_playbacks.fetch_sub(1, std::memory_order_relaxed); }
    unsigned concurrencyLimit() const noexcept { return m_playbacks.load(std::memory_order_relaxed) ? 1 : 2; }
    ~BackgroundExecutor();
private:
    BackgroundExecutor();
    void worker();
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<Handle> m_queue;
    std::vector<std::thread> m_workers;
    std::atomic<unsigned> m_playbacks{0};
    unsigned m_active = 0;
    bool m_stopping = false;
};

class BackgroundPlaybackLease {
public:
    BackgroundPlaybackLease() : m_executor(BackgroundExecutor::instance()) {}
    ~BackgroundPlaybackLease() { setPlaying(false); }
    void setPlaying(bool playing) noexcept {
        if (m_playing.exchange(playing, std::memory_order_relaxed) == playing) return;
        if (playing) m_executor.addPlayback(); else m_executor.removePlayback();
    }
private:
    BackgroundExecutor& m_executor;
    std::atomic<bool> m_playing{false};
};
} // namespace daw::engine
