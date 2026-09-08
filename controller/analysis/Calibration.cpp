#include "Calibration.hpp"
#include "NeuralModels.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>

namespace daw::analysis::detail {
namespace {
using json = nlohmann::json;
json configuration() {
    // Tiny file, read per analysis so tests and explicitly replaced calibration
    // bundles cannot inherit cached settings from another asset directory.
    try {
        std::ifstream file(analysisAssetDirectory() / "calibration.json");
        if (!file) return {};
        auto value = json::parse(file);
        if (value.value("algorithmVersion", 0) != MusicalAnalysisResult::kAlgorithmVersion) return {};
        std::ifstream manifest(analysisAssetDirectory() / "manifest.json");
        if (!manifest) return {};
        const auto models = json::parse(manifest);
        if (value.value("modelSha256", json{}) != models.value("sha256", json{})) return {};
        return value;
    } catch (const json::exception&) { return {}; }
}

template<class Estimate>
void apply(Estimate& estimate, std::string_view task) {
    estimate.calibrated = false;
    if (estimate.status == DetectionStatus::Unavailable) return;
    const auto config = configuration();
    try {
        const auto& entry = config.at("backends").at(std::string(task) + ":" + estimate.backend);
        if (!entry.value("validated", false)) return;
        const auto& knots = entry.at("knots");
        if (!knots.is_array() || knots.size() < 2) return;
        double lastX = -1, lastY = -1;
        for (const auto& knot : knots) {
            if (!knot.is_array() || knot.size() != 2) return;
            const double x = knot[0].template get<double>(), y = knot[1].template get<double>();
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1 || x < lastX || y < lastY) return;
            lastX = x; lastY = y;
        }
        double probability = 0.0;
        for (const auto& knot : knots) {
            if (estimate.evidence < knot[0].template get<double>()) break;
            probability = knot[1].template get<double>();
        }
        estimate.confidence = probability;
        estimate.calibrated = true;
    } catch (const json::exception&) { return; }
}
}
void calibrate(TempoEstimate& estimate) { apply(estimate, "tempo"); }
void calibrate(KeyEstimate& estimate) { apply(estimate, "key"); }
double keyModelWeight() {
    try {
        const double w = configuration().value("keyModelWeight", 0.65);
        return std::isfinite(w) && w >= 0 && w <= 1 ? w : 0.65;
    } catch (const json::exception&) { return 0.65; }
}
} // namespace daw::analysis::detail
