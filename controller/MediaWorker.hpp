#pragma once

#include "AudioMusicalAnalysis.hpp"
#include "SamplePitchAnalysis.hpp"
#include "SliceAnalysis.hpp"
#include "WarpAnalysis.hpp"
#include "WorkerJob.hpp"
#include "platform/AudioFileDecoder.hpp"

namespace daw {

// Installed by the application before starting service threads.
// Worker and unit-test executables leave the native algorithms unredirected.
class MediaWorker final {
public:
    static void install(const std::string& executable);
    static bool enabled() noexcept;
    static audio::Result probe(const std::string& path, audio::platform::AudioFileInfo& out);
    static audio::Result decode(const std::string& path, std::shared_ptr<const engine::SampleBuffer>& out,
        const std::function<bool()>& keepGoing = {});
    static audio::Result analyze(const std::string& path, const analysis::MusicalAnalysisRequest& request,
        analysis::MusicalAnalysisResult& out, const analysis::AnalysisProgress& progress);
    static std::vector<analysis::WarpTransient> transients(const engine::SampleBuffer& audio,
        double begin, double end, const std::function<bool()>& keepGoing);
    static analysis::SamplePitchEstimate pitch(const engine::SampleBuffer& audio,
        engine::FrameCount first, engine::FrameCount end, const std::function<bool()>& keepGoing);
    static plugins::slicer::SliceTable slice(const engine::SampleBuffer& audio,
        const slicer::SliceSettings& settings, const std::function<bool()>& keepGoing);
    static nlohmann::json execute(const std::filesystem::path& root, const WorkerJob::Progress& progress);
};

} // namespace daw
