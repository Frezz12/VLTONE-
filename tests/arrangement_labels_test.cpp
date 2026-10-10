#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
        failures += !condition;
    };
    daw::EngineController controller;
    const std::vector<daw::ArrangementLabel> chords{{"b", "G/B", 8, 4}, {"a", "Am7", 0, 8}};
    check(controller.setArrangementLabels(true, chords), "add chords");
    check(controller.project().chords.front().id == "a", "sort by musical position");
    const auto saved = controller.project().chords;
    check(controller.setArrangementLabels(false, {{"s", "Hook", 16, 32}}), "add song section");
    controller.undo();
    check(controller.project().sections.empty() && controller.project().chords == saved, "undo preserves other row");
    controller.redo();
    check(controller.project().sections.size() == 1, "redo restores section");
    auto moved = saved; moved[0].startBeats = 4;
    check(controller.setArrangementLabels(true, moved), "move chord");
    controller.undo();
    check(controller.project().chords == saved, "undo restores chord position");
    check(!controller.setArrangementLabels(true, {{"bad", "C", -1, 4}}), "reject negative start");
    check(!controller.setArrangementLabels(true, {{"bad", "C", 0, 0}}), "reject zero duration");
    check(!controller.setArrangementLabels(true, {{"bad", "C", 0, std::numeric_limits<double>::infinity()}}), "reject nonfinite duration");
    check(!controller.setArrangementLabels(true, {{"same", "C", 0, 4}, {"same", "D", 4, 4}}), "reject duplicate identities");
    check(controller.project().chords == saved, "invalid edits do not mutate labels");
    std::string bytes;
    check(daw::ProjectSerializer::serializeDocument(controller.project(), bytes).isOk(), "serialize labels");
    daw::ProjectModel restored;
    check(daw::ProjectSerializer::deserializeDocument(restored, bytes).isOk(), "deserialize labels");
    check(restored.chords == saved && restored.sections == controller.project().sections, "roundtrip labels exactly");
    auto json = nlohmann::json::parse(bytes); json.erase("arrangement");
    check(daw::ProjectSerializer::deserializeDocument(restored, json.dump()).isOk() && restored.chords.empty() && restored.sections.empty(), "older project opens with empty annotation rows");
    json = nlohmann::json::parse(bytes);
    json["arrangement"]["chords"].push_back({{"id", "a"}, {"text", "F"}, {"startBeats", 24}, {"durationBeats", 4}});
    json["arrangement"]["sections"][0]["durationBeats"] = -3;
    check(daw::ProjectSerializer::deserializeDocument(restored, json.dump()).isOk() && restored.sections.empty() && restored.chords.size() == 3 && restored.chords.back().id != "a", "repair duplicate IDs and skip invalid durations");
    return failures ? 1 : 0;
}
