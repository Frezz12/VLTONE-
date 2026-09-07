#pragma once

#include "EngineController.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>

namespace telemetry {
// Metadata only: never serialize project documents, plugin state, paths or media.
inline QString text(const std::string& value, int limit = 160) {
    auto result = QString::fromStdString(value).trimmed().left(limit);
    if (result.contains('/') || result.contains('\\')) return QStringLiteral("<path-redacted>");
    return result;
}
inline QJsonObject plugin(const daw::InsertModel& slot, int index) {
    return {{"id", text(slot.id, 64)}, {"slot", index}, {"name", text(slot.name)},
            {"vendor", text(slot.vendor)}, {"version", text(slot.pluginVersion, 64)},
            {"format", text(daw::toString(slot.format), 16)}, {"bypassed", slot.bypassed},
            {"mix", slot.mix}, {"channel_mode", text(daw::toString(slot.channelMode), 32)},
            {"sidechain_track_id", text(slot.sidechainTrackId, 64)}};
}
inline QJsonArray chain(const std::vector<daw::InsertModel>& inserts, bool& truncated) {
    QJsonArray result;
    for (size_t i = 0; i < inserts.size(); ++i) {
        if (!inserts[i].isLoaded()) continue;
        if (result.size() >= 256) { truncated = true; break; }
        result.append(plugin(inserts[i], int(i)));
    }
    return result;
}
inline QJsonArray pluginAggregates(const daw::ProjectModel& project) {
    QMap<QString, QJsonObject> entries;
    const auto add = [&entries](const daw::InsertModel& slot) {
        if (!slot.isLoaded()) return;
        const QString key = text(slot.name) + QChar(31) + text(slot.vendor) + QChar(31)
            + text(daw::toString(slot.format)) + QChar(31) + text(slot.pluginVersion, 64);
        if (!entries.contains(key) && entries.size() >= 500) return;
        auto& entry = entries[key];
        if (entry.isEmpty()) entry = {{"name", text(slot.name)}, {"vendor", text(slot.vendor)},
            {"format", text(daw::toString(slot.format), 16)}, {"version", text(slot.pluginVersion, 64)}, {"count", 0}};
        entry.insert("count", entry.value("count").toInt() + 1);
    };
    for (const auto& slot : project.masterInserts) add(slot);
    for (const auto& track : project.tracks) {
        add(track.instrument);
        for (const auto& slot : track.inserts) add(slot);
        for (const auto& slot : track.samplerFx.inserts) add(slot);
        for (const auto& clip : track.clips) {
            for (const auto& slot : clip.inserts) add(slot);
            for (const auto& slot : clip.offlineProcess.chain) add(slot);
        }
    }
    QJsonArray result;
    for (const auto& entry : entries) result.append(entry);
    return result;
}
inline QJsonObject projectSnapshot(const daw::ProjectModel& project) {
    bool truncated = false;
    QJsonArray tracks;
    auto master = chain(project.masterInserts, truncated);
    qsizetype bytes = QJsonDocument(master).toJson(QJsonDocument::Compact).size();
    for (const auto& track : project.tracks) {
        QJsonArray sends;
        for (const auto& send : track.sends) {
            if (sends.size() >= 256) { truncated = true; break; }
            sends.append(QJsonObject{{"destination_track_id", text(send.destinationTrackId, 64)},
                {"level", send.level}, {"pre_fader", send.preFader}, {"enabled", send.enabled}});
        }
        QJsonArray clipFx;
        qsizetype clipBytes = 0;
        for (const auto& clip : track.clips) {
            if (clip.inserts.empty() && clip.offlineProcess.chain.empty()) continue;
            QJsonObject entry{{"id", text(clip.id, 64)}, {"name", text(clip.name)},
                {"kind", text(daw::toString(clip.kind), 32)}, {"start_seconds", clip.startSeconds},
                {"duration_seconds", clip.durationSeconds}, {"muted", clip.muted},
                {"inserts", chain(clip.inserts, truncated)}, {"offline_inserts", chain(clip.offlineProcess.chain, truncated)}};
            clipBytes += QJsonDocument(entry).toJson(QJsonDocument::Compact).size();
            if (clipFx.size() >= 256 || clipBytes > 128 * 1024) { truncated = true; break; }
            clipFx.append(entry);
        }
        QJsonObject item{{"id", text(track.id, 64)}, {"name", text(track.name)},
            {"kind", text(daw::toString(track.kind), 32)}, {"parent_id", text(track.parentId, 64)},
            {"output_bus_id", text(track.outputBusId, 64)}, {"volume", track.volume}, {"pan", track.pan},
            {"muted", track.muted}, {"soloed", track.soloed}, {"armed", track.armed},
            {"monitor", track.monitor}, {"mono", track.mono}, {"frozen", track.freeze.active()},
            {"input_enabled", track.inputEnabled}, {"input_channel", int(track.inputChannel)},
            {"input_channel_count", int(track.inputChannelCount)}, {"clip_count", int(track.clips.size())},
            {"instrument", track.instrument.isLoaded() ? QJsonValue(plugin(track.instrument, 0)) : QJsonValue::Null},
            {"inserts", chain(track.inserts, truncated)}, {"sampler_fx", chain(track.samplerFx.inserts, truncated)},
            {"sampler_fx_active", track.samplerFx.isOwnedBy(track.instrument)}, {"sends", sends}, {"clip_fx", clipFx}};
        bytes += QJsonDocument(item).toJson(QJsonDocument::Compact).size();
        if (tracks.size() >= 2048 || bytes > 1024 * 1024) { truncated = true; break; }
        tracks.append(item);
    }
    return {{"schema_version", 1}, {"tracks", tracks}, {"master_inserts", master},
        {"truncated", truncated}, {"tempo", project.tempo},
        {"time_signature_numerator", project.timeSigNumerator}, {"time_signature_denominator", project.timeSigDenominator},
        {"loop_enabled", project.loopEnabled}, {"loop_start_seconds", project.loopStartSeconds},
        {"loop_end_seconds", project.loopEndSeconds}, {"master_volume", project.masterVolume}, {"master_pan", project.masterPan}};
}
inline QJsonObject device(const audio::DeviceInfo& value) {
    return {{"name", text(value.name)}, {"manufacturer", text(value.manufacturer)},
        {"host_api", text(value.hostApi, 64)}, {"input_channels", int(value.inputChannels)},
        {"output_channels", int(value.outputChannels)}, {"is_asio", value.isAsio}, {"is_alive", value.isAlive}};
}
inline QJsonObject audioSnapshot(const daw::EngineController& controller) {
    const auto config = controller.audioConfiguration();
    const auto xruns = controller.audioXruns();
    QJsonArray inputs, outputs;
    for (int channel : config.inputChannelSelectors) if (inputs.size() < 256) inputs.append(channel);
    for (int channel : config.outputChannelSelectors) if (outputs.size() < 256) outputs.append(channel);
    return {{"input", device(controller.currentInputDeviceInfo())}, {"output", device(controller.currentOutputDeviceInfo())},
        {"running", controller.audioDeviceRunning()}, {"input_enabled", config.inputEnabled},
        {"input_channels", inputs}, {"output_channels", outputs},
        {"input_underflow", double(xruns[0])}, {"input_overflow", double(xruns[1])},
        {"output_underflow", double(xruns[2])}, {"output_overflow", double(xruns[3])},
        {"gated_blocks", double(controller.gatedAudioBlocks())}, {"workers", int(controller.audioWorkerCount())},
        {"realtime_workers", int(controller.realtimeAudioWorkerCount())}, {"workgroup_workers", int(controller.workgroupAudioWorkerCount())}};
}
} // namespace telemetry
