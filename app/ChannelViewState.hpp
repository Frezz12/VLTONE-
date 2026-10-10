#pragma once

#include "model/Document.hpp"
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QSettings>

namespace ui {
inline int channelInsertRows(const daw::ProjectModel& project, const std::string& id) {
    const bool master = id.empty() || id == "master";
    const auto* track = project.findTrack(id);
    if (!master && !track) return 2;
    const auto& inserts = master ? project.masterInserts : track->inserts;
    const auto& groups = master ? project.masterRackGroups : track->rackGroups;
    int rows = int(inserts.size());
    const QString key = "rack/" + QString::fromStdString(project.miniModuleProjectId)
        + "/" + (master ? QStringLiteral("master") : QString::fromStdString(id)) + "/mixerGroup/";
    for (const auto& group : groups) {
        ++rows;
        if (QSettings().value(key + QString::fromStdString(group.id), false).toBool())
            rows -= int(group.insertIds.size());
    }
    return std::max(2, rows + 1);
}
// Only the fields that determine a strip's controls. Audio, MIDI and opaque
// plugin state must never be copied just to decide whether widgets can survive.
inline QByteArray channelViewState(const daw::ProjectModel& project,
                                   const std::string& id) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    const auto string = [&out](const std::string& value) {
        out << QByteArray::fromStdString(value);
    };
    const auto slot = [&](const daw::InsertModel& insert) {
        string(insert.id); string(insert.name); string(insert.uid);
        string(insert.path);
        out << quint64(insert.sidechainTrackIds.size());
        for (const auto& source : insert.sidechainTrackIds) string(source);
        out << int(insert.format) << int(insert.channelMode) << insert.bypassed;
    };
    const auto writeSlots = [&](const auto& inserts) {
        out << quint64(inserts.size());
        for (const auto& insert : inserts) slot(insert);
    };
    const auto writeGroups = [&](const auto& groups) {
        out << quint64(groups.size());
        for (const auto& group : groups) {
            string(group.id); string(group.name);
            const auto* owner = project.findTrack(id);
            const auto channel = id.empty() || (owner && owner->kind == daw::TrackKind::Master)
                ? QStringLiteral("master") : QString::fromStdString(id);
            out << QSettings().value("rack/" + QString::fromStdString(project.miniModuleProjectId)
                + "/" + channel + "/mixerGroup/" + QString::fromStdString(group.id), false).toBool();
            out << quint64(group.insertIds.size());
            for (const auto& member : group.insertIds) string(member);
        }
    };
    const auto* selected = project.findTrack(id);
    if (id.empty() || id == "master" || (selected && selected->kind == daw::TrackKind::Master)) {
        writeSlots(project.masterInserts); writeGroups(project.masterRackGroups); return bytes;
    }
    if (const auto* track = project.findTrack(id)) {
        out << int(track->kind) << track->summing;
        slot(track->instrument);
        writeSlots(track->inserts);
        writeGroups(track->rackGroups);
        out << quint64(track->sends.size());
        for (const auto& send : track->sends) {
            string(send.id); string(send.destinationTrackId);
            out << send.preFader << send.enabled;
            if (const auto* target = project.findTrack(send.destinationTrackId))
                string(target->name);
        }
    }
    return bytes;
}
} // namespace ui
