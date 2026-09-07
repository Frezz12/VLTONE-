#pragma once

#include "model/Document.hpp"
#include <QByteArray>
#include <QDataStream>
#include <QIODevice>

namespace ui {
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
        string(insert.path); string(insert.sidechainTrackId);
        out << int(insert.format) << int(insert.channelMode) << insert.bypassed;
    };
    const auto writeSlots = [&](const auto& inserts) {
        out << quint64(inserts.size());
        for (const auto& insert : inserts) slot(insert);
    };
    if (id.empty()) { writeSlots(project.masterInserts); return bytes; }
    if (const auto* track = project.findTrack(id)) {
        out << int(track->kind);
        slot(track->instrument);
        writeSlots(track->inserts);
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
