#pragma once

#include "RenderSessionSpec.hpp"
#include "RenderSpec.hpp"
#include "Core/Result.hpp"
#include <functional>
#include <string>

namespace daw {
namespace analysis { struct Metrics; }

/// One disposable process per offline session. Only the parent publishes
/// finished outputs; a crashed or cancelled worker owns no user destination.
class RenderWorker final {
public:
    static audio::Result render(const RenderSessionSpec& session,
        const std::function<bool(const rendering::Progress&)>& progress,
        rendering::Report& out, const std::string& executable = {});
    static audio::Result exportMixdown(const RenderSessionSpec& session,
        const std::string& outputPath, bool normalize);
    static audio::Result analyze(const RenderSessionSpec& session, analysis::Metrics& out);
    static int main(int argc, char** argv);
private:
    struct Options {
        std::string exactOutput;
        bool normalize = false;
        analysis::Metrics* metrics = nullptr;
    };
    static audio::Result run(const RenderSessionSpec& session,
        const std::function<bool(const rendering::Progress&)>& progress,
        rendering::Report& out, const Options& options, const std::string& executable = {});
};

} // namespace daw
