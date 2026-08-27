#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ui {

/// What the current selection is made of. `Mixed` covers a selection the panel
/// can't treat as one kind of thing (several clips of different kinds, say).
enum class SelectionKind { None, Track, Clip, Mixed };

/// A clip identified by both its track and its own id, so a selection survives
/// the clip being moved to another lane — the same pairing TimelineWidget uses
/// internally.
struct ClipSel {
    QString trackId;
    QString clipId;

    bool operator==(const ClipSel& other) const {
        return trackId == other.trackId && clipId == other.clipId;
    }
};

/// The one place that knows what is selected right now.
///
/// Selection used to live wherever it was made: a bare track id copied into
/// five widgets, a private clip vector inside the timeline, note ids inside the
/// piano roll. Nothing could ask "what is selected?", which is exactly what a
/// context-sensitive panel has to do. This model doesn't replace that state —
/// each view still owns and draws its own — it mirrors it so observers have a
/// single thing to watch.
///
/// Track and clip selections are mutually exclusive: picking a clip clears the
/// track selection and vice versa. A clip implies its track, but the two are
/// different contexts and must not present as one.
class SelectionModel : public QObject {
    Q_OBJECT
public:
    explicit SelectionModel(QObject* parent = nullptr) : QObject(parent) {}

    SelectionKind kind() const;
    const QStringList& tracks() const { return m_tracks; }
    const QVector<ClipSel>& clips() const { return m_clips; }
    bool isEmpty() const { return m_tracks.isEmpty() && m_clips.isEmpty(); }

    /// The clip a single-clip context should show. Empty when the selection
    /// isn't exactly one clip.
    ClipSel singleClip() const;
    /// The track a single-track context should show, or an empty string.
    QString singleTrack() const;

    void setTracks(const QStringList& trackIds);
    void setClips(const QVector<ClipSel>& clips);
    void clear();

    /// Re-emit changed() without a content change, so observers re-read the
    /// document. The controller emits nothing of its own, so this is how undo,
    /// redo and edits made elsewhere reach anything watching the selection.
    void refresh() { emit changed(); }

signals:
    void changed();

private:
    QStringList m_tracks;
    QVector<ClipSel> m_clips;
};

}  // namespace ui
