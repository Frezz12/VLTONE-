#include "AudioMusicalAnalysis.hpp"
#include "analysis_v1/AudioMusicalAnalysis.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using nlohmann::json;
namespace {
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::size_t first = 0;
    for (;;) {
        auto next = line.find('\t', first);
        out.push_back(line.substr(first, next == std::string::npos ? next : next - first));
        if (next == std::string::npos) return out;
        first = next + 1;
    }
}
template<class Result> json prediction(const Result& r) {
    return {{"bpm", r.tempo.bpm}, {"tempoStatus", int(r.tempo.status)},
        {"tempoConfidence", r.tempo.confidence}, {"tempoHigh", r.tempo.highConfidence()},
        {"tempoStability", r.tempo.stability}, {"tempoVariable", r.tempo.variable},
        {"tempoAlternatives", r.tempo.alternatives}, {"root", r.key.root}, {"scale", r.key.scale},
        {"keyStatus", int(r.key.status)}, {"keyConfidence", r.key.confidence},
        {"keyHigh", r.key.highConfidence()}, {"alternateRoot", r.key.alternateRoot},
        {"alternateScale", r.key.alternateScale}, {"tuningCents", r.key.tuningCents}};
}
}
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "audio_analysis_bench manifest.tsv [--backend legacy|hybrid|dsp] [--split fit|calibration|test] [--output results.jsonl]\n";
        return 2;
    }
    std::string backend = "hybrid", selection, output;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        std::string opt = argv[i];
        if (opt == "--backend") backend = argv[i + 1];
        else if (opt == "--split") selection = argv[i + 1];
        else if (opt == "--output") output = argv[i + 1];
        else return 2;
    }
    if (backend != "legacy" && backend != "hybrid" && backend != "dsp") return 2;
    std::ifstream in(argv[1]);
    std::ofstream file;
    if (!output.empty()) file.open(output);
    if (!in || (!output.empty() && !file)) return 2;
    std::ostream& out = output.empty() ? std::cout : file;
    std::string line;
    int count = 0, failed = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line);
        if (f[0] == "path") continue;
        if (f.size() < 4) return 2;
        if (!selection.empty() && (f.size() < 6 || f[5] != selection)) continue;
        json row;
        try {
            row = {{"path", f[0]}, {"expectedBpm", std::stod(f[1])}, {"expectedRoot", std::stoi(f[2])},
                   {"expectedScale", f[3]}, {"group", f.size() > 4 ? f[4] : f[0]},
                   {"split", f.size() > 5 ? f[5] : "unspecified"}, {"backend", backend},
                   {"keyAbsent", f.size() > 7 && f[7] == "1"}};
        } catch (...) { std::cerr << "invalid manifest row\n"; return 2; }
        std::filesystem::path path(f[0]);
        if (path.is_relative()) path = std::filesystem::path(argv[1]).parent_path() / path;
        const auto started = std::chrono::steady_clock::now();
        if (backend == "legacy") {
            daw::analysis_v1::MusicalAnalysisRequest req;
            // Freeze old behavior, including its original filename hint.
            req.fileNameHint = path.filename().string();
            daw::analysis_v1::MusicalAnalysisResult r;
            const auto status = daw::analysis_v1::analyzeAudioFile(path.string(), req, r);
            row.update(prediction(r));
            row["error"] = status ? "" : status.message();
        } else {
            daw::analysis::MusicalAnalysisRequest req;
            req.useNeuralModels = backend == "hybrid";
            daw::analysis::MusicalAnalysisResult r;
            const auto status = daw::analysis::analyzeAudioFile(path.string(), req, r);
            row.update(prediction(r));
            row.update({{"tempoEvidence", r.tempo.evidence}, {"keyEvidence", r.key.evidence},
                {"tempoBackend", r.tempo.backend}, {"keyBackend", r.key.backend},
                {"keyVariable", r.key.variable}, {"profileScores", r.key.profileScores},
                {"neuralScores", r.key.neuralScores}, {"error", status ? "" : status.message()}});
        }
        row["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        out << row.dump() << std::endl;
        ++count;
        if (row["error"] != "") ++failed;
        std::cerr << count << " " << path.filename().string() << " " << row["bpm"] << " " << row["root"] << " " << row["scale"] << '\n';
    }
    return count == 0 || failed ? 1 : 0;
}
