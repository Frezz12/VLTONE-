#include "TelemetrySnapshot.hpp"
#include <QJsonDocument>
#include <iostream>

int main() {
    daw::ProjectModel project;
    daw::TrackModel track;
    track.id = "bass"; track.name = "Бас"; track.kind = daw::TrackKind::Instrument;
    track.instrument.format = daw::PluginFormat::Vst3;
    track.instrument.uid = "synth"; track.instrument.name = "Synth";
    track.instrument.pluginVersion = "2.1"; track.instrument.path = "/secret/plugin.vst3";
    track.instrument.stateFile = "private-state";
    auto effect = track.instrument; effect.name = "Compressor"; effect.bypassed = true;
    effect.sidechainTrackId = "kick";
    track.inserts = {daw::InsertModel{}, effect};
    daw::ClipModel clip; clip.id = "clip"; clip.name = "Audio";
    clip.filePath = "/secret/audio.wav"; clip.inserts = {effect};
    track.clips.push_back(clip);
    project.tracks.push_back(track);
    auto snapshot = telemetry::projectSnapshot(project);
    const auto json = QJsonDocument(snapshot).toJson();
    const auto row = snapshot["tracks"].toArray().at(0).toObject();
    const auto insert = row["inserts"].toArray().at(0).toObject();
    if (row["name"] != QString::fromUtf8("Бас") || insert["slot"] != 1 ||
        !insert["bypassed"].toBool() || insert["sidechain_track_id"] != "kick" ||
        row["instrument"].toObject()["version"] != "2.1" || row["clip_fx"].toArray().size() != 1 ||
        json.contains("/secret") || json.contains("private-state")) return 1;
    const auto aggregates = telemetry::pluginAggregates(project);
    int count = 0;
    for (const auto& entry : aggregates) count += entry.toObject()["count"].toInt();
    if (count != 3) return 2;
    project.tracks[0].inserts.assign(257, effect);
    snapshot = telemetry::projectSnapshot(project);
    if (!snapshot["truncated"].toBool() || snapshot["tracks"].toArray().at(0).toObject()["inserts"].toArray().size() != 256) return 3;
    project.tracks.assign(2049, daw::TrackModel{});
    snapshot = telemetry::projectSnapshot(project);
    if (!snapshot["truncated"].toBool() || snapshot["tracks"].toArray().size() > 2048 ||
        QJsonDocument(snapshot).toJson(QJsonDocument::Compact).size() > 1100000) return 4;
    std::cout << "Telemetry metadata, privacy, slot ordering and bounds passed\n";
}
