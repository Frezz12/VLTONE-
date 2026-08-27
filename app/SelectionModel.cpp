#include "SelectionModel.hpp"

namespace ui {

SelectionKind SelectionModel::kind() const {
    if (!m_clips.isEmpty()) return SelectionKind::Clip;
    if (!m_tracks.isEmpty()) return SelectionKind::Track;
    return SelectionKind::None;
}

ClipSel SelectionModel::singleClip() const {
    if (m_clips.size() != 1) return {};
    return m_clips.first();
}

QString SelectionModel::singleTrack() const {
    if (m_tracks.size() != 1) return {};
    return m_tracks.first();
}

void SelectionModel::setTracks(const QStringList& trackIds) {
    // Views push their selection on every mouse-down, most of which re-select
    // what is already selected. Emitting there would rebuild the panel — and
    // restart its animation — on every click.
    if (trackIds == m_tracks && (trackIds.isEmpty() || m_clips.isEmpty())) return;
    m_tracks = trackIds;
    if (!m_tracks.isEmpty()) m_clips.clear();
    emit changed();
}

void SelectionModel::setClips(const QVector<ClipSel>& clips) {
    if (clips == m_clips && (clips.isEmpty() || m_tracks.isEmpty())) return;
    m_clips = clips;
    if (!m_clips.isEmpty()) m_tracks.clear();
    emit changed();
}

void SelectionModel::clear() {
    if (isEmpty()) return;
    m_tracks.clear();
    m_clips.clear();
    emit changed();
}

}  // namespace ui
