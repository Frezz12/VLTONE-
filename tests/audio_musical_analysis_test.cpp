#include "AudioMusicalAnalysis.hpp"
#include "ProjectSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <numbers>
#include <string>
#include <vector>

namespace analysis = daw::analysis;

static int failures = 0;
static bool check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) ++failures;
    return condition;
}

static void addTone(std::vector<float>& audio, double rate, double from,
                    double duration, double hz, double amplitude) {
    const std::size_t first = std::size_t(from * rate);
    const std::size_t count = std::min(audio.size() - std::min(first, audio.size()),
                                       std::size_t(duration * rate));
    for (std::size_t i = 0; i < count; ++i) {
        const double envelope = std::min(1.0, double(i) / (rate * 0.01)) *
                                std::min(1.0, double(count - i) / (rate * 0.03));
        audio[first + i] += float(amplitude * envelope *
            std::sin(2.0 * std::numbers::pi * hz * double(i) / rate));
    }
}

static std::vector<float> musicalBeat(double bpm, int root,
                                      bool minor, double seconds = 18.0) {
    constexpr double rate = 22050.0;
    std::vector<float> audio(std::size_t(seconds * rate), 0.0f);
    const double beat = 60.0 / bpm;
    const int third = minor ? 3 : 4;
    const int progression[] = {0, 5, 7, 0};
    for (int index = 0; double(index) * beat < seconds; ++index) {
        const double at = index * beat;
        // Short broadband-ish click plus a low kick. Alternating accent gives
        // the detector evidence for the tactus rather than only subdivisions.
        const std::size_t first = std::size_t(at * rate);
        for (std::size_t i = 0; i < 900 && first + i < audio.size(); ++i) {
            const double decay = std::exp(-double(i) / 150.0);
            const double kick = std::sin(2.0 * std::numbers::pi * 70.0 * i / rate);
            const double click = (i % 17 < 7 ? 1.0 : -1.0);
            audio[first + i] += float(decay * (0.35 * kick +
                (index % 4 == 0 ? 0.28 : 0.12) * click));
        }
        if ((index & 1) != 0) continue;
        const int chordRoot = root + progression[(index / 8) % 4];
        for (int semitone : {0, third, 7}) {
            const double hz = 220.0 * std::pow(2.0, (chordRoot + semitone - 9) / 12.0);
            addTone(audio, rate, at, beat * 2.0, hz, 0.10);
            addTone(audio, rate, at, beat * 2.0, hz * 2.0, 0.035);
        }
    }
    return audio;
}

