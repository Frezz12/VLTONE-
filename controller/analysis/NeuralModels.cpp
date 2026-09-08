#include "NeuralModels.hpp"
#include "Signal.hpp"
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#if DAW_HAS_ONNX_ANALYSIS
#include <onnxruntime_cxx_api.h>
#endif

namespace daw::analysis::detail {
namespace fs = std::filesystem;
fs::path analysisAssetDirectory() {
    if (const char* path = std::getenv("VLT_AUDIO_ANALYSIS_MODELS")) return fs::u8path(path);
    fs::path executable;
#if defined(__APPLE__)
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> buffer(length);
    if (_NSGetExecutablePath(buffer.data(), &length) == 0) executable = fs::path(buffer.data());
#elif defined(_WIN32)
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length > 0 && length < buffer.size()) executable = fs::path(std::wstring_view(buffer.data(), length));
#else
    std::error_code ec;
    executable = fs::read_symlink("/proc/self/exe", ec);
#endif
    std::error_code error;
    const auto directory = fs::weakly_canonical(executable, error).parent_path();
    for (const auto& candidate : {directory / "audio-analysis", directory.parent_path() / "Resources/audio-analysis"})
        if (fs::is_regular_file(candidate / "manifest.json", error)) return candidate;
#ifdef DAW_ANALYSIS_DEV_MODELS
    return fs::path(DAW_ANALYSIS_DEV_MODELS);
#else
    return directory / "audio-analysis";
#endif
}

std::vector<float> beatLogMel(std::span<const float> mono, const AnalysisProgress& progress) {
    constexpr std::size_t n = 1024, hop = 441, melBins = 128;
    if (mono.size() < 2) return {};
    std::ifstream file(analysisAssetDirectory() / "mel-filterbank.bin", std::ios::binary);
    std::vector<float> filter((n / 2 + 1) * melBins);
    if (!file.read(reinterpret_cast<char*>(filter.data()), std::streamsize(filter.size() * sizeof(float)))) return {};
    const std::size_t frames = 1 + mono.size() / hop;
    std::vector<float> result(frames * melBins);
    std::vector<std::complex<double>> bins(n);
    std::array<double, n> window;
    for (std::size_t i = 0; i < n; ++i) window[i] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * double(i) / n);
    const auto reflected = [&](std::ptrdiff_t i) {
        const auto last = std::ptrdiff_t(mono.size()) - 1;
        while (i < 0 || i > last) { if (i < 0) i = -i; if (i > last) i = 2 * last - i; }
        return mono[std::size_t(i)];
    };
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t i = 0; i < n; ++i) bins[i] = reflected(std::ptrdiff_t(frame * hop + i) - std::ptrdiff_t(n / 2)) * window[i];
        fft(bins);
        std::array<double, melBins> mel{};
        for (std::size_t k = 0; k <= n / 2; ++k) {
            const double magnitude = std::abs(bins[k]) / std::sqrt(double(n));
            for (std::size_t b = 0; b < melBins; ++b) mel[b] += magnitude * filter[k * melBins + b];
        }
        for (std::size_t b = 0; b < melBins; ++b) result[frame * melBins + b] = float(std::log1p(1000 * mel[b]));
        if ((frame & 63u) == 0) checkpoint(progress, double(frame) / frames, "tempo_model");
    }
    return result;
}

#if DAW_HAS_ONNX_ANALYSIS
namespace {
struct Sessions {
    Ort::Env environment{ORT_LOGGING_LEVEL_ERROR, "VLTONE musical analysis"};
    std::mutex mutex;
    fs::path directory;
    std::shared_ptr<Ort::Session> beat, key;
    bool triedBeat = false, triedKey = false;
    std::shared_ptr<Ort::Session> get(bool isBeat) {
        std::lock_guard lock(mutex);
        const auto path = analysisAssetDirectory();
        if (directory != path) { beat.reset(); key.reset(); triedBeat = triedKey = false; directory = path; }
        auto& session = isBeat ? beat : key;
        auto& tried = isBeat ? triedBeat : triedKey;
        if (!tried) {
            tried = true;
            try {
                Ort::SessionOptions options;
                options.SetIntraOpNumThreads(2);
                options.SetInterOpNumThreads(1);
                options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                options.AddConfigEntry("session.intra_op.allow_spinning", "0");
                options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
                const auto model = directory / (isBeat ? "beat-this-final0.onnx" : "skey.onnx");
                std::error_code error;
                if (fs::is_regular_file(model, error)) session = std::make_shared<Ort::Session>(environment, model.c_str(), options);
            } catch (const Ort::Exception&) { session.reset(); }
        }
        return session;
    }
};
Sessions& sessions() { static Sessions instance; return instance; }

std::vector<Ort::Value> run(Ort::Session& session, const char* inputName, Ort::Value& input,
                            std::span<const char* const> outputNames, const AnalysisProgress& progress) {
    Ort::RunOptions options;
    auto future = std::async(std::launch::async, [&] {
        return session.Run(options, &inputName, &input, 1, outputNames.data(), outputNames.size());
    });
    while (future.wait_for(std::chrono::milliseconds(40)) != std::future_status::ready) {
        if (progress && !progress(0, "model_inference")) {
            options.SetTerminate();
            try { future.get(); } catch (const Ort::Exception&) {}
            throw Cancelled{};
        }
    }
    checkpoint(progress, 1, "model_inference");
    return future.get();
}
}
#endif

