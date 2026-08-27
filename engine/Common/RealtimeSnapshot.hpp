#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace daw::engine {

/// Single-realtime-reader immutable snapshot publication.
///
/// The reader touches only raw atomics. Shared ownership and all destruction
/// remain on the publishing/control thread, avoiding both atomic_shared_ptr's
/// implementation lock and last-reference reclamation in a realtime callback.
template <typename T>
class RealtimeSnapshot {
public:
    class Reader {
    public:
        Reader() = default;
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;
        Reader(Reader&& other) noexcept
            : m_source(other.m_source), m_pointer(other.m_pointer) {
            other.m_source = nullptr;
            other.m_pointer = nullptr;
        }
        Reader& operator=(Reader&& other) noexcept {
            if (this == &other) return *this;
            release();
            m_source = other.m_source;
            m_pointer = other.m_pointer;
            other.m_source = nullptr;
            other.m_pointer = nullptr;
            return *this;
        }
        ~Reader() { release(); }

        const T* get() const noexcept { return m_pointer; }
        const T& operator*() const noexcept { return *m_pointer; }
        const T* operator->() const noexcept { return m_pointer; }
        explicit operator bool() const noexcept { return m_pointer != nullptr; }

    private:
        friend class RealtimeSnapshot;
        Reader(RealtimeSnapshot* source, const T* pointer) noexcept
            : m_source(source), m_pointer(pointer) {}
        void release() noexcept {
            if (m_source) m_source->m_hazard.store(nullptr, std::memory_order_release);
            m_source = nullptr;
            m_pointer = nullptr;
        }
        RealtimeSnapshot* m_source = nullptr;
        const T* m_pointer = nullptr;
    };

    RealtimeSnapshot() = default;
    explicit RealtimeSnapshot(std::shared_ptr<const T> initial) {
        publish(std::move(initial));
    }
    RealtimeSnapshot(const RealtimeSnapshot&) = delete;
    RealtimeSnapshot& operator=(const RealtimeSnapshot&) = delete;

    void publish(std::shared_ptr<const T> snapshot) {
        {
            std::lock_guard lock(m_ownerMutex);
            if (m_owner) m_retired.push_back(std::move(m_owner));
            m_owner = std::move(snapshot);
            m_raw.store(m_owner.get(), std::memory_order_seq_cst);
        }
        reclaim();
    }

    Reader read() noexcept {
        const T* snapshot = nullptr;
        do {
            snapshot = m_raw.load(std::memory_order_seq_cst);
            m_hazard.store(snapshot, std::memory_order_seq_cst);
        } while (snapshot != m_raw.load(std::memory_order_seq_cst));
        return Reader(this, snapshot);
    }

    std::shared_ptr<const T> controlCopy() const {
        std::lock_guard lock(m_ownerMutex);
        return m_owner;
    }

private:
    void reclaim() {
        const T* hazard = m_hazard.load(std::memory_order_seq_cst);
        std::lock_guard lock(m_ownerMutex);
        std::erase_if(m_retired, [hazard](const auto& item) {
            return item.get() != hazard;
        });
    }

    std::atomic<const T*> m_raw{nullptr};
    std::atomic<const T*> m_hazard{nullptr};
    mutable std::mutex m_ownerMutex;
    std::shared_ptr<const T> m_owner;
    std::vector<std::shared_ptr<const T>> m_retired;
};

} // namespace daw::engine
