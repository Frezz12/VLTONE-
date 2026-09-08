#include "AudioMusicalAnalysis.hpp"
#include "analysis/NeuralModels.hpp"
#include "analysis/Calibration.hpp"
#include "ProjectSerializer.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <chrono>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>

namespace a = daw::analysis;
int failures = 0;
void check(bool value, const char* message) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", message);
    failures += !value;
}
std::vector<float> floats(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    std::vector<float> data(std::size_t(file.tellg()) / sizeof(float));
    file.seekg(0); file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size() * sizeof(float)));
    return data;
}

void calibrationContract() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const char* previous = std::getenv("VLT_AUDIO_ANALYSIS_MODELS");
    const bool hadOverride = previous != nullptr;
    const std::string saved = previous ? previous : "";
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = fs::temp_directory_path() / ("analysis-calibration-" + std::to_string(nonce));
    fs::create_directories(directory);
    const auto environment = [](const std::string& value, bool present) {
#if defined(_WIN32)
        _putenv_s("VLT_AUDIO_ANALYSIS_MODELS", present ? value.c_str() : "");
#else
        if (present) setenv("VLT_AUDIO_ANALYSIS_MODELS", value.c_str(), 1);
        else unsetenv("VLT_AUDIO_ANALYSIS_MODELS");
#endif
    };
    environment(directory.string(), true);
    const json hashes{{"synthetic-model", "fixture"}};
    std::ofstream(directory / "manifest.json") << json{{"sha256", hashes}};
    json config{{"algorithmVersion", 2}, {"modelSha256", hashes},
        {"backends", {{"tempo:dsp", {{"validated", true},
            {"knots", {{0.2, 0.5}, {0.8, 0.99}}}}}}}};
    const auto write = [&] { std::ofstream(directory / "calibration.json") << config; };
    write();
    a::TempoEstimate tempo;
    tempo.bpm=128;tempo.status=a::DetectionStatus::Available;tempo.stability=1;tempo.evidence=.9;
    a::detail::calibrate(tempo);
    check(tempo.calibrated && tempo.confidence==.99 && tempo.highConfidence(),
          "validated matching calibration supplies the runtime confidence probability");
    tempo.evidence=.1;a::detail::calibrate(tempo);
    check(tempo.calibrated && tempo.confidence==0 && !tempo.highConfidence(),
          "evidence below the first calibration knot has zero probability, matching the release evaluator");
    config["modelSha256"]["synthetic-model"]="different";write();
    a::detail::calibrate(tempo);
    check(!tempo.calibrated, "calibration for different model assets is rejected");
    config["modelSha256"]=hashes;
    config["backends"]["tempo:dsp"]["knots"]={{0, .9}, {.8, .5}};write();
    a::detail::calibrate(tempo);
    check(!tempo.calibrated, "non-monotonic calibration cannot authorize application");
    environment(saved, hadOverride);
    std::error_code error;fs::remove_all(directory,error);
}
int main() {
    calibrationContract();
    check(a::roundedBpm(127.86) == 128 && a::roundedBpm(128.5) == 129 &&
          a::roundedBpm(128.49) == 128 && a::roundedBpm(-1) == 0 &&
          a::roundedBpm(std::numeric_limits<double>::quiet_NaN()) == 0,
          "one whole-number rounding policy, including invalid inputs");
    a::TempoEstimate t;
    t.status = a::DetectionStatus::Available; t.bpm = 127.86;
    t.alternatives = {128.1,64.3,63.9,256};
    check(a::applicableTempos(t) == std::vector<int>({128,64,256}), "rounded alternatives are unique and preserve primary order");
    t.confidence = 1; t.stability = 1;
    check(!t.highConfidence(), "uncalibrated scores never authorize automatic application");
    t.calibrated = true;
    check(t.highConfidence(), "calibrated confident tempo can be applied");
    t.variable = true;
    check(!t.highConfidence(), "variable tempo requires a choice");
    const auto input = floats(std::filesystem::path(DAW_ANALYSIS_FIXTURES) / "audio.f32");
    const auto expected = floats(std::filesystem::path(DAW_ANALYSIS_FIXTURES) / "mel.f32");
    const auto actual = a::detail::beatLogMel(input, {});
    double error = 0;
    if (actual.size() == expected.size()) for (std::size_t i = 0; i < actual.size(); ++i) error = std::max(error, std::abs(double(actual[i]) - expected[i]));
    check(!actual.empty() && actual.size() == expected.size() && error < 3e-4, "native log-mel matches upstream Torch at centered edges and every bin");
    std::printf("log-mel maximum error %.9f\n",error);
    constexpr double rate = 22050;
    std::vector<float> pulse(std::size_t(rate * 8));
    for (double at = .13; at < 8; at += 60.0 / 128.0)
        for (int i=0; i<700 && std::size_t(at*rate+i)<pulse.size(); ++i)
            pulse[std::size_t(at*rate)+i] += float(std::exp(-i/90.0) * std::sin(i*1.7));
    a::MusicalAnalysisRequest request;
    request.useNeuralModels = false; request.detectKey = false;
    a::MusicalAnalysisResult mono, stereo, renamed;
    double last = -1; bool monotonic = true;
    check(bool(a::analyzeAudioSamples(pulse.data(),pulse.size(),1,rate,request,mono,[&](double p,std::string_view) {
        monotonic = monotonic && p >= last && p >= 0 && p <= 1; last=p; return true;
    })) && monotonic && last==1, "analysis progress is monotonic and completes");
    std::vector<float> anti(pulse.size()*2);
    for (std::size_t i=0;i<pulse.size();++i) { anti[2*i]=pulse[i]; anti[2*i+1]=-pulse[i]; }
    a::analyzeAudioSamples(anti.data(),pulse.size(),2,rate,request,stereo);
    check(std::abs(mono.tempo.bpm-stereo.tempo.bpm)<1e-8, "anti-phase stereo retains the mono rhythm without switching channels");
    for (auto& sample : anti) sample += .3f;
    a::analyzeAudioSamples(anti.data(),pulse.size(),2,rate,request,stereo);
    check(std::abs(mono.tempo.bpm-stereo.tempo.bpm)<.05, "DC offset does not hide anti-phase cancellation");
    request.fileNameHint="WRONG_77BPM_F_SHARP_MINOR.wav";
    a::analyzeAudioSamples(pulse.data(),pulse.size(),1,rate,request,renamed);
    check(mono.tempo.bpm==renamed.tempo.bpm, "filenames cannot influence audio inference");
    for (double fileRate : {rate, 44100.0, 48000.0}) {
        namespace fs = std::filesystem;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto file = fs::temp_directory_path() / ("analysis-range-" + std::to_string(nonce) + ".wav");
        std::vector<float> padded(std::size_t(fileRate * 9), 0.0f);
        for (std::size_t i=0; i<std::size_t(fileRate * 8); ++i) {
            const double source = i * rate / fileRate;
            const auto at = std::min(std::size_t(source), pulse.size()-1);
            const double fraction = source - at;
            padded[std::size_t(fileRate)+i] = float(pulse[at] * (1-fraction) +
                pulse[std::min(at+1,pulse.size()-1)] * fraction);
        }
        audio::platform::AudioFileWriter writer;
        const float* channel = padded.data();
        const bool written = writer.open(file.string(),fileRate,1,padded.size()).isOk() &&
            writer.write(&channel,padded.size()).isOk() && writer.close().isOk();
        request.offsetSeconds=1;request.durationSeconds=8;
        a::MusicalAnalysisResult decoded;
        const auto result = a::analyzeAudioFile(file.string(),request,decoded);
        check(written && result.isOk() && std::abs(decoded.analyzedSeconds-8)<1e-9 &&
              std::abs(decoded.tempo.bpm-mono.tempo.bpm)<(fileRate==rate ? 1e-8 : .05),
              "22.05/44.1/48 kHz decoding preserves source offsets and grid timing across cache boundaries");
        request.offsetSeconds=0;request.durationSeconds=0;
        std::error_code error;fs::remove(file,error);
    }
    a::MusicalAnalysisResult cancelled;
    check(!a::analyzeAudioSamples(pulse.data(),pulse.size(),1,rate,request,cancelled,[](double p,std::string_view){return p<.8;}) &&
          cancelled.tempo.status==a::DetectionStatus::Unavailable, "cancellation discards partial results");
#if DAW_TEST_NEURAL
    request.useNeuralModels = true;
    bool reachedModel = false;
    const auto modelCancelled = a::analyzeAudioSamples(pulse.data(),pulse.size(),1,rate,request,cancelled,
        [&](double,std::string_view phase) {
            if (phase=="model_inference") { reachedModel=true; return false; }
            return true;
        });
    check(reachedModel && !modelCancelled && cancelled.tempo.status==a::DetectionStatus::Unavailable,
          "ONNX inference can be cancelled without retaining partial output");
    request.useNeuralModels = false;
#endif
    request.detectTempo=false;request.detectKey=true;
    std::vector<float> bass(std::size_t(rate*4));
    for (std::size_t i=0;i<bass.size();++i) for (int harmonic=1;harmonic<=8;++harmonic)
        bass[i]+=float(.2/harmonic*std::sin(2*std::numbers::pi*55*harmonic*i/rate));
    a::MusicalAnalysisResult unknown;
    a::analyzeAudioSamples(bass.data(),bass.size(),1,rate,request,unknown);
    check(unknown.key.status==a::DetectionStatus::Unavailable, "one bass note with harmonics does not establish a key");
    std::mt19937 generator(1928);std::normal_distribution<float> noise(0,.1);
    for (auto& sample:bass) sample=noise(generator);
    a::analyzeAudioSamples(bass.data(),bass.size(),1,rate,request,unknown);
    check(unknown.key.status==a::DetectionStatus::Unavailable, "noise does not establish a key");
    a::analyzeAudioSamples(pulse.data(),pulse.size(),1,rate,request,unknown);
    check(unknown.key.status==a::DetectionStatus::Unavailable, "percussion alone does not establish a key");
    bass[0]=std::numeric_limits<float>::infinity();
    check(!a::analyzeAudioSamples(bass.data(),bass.size(),1,rate,request,unknown), "non-finite input is rejected before FFT or inference");
    request.detectTempo=false; request.detectKey=false;
    check(a::toClipAnalysisModel({},request).empty(), "skipped analysis is not stamped as a new algorithm result");
    request.detectTempo=true;
    check(a::toClipAnalysisModel({},request).key.algorithmVersion==0, "tempo-only analysis does not update the key version");
    return failures ? 1 : 0;
}
