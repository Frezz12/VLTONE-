#include "WasmProgram.hpp"
#include "CodeAbi.hpp"
#include "CodeUtilities.hpp"
#include "Common/LockFreeQueue.hpp"
#include "CompilerProcess.hpp"
#include "CreatorCompilerClient.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wasm_export.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace daw::plugins::mini {
namespace {
thread_local bool realtime = false;
std::atomic<unsigned> realtimeMemory{0};
void *allocate(unsigned size) {
  if (realtime) {
    ++realtimeMemory;
    return nullptr;
  }
  return std::malloc(size);
}
void *reallocate(void *p, unsigned size) {
  if (realtime) {
    ++realtimeMemory;
    return nullptr;
  }
  return std::realloc(p, size);
}
void release(void *p) {
  if (realtime && p) {
    ++realtimeMemory;
    return;
  }
  std::free(p);
}
struct RealtimeScope {
  bool previous = realtime;
  RealtimeScope() { realtime = true; }
  ~RealtimeScope() { realtime = previous; }
};
struct MemoryOwner {
  bool preparing = false;
  unsigned next = 0;
};
std::uint32_t reserve(wasm_exec_env_t env, unsigned bytes, unsigned align) {
  auto instance = wasm_runtime_get_module_inst(env);
  auto *owner =
      static_cast<MemoryOwner *>(wasm_runtime_get_custom_data(instance));
  if (!owner || !owner->preparing || !align || align > 4096 ||
      (align & (align - 1)) || bytes > kCodeMemoryLimit) {
    wasm_runtime_set_exception(instance, "Creator: allocation outside prepare");
    return 0;
  }
  const std::uint64_t offset =
      (std::uint64_t(owner->next) + align - 1) & ~std::uint64_t(align - 1);
  const auto end = offset + bytes;
  if (!offset || end > kCodeMemoryLimit) {
    wasm_runtime_set_exception(instance, "Creator: memory limit exceeded");
    return 0;
  }
  const auto memory = wasm_runtime_get_default_memory(instance);
  const auto pages = wasm_memory_get_cur_page_count(memory);
  const auto pageSize = wasm_memory_get_bytes_per_page(memory);
  if (pageSize != 65536) {
    wasm_runtime_set_exception(instance,
                               "Creator: unsupported memory page size");
    return 0;
  }
  const auto required = (end + pageSize - 1) / pageSize;
  if (required > pages &&
      !wasm_runtime_enlarge_memory(instance, unsigned(required - pages))) {
    wasm_runtime_set_exception(instance, "Creator: memory preparation failed");
    return 0;
  }
  owner->next = unsigned(end);
  return unsigned(offset);
}
bool initializeRuntime() {
  static const bool ready = [] {
    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func =
        reinterpret_cast<void *>(allocate);
    args.mem_alloc_option.allocator.realloc_func =
        reinterpret_cast<void *>(reallocate);
    args.mem_alloc_option.allocator.free_func =
        reinterpret_cast<void *>(release);
    if (!wasm_runtime_full_init(&args))
      return false;
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_FATAL);
    static NativeSymbol imports[] = {
        {"reserve", reinterpret_cast<void *>(reserve), "(ii)i", nullptr}};
    return bool(wasm_runtime_register_natives("vlt", imports, 1));
  }();
  return ready;
}
template <class T> void pack(std::uint32_t *&to, T value) {
  std::memcpy(to, &value, sizeof(value));
  to += sizeof(value) / 4;
}
} // namespace
struct WasmProgram::Impl {
  std::vector<std::uint8_t> bytes, snapshot;
#ifdef _WIN32
  void *mappedCode = nullptr;
#endif
  wasm_module_t module = nullptr;
  wasm_module_inst_t instance = nullptr;
  wasm_exec_env_t env = nullptr;
  wasm_function_inst_t process = nullptr, reset = nullptr, budget = nullptr;
  MemoryOwner owner;
  std::array<CodeSlot, 64> slots{};
  std::array<std::array<float *, 16>, 64> inputs{}, outputs{};
  std::array<unsigned, 64> outputCounts{};
  unsigned maxFrames = 0;
  std::uint8_t *memory = nullptr;
  std::array<char, 256> failure{};
  engine::LockFreeSPSCQueue<std::array<char, 256>, 8> diagnostics;
  bool broken = false;
  void *stackPointer = nullptr;
  std::uint32_t initialStackPointer = 0;
  ~Impl() {
    if (env)
      wasm_runtime_destroy_exec_env(env);
    if (instance)
      wasm_runtime_deinstantiate(instance);
    if (module)
      wasm_runtime_unload(module);
#ifdef _WIN32
    if (mappedCode)
      VirtualFree(mappedCode, 0, MEM_RELEASE);
#endif
  }
  bool call(wasm_function_inst_t f, unsigned argc,
            std::uint32_t *args) noexcept {
    if (!f || !wasm_runtime_call_wasm(env, f, argc, args)) {
      broken = true;
      const char *why = wasm_runtime_get_exception(instance);
      if (!why)
        why = "Creator function failed";
      std::size_t n = 0;
      while (n + 1 < failure.size() && why[n]) {
        failure[n] = why[n];
        ++n;
      }
      failure[n] = '\0';
      diagnostics.push(failure);
      return false;
    }
    return true;
  }
  bool fuel(std::uint64_t amount) noexcept {
    std::uint32_t args[2];
    std::memcpy(args, &amount, 8);
    return call(budget, 2, args);
  }
};
WasmProgram::WasmProgram() : m(std::make_unique<Impl>()) {}
WasmProgram::~WasmProgram() = default;
bool WasmProgram::prepare(const MiniModuleDefinition &d,
                          const PluginProcessInfo &info, unsigned channels,
                          std::uint64_t seed, std::string &error) {
  m = std::make_unique<Impl>();
  if (!initializeRuntime()) {
    error = "Unable to initialize Creator runtime";
    return false;
  }
  if (d.code.sourceHash != graphCodeHash(d)) {
    error = "C++ code is stale; compile the module in Creator";
    return false;
  }
  std::filesystem::path aot;
  if (!prepareCodeArtifact(d.code, aot, error))
    return false;
  const auto data = readCreatorText(aot, kMaxCodeArtifactBytes * 4);
  m->bytes.assign(data.begin(), data.end());
  auto *code = m->bytes.data();
#ifdef _WIN32
  // XIP executes from the file buffer itself. Never change page permissions on
  // a vector's shared heap pages: give the loader its own writable mapping,
  // finish its relocations, then seal that mapping before any code runs.
  if (wasm_runtime_is_xip_file(code, unsigned(m->bytes.size()))) {
    m->mappedCode = VirtualAlloc(nullptr, m->bytes.size(),
                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m->mappedCode) {
      error = "Unable to allocate Creator code memory";
      return false;
    }
    std::memcpy(m->mappedCode, code, m->bytes.size());
    code = static_cast<std::uint8_t *>(m->mappedCode);
  }
#endif
  char diagnostic[512]{};
  m->module = wasm_runtime_load(code, unsigned(m->bytes.size()), diagnostic,
                                sizeof(diagnostic));
  if (!m->module) {
    error = diagnostic;
    return false;
  }
#ifdef _WIN32
  if (m->mappedCode) {
    DWORD previousProtection = 0;
    if (!VirtualProtect(m->mappedCode, m->bytes.size(), PAGE_EXECUTE_READ,
                        &previousProtection) ||
        !FlushInstructionCache(GetCurrentProcess(), m->mappedCode,
                               m->bytes.size())) {
      error = "Unable to protect Creator code memory";
      return false;
    }
  }
#endif
  m->instance = wasm_runtime_instantiate(m->module, 65536, 0, diagnostic,
                                         sizeof(diagnostic));
  if (!m->instance) {
    error = diagnostic;
    return false;
  }
  m->env = wasm_runtime_create_exec_env(m->instance, 65536);
  if (!m->env) {
    error = "Unable to prepare C++ execution stack";
    return false;
  }
  wasm_runtime_set_custom_data(m->instance, &m->owner);
  const auto lookup = [&](const char *name) {
    return wasm_runtime_lookup_function(m->instance, name);
  };
  m->process = lookup("vlt_process");
  m->reset = lookup("vlt_reset");
  m->budget = lookup("__vlt_set_budget");
  auto init = lookup("vlt_init"), heap = lookup("vlt_heap_base"),
       table = lookup("vlt_table"), abi = lookup("vlt_abi");
  if (!init || !heap || !table || !abi || !m->process || !m->reset ||
      !m->budget) {
    error = "Incomplete Creator ABI";
    return false;
  }
  std::uint32_t args[16]{};
  if (!m->fuel(50000000) || !m->call(abi, 0, args) || args[0] != kCodeAbi) {
    error = "Unsupported Creator code ABI";
    return false;
  }
  if (!m->call(heap, 0, args)) {
    error = m->failure.data();
    return false;
  }
  m->owner.next = args[0];
  m->owner.preparing = true;
  auto *cursor = args;
  pack(cursor, info.sampleRate);
  pack(cursor, std::uint32_t(channels));
  pack(cursor, std::uint32_t(info.maxBlockSize));
  pack(cursor, seed);
  const bool initialized = m->call(init, unsigned(cursor - args), args);
  m->owner.preparing = false;
  if (!initialized || !m->call(table, 0, args)) {
    error = m->failure.data();
    return false;
  }
  if (!wasm_runtime_validate_app_addr(m->instance, args[0], sizeof(m->slots))) {
    error = "Invalid C++ output table";
    return false;
  }
  std::memcpy(m->slots.data(),
              wasm_runtime_addr_app_to_native(m->instance, args[0]),
              sizeof(m->slots));
  m->maxFrames = info.maxBlockSize;
  const auto spanBytes = std::uint64_t(info.maxBlockSize) * 8;
  if (spanBytes > kCodeMemoryLimit) {
    error = "C++ block size exceeds its memory limit";
    return false;
  }
  for (unsigned i = 0; i < d.nodes.size(); ++i)
    if (d.nodes[i].function) {
      const auto &f = *d.nodes[i].function;
      // Function-only nodes have no scheduled slot and own state through
      // callers.
      if (!m->slots[i].outputs[0])
        continue;
      m->outputCounts[i] = unsigned(f.outputs.size());
      if (m->slots[i].latency > unsigned(info.sampleRate * 30) ||
          m->slots[i].tail > unsigned(info.sampleRate * 120)) {
        error = "C++ latency or tail exceeds its limit";
        return false;
      }
      const auto pointer = [&](unsigned offset) -> float * {
        return offset && offset % 4 == 0 &&
                       wasm_runtime_validate_app_addr(m->instance, offset,
                                                      unsigned(spanBytes))
                   ? static_cast<float *>(
                         wasm_runtime_addr_app_to_native(m->instance, offset))
                   : nullptr;
      };
      for (unsigned p = 0; p < f.inputs.size(); ++p)
        if (f.inputs[p].type != "function") {
          m->inputs[i][p] = pointer(m->slots[i].inputs[p]);
          if (!m->inputs[i][p]) {
            error = "Invalid C++ input buffer";
            return false;
          }
        }
      for (unsigned p = 0; p < f.outputs.size(); ++p) {
        m->outputs[i][p] = pointer(m->slots[i].outputs[p]);
        if (!m->outputs[i][p]) {
          error = "Invalid C++ output buffer";
          return false;
        }
      }
    }
  const auto memory = wasm_runtime_get_default_memory(m->instance);
  const auto size = std::uint64_t(wasm_memory_get_cur_page_count(memory)) *
                    wasm_memory_get_bytes_per_page(memory);
  if (size > kCodeMemoryLimit) {
    error = "C++ memory limit exceeded";
    return false;
  }
  m->memory = static_cast<std::uint8_t *>(wasm_memory_get_base_address(memory));
  m->snapshot.assign(m->memory, m->memory + size);
  // Traps can skip the epilogue that restores Clang's linear-memory stack.
  // Keep its initial global separately; it is not part of the RAM snapshot.
  wasm_global_inst_t stack{};
  if (wasm_runtime_get_export_global_inst(m->instance, "__stack_pointer",
                                          &stack) &&
      stack.kind == WASM_I32 && stack.is_mutable && stack.global_data) {
    m->stackPointer = stack.global_data;
    std::memcpy(&m->initialStackPointer, stack.global_data,
                sizeof(std::uint32_t));
  }
  // Touch every prepared page before an audio worker accesses it.
  for (std::size_t i = 0; i < m->snapshot.size(); i += 4096)
    m->memory[i] = m->snapshot[i];
  return true;
}
float *WasmProgram::input(unsigned n, unsigned p) noexcept {
  return m->inputs[n][p];
}
const float *WasmProgram::output(unsigned n, unsigned p) const noexcept {
  return m->outputs[n][p];
}
unsigned WasmProgram::latency(unsigned n) const noexcept {
  return m->slots[n].latency;
}
unsigned WasmProgram::tail(unsigned n) const noexcept {
  return m->slots[n].tail;
}
bool WasmProgram::failed() const noexcept { return m->broken; }
std::string WasmProgram::error() const { return m->failure.data(); }
bool WasmProgram::takeError(std::string &message) {
  std::array<char, 256> error{};
  if (!m->diagnostics.pop(error))
    return false;
  message = error.data();
  return true;
}
unsigned WasmProgram::realtimeMemoryOperations() noexcept {
  return realtimeMemory.load();
}
bool WasmProgram::render(unsigned node,
                         const engine::ProcessContext &context) noexcept {
  if (m->broken || context.frames > m->maxFrames || node >= 64)
    return false;
  RealtimeScope realtimeScope;
  if (!m->fuel(std::max<std::uint64_t>(100000,
                                       std::uint64_t(context.frames) * 20000)))
    return false;
  std::uint32_t args[16]{}, *cursor = args;
  pack(cursor, std::uint32_t(node));
  pack(cursor, std::uint32_t(context.frames));
  pack(cursor, context.transport.ppqPosition);
  pack(cursor, context.transport.tempo);
  pack(cursor, std::int64_t(context.timelinePosition));
  pack(cursor, std::uint32_t(context.playing));
  if (!m->call(m->process, unsigned(cursor - args), args))
    return false;
  for (unsigned p = 0; p < m->outputCounts[node]; ++p)
    for (unsigned i = 0; i < context.frames * 2; ++i)
      if (!std::isfinite(m->outputs[node][p][i])) {
        m->broken = true;
        std::memcpy(m->failure.data(), "C++ function produced NaN or infinity",
                    37);
        m->diagnostics.push(m->failure);
        return false;
      }
  return true;
}
void WasmProgram::reset() noexcept {
  if (!m->memory)
    return;
  RealtimeScope realtimeScope;
  std::memcpy(m->memory, m->snapshot.data(), m->snapshot.size());
  if (m->stackPointer)
    std::memcpy(m->stackPointer, &m->initialStackPointer,
                sizeof(std::uint32_t));
  wasm_runtime_set_exception(m->instance, nullptr);
  m->broken = false;
  m->failure.fill(0);
  std::uint32_t args[2]{};
  if (m->fuel(50000000))
    m->call(m->reset, 0, args);
}
} // namespace daw::plugins::mini
