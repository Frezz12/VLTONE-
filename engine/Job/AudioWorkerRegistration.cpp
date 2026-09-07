#include "Job/AudioWorkerRegistration.hpp"
#include <algorithm>
#include <cmath>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <os/workgroup.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#endif

namespace daw::engine {
struct AudioWorkerRegistration::Impl {
    rt::AudioWorkerConfig config;
#if defined(__APPLE__)
    os_workgroup_join_token_s token{};
    bool joined = false, realtime = false;
#elif defined(_WIN32)
    HANDLE mmcss = nullptr;
#endif
};
AudioWorkerRegistration::AudioWorkerRegistration() : m_impl(std::make_unique<Impl>()) {}
AudioWorkerRegistration::~AudioWorkerRegistration() { configure({}); }
unsigned AudioWorkerRegistration::configure(const rt::AudioWorkerConfig& config) noexcept {
    auto& p = *m_impl;
    unsigned status = 0;
#if defined(__APPLE__)
    if (__builtin_available(macOS 11.0, *)) {
        if (p.joined) os_workgroup_leave(static_cast<os_workgroup_t>(p.config.workgroup.get()), &p.token);
    }
    p.joined = false;
    if (p.realtime) {
        thread_extended_policy_data_t normal{1};
        thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_POLICY,
                          reinterpret_cast<thread_policy_t>(&normal), THREAD_EXTENDED_POLICY_COUNT);
    }
    p.realtime = false;
    pthread_set_qos_class_self_np(config.active ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
    if (config.active && config.sampleRate > 0 && config.blockFrames > 0) {
        mach_timebase_info_data_t clock{};
        mach_timebase_info(&clock);
        const double nanos = std::clamp(double(config.blockFrames) * 1e9 / config.sampleRate, 100000., 100000000.);
        const auto ticks = std::uint32_t(nanos * clock.denom / clock.numer);
        thread_time_constraint_policy_data_t policy{ticks, std::max(1u, ticks * 4 / 5), ticks, 1};
        p.realtime = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
        if (p.realtime) status |= 1;
        if (__builtin_available(macOS 11.0, *)) {
            if (p.realtime && config.workgroup) {
                p.joined = os_workgroup_join(static_cast<os_workgroup_t>(config.workgroup.get()), &p.token) == 0;
                if (p.joined) status |= 2;
            }
        }
    }
#elif defined(_WIN32)
    if (p.mmcss) AvRevertMmThreadCharacteristics(p.mmcss);
    p.mmcss = nullptr;
    if (config.active) {
        DWORD index = 0;
        p.mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
        if (p.mmcss) { AvSetMmThreadPriority(p.mmcss, AVRT_PRIORITY_HIGH); status |= 1; }
    }
#endif
    p.config = config;
    return status;
}
} // namespace daw::engine
