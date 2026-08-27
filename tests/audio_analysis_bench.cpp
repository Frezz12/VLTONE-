#include "AudioMusicalAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace analysis = daw::analysis;

namespace {

struct Item {
    std::string path;
    double bpm = 0.0;
    int root = -1;
    std::string scale;
};

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) fields.push_back(field);
    return fields;
}

bool tempoMatches(double detected, double expected) {
    return detected > 0.0 && expected > 0.0 &&
           std::abs(detected - expected) / expected <= 0.01;
}

bool metricalMatch(double detected, double expected) {
    return tempoMatches(detected, expected) ||
           tempoMatches(detected * 0.5, expected) ||
           tempoMatches(detected * 2.0, expected);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr
            << "usage: audio_analysis_bench manifest.tsv\n\n"
               "TSV columns: path, bpm (0 if unknown), root (C=0, -1 if "
               "unknown), scale (major or natural_minor). A header is optional.\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "cannot open manifest: " << argv[1] << '\n';
        return 2;
    }

    std::vector<Item> items;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') continue;
        const auto fields = splitTabs(line);
        if (fields.size() < 4) {
            std::cerr << "line " << lineNumber << ": expected four TSV fields\n";
            return 2;
        }
        try {
            Item item{fields[0], std::stod(fields[1]), std::stoi(fields[2]),
                      fields[3]};
            items.push_back(std::move(item));
        } catch (...) {
            if (items.empty() && lineNumber == 1) continue; // optional header
            std::cerr << "line " << lineNumber << ": invalid BPM/root\n";
            return 2;
        }
    }
    if (items.empty()) {
        std::cerr << "manifest contains no examples\n";
        return 2;
    }

    int analyzed = 0, failed = 0;
    int tempoKnown = 0, tempoExact = 0, tempoMetrical = 0;
    int tempoHigh = 0, tempoHighExact = 0;
    int keyKnown = 0, keyExact = 0, keyHigh = 0, keyHighExact = 0;
    std::cout << "file\texpected_bpm\tdetected_bpm\ttempo_conf\texpected_key"
                 "\tdetected_key\tkey_conf\n";
    for (const Item& item : items) {
        analysis::MusicalAnalysisRequest request;
        request.fileNameHint = item.path;
        analysis::MusicalAnalysisResult result;
        const auto status = analysis::analyzeAudioFile(item.path, request, result);
        if (!status) {
            ++failed;
            std::cerr << item.path << ": " << status.message() << '\n';
            continue;
        }
        ++analyzed;
        bool bpmExact = false;
        if (item.bpm > 0.0) {
            ++tempoKnown;
            bpmExact = tempoMatches(result.tempo.bpm, item.bpm);
            tempoExact += bpmExact;
            bool usable = metricalMatch(result.tempo.bpm, item.bpm);
            for (double alternative : result.tempo.alternatives)
                usable = usable || tempoMatches(alternative, item.bpm);
            tempoMetrical += usable;
            if (result.tempo.highConfidence()) {
                ++tempoHigh;
                tempoHighExact += bpmExact;
            }
        }
        bool exactKey = false;
        if (item.root >= 0) {
            ++keyKnown;
            exactKey = result.key.root == item.root &&
                       result.key.scale == item.scale;
            keyExact += exactKey;
            if (result.key.highConfidence()) {
                ++keyHigh;
                keyHighExact += exactKey;
            }
        }
        const std::string expectedKey = item.root < 0
            ? "-" : analysis::pitchClassName(item.root) + " " + item.scale;
        std::cout << item.path << '\t' << std::fixed << std::setprecision(1)
                  << item.bpm << '\t' << result.tempo.bpm << '\t'
                  << std::setprecision(3) << result.tempo.confidence << '\t'
                  << expectedKey << '\t' << analysis::keyDisplayName(result.key)
                  << '\t' << result.key.confidence << '\n';
    }

    const auto percent = [](int correct, int total) {
        return total ? 100.0 * correct / total : 0.0;
    };
    std::cout << "\nAnalyzed " << analyzed << ", failed " << failed << '\n'
              << "Tempo exact (1%): " << tempoExact << '/' << tempoKnown << " ("
              << std::setprecision(1) << percent(tempoExact, tempoKnown) << "%)\n"
              << "Tempo incl. half/double or alternative: " << tempoMetrical << '/'
              << tempoKnown << " (" << percent(tempoMetrical, tempoKnown) << "%)\n"
              << "High-confidence tempo precision: " << tempoHighExact << '/'
              << tempoHigh << " (" << percent(tempoHighExact, tempoHigh) << "%)\n"
              << "Key exact: " << keyExact << '/' << keyKnown << " ("
              << percent(keyExact, keyKnown) << "%)\n"
              << "High-confidence key precision: " << keyHighExact << '/'
              << keyHigh << " (" << percent(keyHighExact, keyHigh) << "%)\n";
    return failed == int(items.size()) ? 1 : 0;
}
