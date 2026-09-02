#pragma once

#include <string_view>

namespace daw::collab {

enum class MutationCapability {
    SharedCommand,
    LocalOnly,
    BlockedV1,
    Unclassified,
};

struct MutationCapabilityEntry {
    std::string_view method;
    MutationCapability capability = MutationCapability::Unclassified;
};

inline constexpr MutationCapabilityEntry kMutationCapabilityLedger[] = {
#define VLT_MUTATION(method, capability) \
    {#method, MutationCapability::capability},
#include "MutationCapabilityLedger.def"
#undef VLT_MUTATION
};

constexpr bool mutationCapabilityLedgerIsClassified() noexcept {
    for (const MutationCapabilityEntry& entry : kMutationCapabilityLedger) {
        if (entry.method.empty() ||
            entry.capability == MutationCapability::Unclassified) {
            return false;
        }
    }
    return true;
}

static_assert(mutationCapabilityLedgerIsClassified());

} // namespace daw::collab
