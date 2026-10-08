#pragma once

#include <cstdint>
#include <memory>

namespace daw {

/// Immutable input to an offline runtime. Retains document, native plugin
/// states, queued parameter edits, catalogue and immutable decoded audio.
/// It can outlive its source controller and be rendered repeatedly. External
/// files/libraries referenced by plugins must still exist. Local sample handles
/// are shared ownership, not an IPC wire representation.
class RenderSessionSpec final {
public:
    bool valid() const noexcept { return bool(m_data); }
    std::uint64_t sourceRevision() const noexcept;
    std::uint64_t sourceGeneration() const noexcept;

private:
    friend class EngineController;
    friend class RenderWorker;
    struct Data;
    std::shared_ptr<const Data> m_data;
};

} // namespace daw