BeatActivations neuralBeats(std::span<const float> mono, const AnalysisProgress& progress) {
    BeatActivations result;
#if DAW_HAS_ONNX_ANALYSIS
    checkpoint(progress, 0, "tempo_model");
    auto session = sessions().get(true);
    if (!session) return result;
    try {
        const auto spect = beatLogMel(mono, subProgress(progress, 0, 0.1));
        if (spect.empty()) return result;
        constexpr std::ptrdiff_t chunk = 1500, border = 6;
        const auto frames = std::ptrdiff_t(spect.size() / 128);
        result.beats.assign(std::size_t(frames), 0);
        result.downbeats.assign(std::size_t(frames), 0);
        std::vector<std::ptrdiff_t> starts;
        for (std::ptrdiff_t start = -border; start < frames - border; start += chunk - 2 * border) starts.push_back(start);
        if (frames > chunk - 2 * border) starts.back() = frames - (chunk - border);
        std::vector<bool> written(std::size_t(frames), false);
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        constexpr std::array<const char*, 2> outputs = {"beat", "downbeat"};
        for (std::size_t segment = 0; segment < starts.size(); ++segment) {
            const auto start = starts[segment];
            const auto count = std::min(chunk, frames - start + border);
            std::vector<float> input(std::size_t(count) * 128, 0);
            for (auto frame = std::max<std::ptrdiff_t>(0, start); frame < std::min(frames, start + count); ++frame)
                std::copy_n(spect.data() + frame * 128, 128, input.data() + (frame - start) * 128);
            const std::array<std::int64_t, 3> shape = {1, count, 128};
            auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
            auto prediction = run(*session, "spect", tensor, outputs,
                subProgress(progress, 0.1 + 0.9 * double(segment) / starts.size(), 0.1 + 0.9 * double(segment + 1) / starts.size()));
            if (prediction.size() != 2 || prediction[0].GetTensorTypeAndShapeInfo().GetElementCount() != std::size_t(count) ||
                prediction[1].GetTensorTypeAndShapeInfo().GetElementCount() != std::size_t(count)) return {};
            const float* beat = prediction[0].GetTensorData<float>();
            const float* downbeat = prediction[1].GetTensorData<float>();
            for (auto frame = border; frame < count - border; ++frame) {
                const auto at = frame + start;
                if (at < 0 || at >= frames || written[std::size_t(at)]) continue;
                if (!std::isfinite(beat[frame]) || !std::isfinite(downbeat[frame])) return {};
                result.beats[std::size_t(at)] = 1 / (1 + std::exp(-double(beat[frame])));
                result.downbeats[std::size_t(at)] = 1 / (1 + std::exp(-double(downbeat[frame])));
                written[std::size_t(at)] = true; // upstream keep_first overlap policy
            }
        }
        checkpoint(progress, 1, "tempo_model");
    } catch (const Ort::Exception&) { return {}; }
#endif
    return result;
}

std::vector<double> neuralKey(std::span<const float> mono, const AnalysisProgress& progress) {
#if DAW_HAS_ONNX_ANALYSIS
    if (mono.size() < 3 * 22050) return {};
    checkpoint(progress, 0, "key_model");
    auto session = sessions().get(false);
    if (!session) return {};
    try {
        float peak = 0;
        for (float sample : mono) peak = std::max(peak, std::abs(sample));
        if (peak < 1e-7) return {};
        std::vector<float> input(mono.size());
        for (std::size_t i = 0; i < mono.size(); ++i) input[i] = mono[i] / peak;
        const std::array<std::int64_t, 2> shape = {1, std::int64_t(input.size())};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        constexpr std::array<const char*, 1> outputs = {"probs"};
        auto prediction = run(*session, "audio", tensor, outputs, progress);
        if (prediction[0].GetTensorTypeAndShapeInfo().GetElementCount() != 24) return {};
        const auto* probs = prediction[0].GetTensorData<float>();
        std::vector<double> result(24);
        // Upstream key_map: A major first, B minor first. Not C major/C minor!
        double sum = 0;
        for (int i = 0; i < 24; ++i) {
            if (!std::isfinite(probs[i]) || probs[i] < 0 || probs[i] > 1) return {};
            const int root = i < 12 ? (i + 9) % 12 : (i - 12 + 11) % 12;
            result[(i < 12 ? 0 : 12) + root] = probs[i]; sum += probs[i];
        }
        if (std::abs(sum - 1.0) > 0.01) return {};
        return result;
    } catch (const Ort::Exception&) { return {}; }
#else
    return {};
#endif
}
} // namespace daw::analysis::detail
