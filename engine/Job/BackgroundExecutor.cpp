#include "Job/BackgroundExecutor.hpp"
#include "ScopedNoDenormals.hpp"
#include <algorithm>
#include <chrono>
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace daw::engine {
BackgroundExecutor& BackgroundExecutor::instance() {
    static BackgroundExecutor executor;
    return executor;
}
BackgroundExecutor::BackgroundExecutor() {
    for (unsigned i = 0; i < 2; ++i) m_workers.emplace_back([this] { worker(); });
}
BackgroundExecutor::~BackgroundExecutor() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        for (auto& task : m_queue) { task->cancelled = true; task->complete = true; }
        m_queue.clear();
    }
    m_changed.notify_all();
    for (auto& thread : m_workers) thread.join();
}
BackgroundExecutor::Handle BackgroundExecutor::submit(std::function<void()> work) {
    auto task = std::make_shared<Task>();
    task->run = std::move(work);
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(task);
    }
    m_changed.notify_all();
    return task;
}
void BackgroundExecutor::cancelAndWait(const Handle& task) {
    if (!task) return;
    std::unique_lock lock(m_mutex);
    task->cancelled = true;
    std::erase(m_queue, task);
    m_changed.wait(lock, [&] { return !task->running; });
    task->complete = true;
}
void BackgroundExecutor::worker() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    const rt::ScopedNoDenormals noDenormals;
    for (;;) {
        Handle task;
        {
            std::unique_lock lock(m_mutex);
            m_changed.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return m_stopping || (!m_queue.empty() && m_active < concurrencyLimit());
            });
            if (m_stopping) return;
            if (m_queue.empty() || m_active >= concurrencyLimit()) continue;
            task = std::move(m_queue.front()); m_queue.pop_front();
            if (task->cancelled) { task->complete = true; continue; }
            task->running = true; ++m_active;
        }
        try { task->run(); } catch (...) { /* Task owners publish failures themselves. */ }
        {
            std::lock_guard lock(m_mutex);
            task->running = false; task->complete = true; --m_active;
        }
        m_changed.notify_all();
    }
}
} // namespace daw::engine
