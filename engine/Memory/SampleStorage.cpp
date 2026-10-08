#include "Memory/SampleStorage.hpp"
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace daw::engine {
namespace {
std::atomic<std::size_t> residentSampleBytes{0};
constexpr std::size_t kHeapBudget = 64u * 1024u * 1024u;
bool reserveHeap(std::size_t bytes) {
    auto current = residentSampleBytes.load(std::memory_order_relaxed);
    while (bytes <= kHeapBudget - current) {
        if (residentSampleBytes.compare_exchange_weak(current, current + bytes,
                std::memory_order_relaxed)) return true;
    }
    return false;
}
}
struct SampleStorage::Mapping {
    FILE* file = nullptr;
    float* address = nullptr;
    std::size_t bytes = 0;
    bool readOnly = false;
#if defined(_WIN32)
    HANDLE handle = nullptr;
#endif
    ~Mapping() {
#if defined(_WIN32)
        if (address) UnmapViewOfFile(address);
        if (handle) CloseHandle(handle);
#else
        if (address) munmap(address, bytes);
#endif
        if (file) std::fclose(file);
    }
};
SampleStorage::SampleStorage(std::size_t samples) {
    if (samples > std::numeric_limits<std::size_t>::max() / sizeof(float))
        throw std::length_error("sample storage is too large");
    const auto bytes = samples * sizeof(float);
    if (bytes < 1024 * 1024 && reserveHeap(bytes)) {
        try { m_memory.resize(samples, 0.0f); }
        catch (...) { residentSampleBytes.fetch_sub(bytes); throw; }
        return;
    }
    auto mapping = std::make_unique<Mapping>();
    mapping->bytes = bytes;
    mapping->file = std::tmpfile();
    if (!mapping->file) throw std::runtime_error("cannot create audio cache file");
#if defined(_WIN32)
    const int fd = _fileno(mapping->file);
    if (_chsize_s(fd, bytes) != 0)
        throw std::runtime_error("cannot size audio cache file");
    mapping->handle = CreateFileMappingW(
        reinterpret_cast<HANDLE>(_get_osfhandle(fd)), nullptr, PAGE_READWRITE,
        DWORD(std::uint64_t(bytes) >> 32), DWORD(bytes), nullptr);
    if (mapping->handle)
        mapping->address = static_cast<float*>(MapViewOfFile(
            mapping->handle, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
#else
    const int fd = fileno(mapping->file);
    // Reserve real space before mapping: a sparse file on a full disk can
    // otherwise deliver SIGBUS while a decoder stores samples into its pages.
#if defined(__APPLE__)
    fstore_t allocation{};
    allocation.fst_flags = F_ALLOCATEALL;
    allocation.fst_posmode = F_PEOFPOSMODE;
    allocation.fst_length = off_t(bytes);
    if (fcntl(fd, F_PREALLOCATE, &allocation) != 0)
        throw std::runtime_error("not enough space for audio cache");
#else
    if (posix_fallocate(fd, 0, off_t(bytes)) != 0)
        throw std::runtime_error("not enough space for audio cache");
#endif
    if (ftruncate(fd, off_t(bytes)) != 0)
        throw std::runtime_error("cannot size audio cache file");
    void* address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address != MAP_FAILED) mapping->address = static_cast<float*>(address);
#endif
    if (!mapping->address) throw std::runtime_error("cannot map audio cache file");
    // A mapping keeps its backing object alive. Release file descriptors now,
    // otherwise a thousand imported clips can exhaust the process FD limit.
    std::fclose(mapping->file);
    mapping->file = nullptr;
#if defined(_WIN32)
    CloseHandle(mapping->handle);
    mapping->handle = nullptr;
#endif
    m_mapping = std::move(mapping);
}
SampleStorage::~SampleStorage() {
    residentSampleBytes.fetch_sub(m_memory.size() * sizeof(float), std::memory_order_relaxed);
}
SampleStorage::SampleStorage(const std::filesystem::path& path, std::size_t samples) {
    if (!samples || samples > std::numeric_limits<std::size_t>::max() / sizeof(float))
        throw std::length_error("invalid mapped audio size");
    auto mapping = std::make_unique<Mapping>();
    mapping->bytes = samples * sizeof(float);
    mapping->readOnly = true;
#if defined(_WIN32)
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open audio resource");
    LARGE_INTEGER size{};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileSizeEx(file, &size) && GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
        size.QuadPart >= 0 && std::uint64_t(size.QuadPart) == mapping->bytes;
    if (valid) mapping->handle = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!valid) throw std::runtime_error("invalid audio resource file or size");
    if (mapping->handle) mapping->address = static_cast<float*>(MapViewOfFile(
        mapping->handle, FILE_MAP_READ, 0, 0, mapping->bytes));
#else
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("cannot open audio resource");
    struct stat info{};
    const bool valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        std::uint64_t(info.st_size) == mapping->bytes;
    if (valid) {
        void* address = mmap(nullptr, mapping->bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (address != MAP_FAILED) mapping->address = static_cast<float*>(address);
    }
    close(fd);
    if (!valid) throw std::runtime_error("invalid audio resource file or size");
#endif
    if (!mapping->address) throw std::runtime_error("cannot map audio resource read-only");
#if defined(_WIN32)
    CloseHandle(mapping->handle);
    mapping->handle = nullptr;
#endif
    m_mapping = std::move(mapping);
}
float* SampleStorage::data() noexcept {
    if (readOnly()) return nullptr;
    return m_mapping ? m_mapping->address : m_memory.data();
}
const float* SampleStorage::data() const noexcept {
    return m_mapping ? m_mapping->address : m_memory.data();
}
bool SampleStorage::fileBacked() const noexcept { return bool(m_mapping); }
bool SampleStorage::readOnly() const noexcept { return m_mapping && m_mapping->readOnly; }
} // namespace daw::engine
