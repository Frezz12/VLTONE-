#include "PlatformDiagnostics.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSysInfo>
#include <QThread>

#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif

namespace {

#if defined(Q_OS_WIN)
quint64 fileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

QString windowsCPUName() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) != ERROR_SUCCESS) return {};
    wchar_t value[512]{};
    DWORD bytes = sizeof(value);
    const LONG status = RegQueryValueExW(key, L"ProcessorNameString", nullptr,
        nullptr, reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? QString::fromWCharArray(value).trimmed() : QString();
}
#elif defined(Q_OS_MACOS)
QByteArray sysctlText(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    QByteArray value(int(size), Qt::Uninitialized);
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    value.resize(int(size));
    while (value.endsWith('\0')) value.chop(1);
    return value;
}

quint64 sysctlUInt64(const char* name) {
    quint64 value = 0;
    size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : 0;
}
#endif

QJsonArray gpuSnapshot() {
    QJsonArray result;
#if defined(Q_OS_WIN)
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        result.append(QJsonObject{
            {QStringLiteral("name"), QString::fromWCharArray(device.DeviceString)},
            {QStringLiteral("driver"), QString::fromWCharArray(device.DeviceName)},
        });
    }
#elif defined(Q_OS_MACOS)
    QProcess process;
    process.start(QStringLiteral("/usr/sbin/system_profiler"),
                  {QStringLiteral("SPDisplaysDataType"), QStringLiteral("-json")},
                  QIODevice::ReadOnly);
    if (process.waitForFinished(3000)) {
        const QJsonObject root = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        const QJsonArray displays = root.value(QStringLiteral("SPDisplaysDataType")).toArray();
        for (const QJsonValue& value : displays) {
            const QJsonObject display = value.toObject();
            result.append(QJsonObject{
                {QStringLiteral("name"), display.value(QStringLiteral("sppci_model"))},
                {QStringLiteral("driver"), display.value(QStringLiteral("spdisplays_metal"))},
            });
        }
    }
#endif
    return result;
}
}

QJsonObject PlatformDiagnostics::hardwareSnapshot() {
    QString cpu;
    quint64 ram = 0;
#if defined(Q_OS_WIN)
    cpu = windowsCPUName();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) ram = memory.ullTotalPhys;
#elif defined(Q_OS_MACOS)
    cpu = QString::fromUtf8(sysctlText("machdep.cpu.brand_string"));
    ram = sysctlUInt64("hw.memsize");
#endif
    return {
        {QStringLiteral("cpu_model"), cpu},
        {QStringLiteral("cpu_cores"), QThread::idealThreadCount()},
        {QStringLiteral("cpu_threads"), QThread::idealThreadCount()},
        {QStringLiteral("gpu"), gpuSnapshot()},
        {QStringLiteral("ram_bytes"), double(ram)},
        {QStringLiteral("os_name"), QSysInfo::productType()},
        {QStringLiteral("os_version"), QSysInfo::productVersion()},
        {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
    };
}

QJsonObject PlatformDiagnostics::processSample() {
    double processCPU = 0.0;
    double systemCPU = 0.0;
    quint64 resident = 0;
#if defined(Q_OS_WIN)
    static quint64 previousWall = 0;
    static quint64 previousProcess = 0;
    static quint64 previousSystemTotal = 0;
    static quint64 previousSystemIdle = 0;
    FILETIME created{}, exited{}, kernel{}, user{}, idle{}, systemKernel{}, systemUser{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimes(&idle, &systemKernel, &systemUser);
    const quint64 nowWall = GetTickCount64() * 10'000;
    const quint64 nowProcess = fileTimeValue(kernel) + fileTimeValue(user);
    const quint64 total = fileTimeValue(systemKernel) + fileTimeValue(systemUser);
    const quint64 idleValue = fileTimeValue(idle);
    if (previousWall && nowWall > previousWall)
        processCPU = double(nowProcess - previousProcess) / double(nowWall - previousWall) * 100.0;
    if (previousSystemTotal && total > previousSystemTotal)
        systemCPU = double((total - previousSystemTotal) - (idleValue - previousSystemIdle)) /
                    double(total - previousSystemTotal) * 100.0;
    previousWall = nowWall; previousProcess = nowProcess;
    previousSystemTotal = total; previousSystemIdle = idleValue;
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        resident = memory.WorkingSetSize;
#elif defined(Q_OS_MACOS)
    static quint64 previousProcessMicros = 0;
    static qint64 previousWallMillis = 0;
    task_thread_times_info_data_t times{};
    mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                  reinterpret_cast<task_info_t>(&times), &count) == KERN_SUCCESS) {
        const quint64 micros = quint64(times.user_time.seconds + times.system_time.seconds) * 1'000'000 +
            quint64(times.user_time.microseconds + times.system_time.microseconds);
        const qint64 wall = QDateTime::currentMSecsSinceEpoch();
        if (previousWallMillis && wall > previousWallMillis)
            processCPU = double(micros - previousProcessMicros) / double((wall - previousWallMillis) * 1000) * 100.0;
        previousProcessMicros = micros; previousWallMillis = wall;
    }
    task_basic_info_data_t basic{};
    count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&basic), &count) == KERN_SUCCESS)
        resident = basic.resident_size;
    natural_t processors = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t infoCount = 0;
    static quint64 previousTotal = 0;
    static quint64 previousIdle = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &processors,
                            &info, &infoCount) == KERN_SUCCESS) {
        quint64 total = 0, idle = 0;
        auto* ticks = reinterpret_cast<processor_cpu_load_info_t>(info);
        for (natural_t i = 0; i < processors; ++i) {
            for (int state = 0; state < CPU_STATE_MAX; ++state) total += ticks[i].cpu_ticks[state];
            idle += ticks[i].cpu_ticks[CPU_STATE_IDLE];
        }
        if (previousTotal && total > previousTotal)
            systemCPU = double((total - previousTotal) - (idle - previousIdle)) /
                        double(total - previousTotal) * 100.0;
        previousTotal = total; previousIdle = idle;
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info),
                      vm_size_t(infoCount * sizeof(integer_t)));
    }
#endif
    return {
        {QStringLiteral("process_cpu"), std::clamp(processCPU, 0.0, 10'000.0)},
        {QStringLiteral("system_cpu"), std::clamp(systemCPU, 0.0, 100.0)},
        {QStringLiteral("resident_bytes"), double(resident)},
    };
}
