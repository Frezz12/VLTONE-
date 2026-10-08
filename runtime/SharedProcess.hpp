#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace daw::plugins::ipc {

inline constexpr auto kProcessStopWait = std::chrono::seconds(5);

/// Private mapping + nonblocking wake + owned process. No inherited global
/// namespace, no shell, no plugin-controlled pipe output in the protocol.
class SharedProcess {
public:
    SharedProcess();
    ~SharedProcess();
    SharedProcess(const SharedProcess&) = delete;
    SharedProcess& operator=(const SharedProcess&) = delete;
    bool create(std::size_t bytes, std::string& error);
    bool launch(const std::string& executable, std::string& error);
    bool attach(int argc, char** argv, std::string& error);
    std::byte* data() const noexcept;
    std::size_t size() const noexcept;
    std::uint64_t processId() const noexcept;
    bool signal(bool audio = false) noexcept;
    bool wait(int timeoutMs = -1, bool audio = false);
    bool wakeAudioThread() noexcept; // child-local lifecycle/stop wake
    bool running(); // control thread only
    void stop(); // kill/reap before unmapping; control thread only
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace daw::plugins::ipc
