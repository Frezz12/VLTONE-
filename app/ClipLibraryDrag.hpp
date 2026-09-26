#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QString>

namespace ui::cliplibrary {
inline constexpr auto kTimelineMime = "application/x-vltone-timeline-clip";
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
