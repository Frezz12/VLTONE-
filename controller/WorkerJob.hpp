#pragma once

#include "Core/Result.hpp"
#include "RenderSpec.hpp"
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>

namespace daw {

// One private directory and disposable child per job. The transport, deadlines,
// cancellation and resource lifetime are shared by rendering and media analysis.
class WorkerJob final {
public:
    using Progress = std::function<bool(const rendering::Progress&)>;
    using Handler = std::function<nlohmann::json(const std::filesystem::path&, const Progress&)>;
    explicit WorkerJob(const std::filesystem::path& parent);
    ~WorkerJob();
    WorkerJob(const WorkerJob&) = delete;
    WorkerJob& operator=(const WorkerJob&) = delete;
    std::filesystem::path root;
    audio::Result run(const std::string& executable, const Progress& progress,
        nlohmann::json& reply, bool& cancelled);
    static int serve(int argc, char** argv, const Handler& handler);
};

} // namespace daw