int main() {
    constexpr double rate = 22050.0;
    {
        auto audio = musicalBeat(128.0, 0, false);
        analysis::MusicalAnalysisResult result;
        analysis::MusicalAnalysisRequest request;
        const auto status = analysis::analyzeAudioSamples(
            audio.data(), audio.size(), 1, rate, request, result);
        std::printf("      detected %.1f BPM conf %.3f, key %s conf %.3f\n",
                    result.tempo.bpm, result.tempo.confidence,
                    analysis::keyDisplayName(result.key).c_str(),
                    result.key.confidence);
        check(bool(status), "combined analysis succeeds");
        check(std::abs(result.tempo.bpm - 128.0) < 2.0,
              "steady beat is detected at the primary metrical level");
        check(result.tempo.status != analysis::DetectionStatus::Unavailable,
              "steady beat returns a tempo result");
        check(result.key.root == 0 && result.key.scale == "major",
              "C-major progression is identified");
    }
    {
        auto audio = musicalBeat(92.0, 9, true);
        analysis::MusicalAnalysisResult result;
        analysis::MusicalAnalysisRequest request;
        request.pitchShiftSemitones = 2.0;
        request.stretchTime = 2.0;
        analysis::analyzeAudioSamples(audio.data(), audio.size(), 1, rate,
                                      request, result);
        std::printf("      transformed %.1f BPM, key %s\n", result.tempo.bpm,
                    analysis::keyDisplayName(result.key).c_str());
        check(std::abs(result.tempo.bpm - 46.0) < 2.0,
              "playback stretch is reflected in detected BPM");
        check(result.key.root == 11 && result.key.scale == "natural_minor",
              "pitch shift is reflected in detected key");
    }
    {
        bool coveredRange = true;
        for (double expected : {72.0, 92.0, 110.0, 140.0, 174.0}) {
            auto audio = musicalBeat(expected, 0, false, 12.0);
            analysis::MusicalAnalysisResult result;
            analysis::MusicalAnalysisRequest request;
            request.detectKey = false;
            analysis::analyzeAudioSamples(audio.data(), audio.size(), 1, rate,
                                          request, result);
            std::printf("      range %.0f -> %.1f BPM (%.3f)\n", expected,
                        result.tempo.bpm, result.tempo.confidence);
            coveredRange = coveredRange &&
                std::abs(result.tempo.bpm - expected) < 2.5;
        }
        check(coveredRange, "tempo detector covers slow through fast beat loops");
    }
    {
        bool coveredKeys = true;
        struct ExpectedKey { int root; bool minor; };
        for (const ExpectedKey expected :
             {ExpectedKey{2, false}, ExpectedKey{6, false},
              ExpectedKey{0, true}, ExpectedKey{5, true}}) {
            auto audio = musicalBeat(110.0, expected.root, expected.minor, 12.0);
            analysis::MusicalAnalysisResult result;
            analysis::MusicalAnalysisRequest request;
            request.detectTempo = false;
            analysis::analyzeAudioSamples(audio.data(), audio.size(), 1, rate,
                                          request, result);
            std::printf("      key %s %s -> %s (%.3f)\n",
                        analysis::pitchClassName(expected.root).c_str(),
                        expected.minor ? "minor" : "major",
                        analysis::keyDisplayName(result.key).c_str(),
                        result.key.confidence);
            coveredKeys = coveredKeys && result.key.root == expected.root &&
                result.key.scale == (expected.minor ? "natural_minor" : "major");
        }
        check(coveredKeys, "key detector covers transposed major and minor loops");
    }
    {
        std::vector<float> silence(std::size_t(rate * 4.0), 0.0f);
        analysis::MusicalAnalysisResult result;
        analysis::MusicalAnalysisRequest request;
        analysis::analyzeAudioSamples(silence.data(), silence.size(), 1, rate,
                                      request, result);
        check(result.tempo.status == analysis::DetectionStatus::Unavailable,
              "silence does not invent a tempo");
        check(result.key.status == analysis::DetectionStatus::Unavailable,
              "silence does not invent a key");
    }
    {
        daw::ProjectModel project;
        daw::TrackModel track;
        track.id = "analysis-track";
        track.kind = daw::TrackKind::Audio;
        daw::ClipModel clip;
        clip.id = "analysis-clip";
        clip.kind = daw::ClipKind::Audio;
        clip.filePath = "/tmp/analysis-fixture.wav";
        clip.musicalAnalysis.algorithmVersion = 1;
        clip.musicalAnalysis.analyzedOffsetSeconds = 1.25;
        clip.musicalAnalysis.analyzedDurationSeconds = 8.0;
        clip.musicalAnalysis.tempo.status = daw::MusicalAnalysisStatus::Available;
        clip.musicalAnalysis.tempo.bpm = 127.8;
        clip.musicalAnalysis.tempo.confidence = 0.91;
        clip.musicalAnalysis.tempo.stability = 0.96;
        clip.musicalAnalysis.tempo.alternatives = {63.9, 255.6};
        clip.musicalAnalysis.key.status = daw::MusicalAnalysisStatus::Available;
        clip.musicalAnalysis.key.root = 3;
        clip.musicalAnalysis.key.scale = "natural_minor";
        clip.musicalAnalysis.key.confidence = 0.84;
        track.clips.push_back(clip);
        project.tracks.push_back(track);

        namespace fs = std::filesystem;
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path folder = fs::temp_directory_path() /
            ("daw-musical-analysis-" + std::to_string(nonce));
        const fs::path document = folder / "recovery.vlt";
        daw::ProjectModel loaded;
        const bool saved = daw::ProjectSerializer::saveDocument(
            project, document.string(), daw::MediaPaths::Absolute).isOk();
        const bool opened = saved && daw::ProjectSerializer::loadDocument(
            loaded, document.string(), "").isOk();
        const daw::ClipModel* restored = nullptr;
        if (opened && !loaded.tracks.empty() && !loaded.tracks[0].clips.empty())
            restored = &loaded.tracks[0].clips[0];
        check(restored && restored->musicalAnalysis.algorithmVersion == 1 &&
                  std::abs(restored->musicalAnalysis.tempo.bpm - 127.8) < 1e-9 &&
                  restored->musicalAnalysis.tempo.alternatives.size() == 2 &&
                  restored->musicalAnalysis.key.root == 3 &&
                  restored->musicalAnalysis.key.scale == "natural_minor",
              "BPM and key analysis survives project serialization");
        std::error_code cleanupError;
        fs::remove_all(folder, cleanupError);
    }
    return failures == 0 ? 0 : 1;
}
