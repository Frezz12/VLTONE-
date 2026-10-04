#pragma once
#include <cstdint>
namespace daw::plugins::mini {
// All pointers in this ABI are checked offsets into Wasm linear memory.
inline constexpr unsigned kCodeAbi = 1, kCodeMemoryLimit = 16 * 1024 * 1024;
struct CodeSlot {
  std::uint32_t inputs[16]{}, outputs[16]{};
  std::uint32_t latency = 0, tail = 0;
};
static_assert(sizeof(CodeSlot) == 136);
} // namespace daw::plugins::mini
