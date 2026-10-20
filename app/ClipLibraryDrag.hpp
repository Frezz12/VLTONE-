#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QString>
#include "SelectionModel.hpp"
#include <QJsonObject>

namespace ui::cliplibrary {
inline constexpr auto kTimelineMime = "application/x-vltone-timeline-clip";
inline constexpr auto kTimelineGroupMime = "application/x-vltone-timeline-clips";
inline void addTimelineGroup(QMimeData* mime, const QVector<ClipSel>& clips,
                             const void* project, quint64 generation) {
    QJsonArray items;
    for (const auto& clip : clips) items.append(QJsonArray{clip.trackId, clip.clipId});
    mime->setData(kTimelineGroupMime, QJsonDocument(QJsonObject{
        {"owner", QString::number(quintptr(project), 16)}, {"generation", QString::number(generation)},
        {"clips", items}}).toJson(QJsonDocument::Compact));
}
inline QVector<ClipSel> timelineGroup(const QMimeData* mime, const void* project, quint64 generation) {
    if (!mime || !mime->hasFormat(kTimelineGroupMime)) return {};
    const auto object = QJsonDocument::fromJson(mime->data(kTimelineGroupMime)).object();
    if (object["owner"].toString() != QString::number(quintptr(project), 16) ||
        object["generation"].toString() != QString::number(generation)) return {};
    QVector<ClipSel> clips;
    for (const auto& value : object["clips"].toArray()) {
        const auto pair = value.toArray();
        if (pair.size() == 2 && pair[0].isString() && pair[1].isString() &&
            !pair[0].toString().isEmpty() && !pair[1].toString().isEmpty())
            clips.push_back({pair[0].toString(), pair[1].toString()});
    }
    return clips;
}
inline constexpr auto kLibraryMime = "application/x-vltone-library-clip";
inline const QString kTabId = QStringLiteral("project-clips");
inline QMimeData* timelineMime(const QString& track, const QString& clip) {
    auto* mime = new QMimeData;
    mime->setData(kTimelineMime, QJsonDocument(QJsonArray{track, clip}).toJson(QJsonDocument::Compact));
    return mime;
}
inline bool decodeTimeline(const QMimeData* mime, QString& track, QString& clip) {
    if (!mime || !mime->hasFormat(kTimelineMime)) return false;
    const auto fields = QJsonDocument::fromJson(mime->data(kTimelineMime)).array();
    if (fields.size() != 2 || !fields[0].isString() || !fields[1].isString()) return false;
    track = fields[0].toString(); clip = fields[1].toString();
    return !track.isEmpty() && !clip.isEmpty();
}
inline QString libraryId(const QMimeData* mime) {
    return mime && mime->hasFormat(kLibraryMime) ? QString::fromUtf8(mime->data(kLibraryMime)) : QString{};
}
} // namespace ui::cliplibrary
