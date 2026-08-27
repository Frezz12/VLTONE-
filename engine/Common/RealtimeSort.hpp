#pragma once

#include <iterator>
#include <utility>

namespace daw::engine {

/// Stable in-place ordering for small bounded realtime event buffers.
/// Unlike std::stable_sort this never requests temporary heap storage.
template <typename RandomIt, typename Compare>
void stableRealtimeSort(RandomIt first, RandomIt last, Compare compare) noexcept {
    if (first == last) return;
    for (RandomIt current = first + 1; current != last; ++current) {
        auto value = std::move(*current);
        RandomIt insertion = current;
        while (insertion != first && compare(value, *(insertion - 1))) {
            *insertion = std::move(*(insertion - 1));
            --insertion;
        }
        *insertion = std::move(value);
    }
}

} // namespace daw::engine
