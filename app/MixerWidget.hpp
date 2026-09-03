#pragma once

#include <QString>
#include <QWidget>

#include "CollaborationTypes.hpp"

#include <optional>
#include <vector>

namespace daw { class EngineController; }
class QHBoxLayout;
class QLabel;
class QScrollArea;
class ChannelStrip;

/// The mixer console: a scrolling row of channel strips with the master strip
/// pinned to the right, under a thin header ("MIXER · n channels · audio
/// online"). It lives in the centre column below the timeline, so it spans from
/// the track headers to the right edge of the window.
class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(daw::EngineController* controller,
                         QWidget* parent = nullptr);

    void rebuild();
    void refreshMeters();
    void refreshAutomationValues();
    /// Re-read every strip's values from the document — the cheap counterpart
    /// to `rebuild`, for when a level or a flag was changed somewhere else.
    void syncFromModel();
    /// The gain a strip is showing, or −1 when there is no such strip.
    double faderGainForTest(const QString& trackId) const;
    void setSelectedTrack(const QString& trackId);

    /// Presence encode/decode, mirroring TimelineWidget's pair. A pointer is
    /// described by the strip it is over and how far down that strip it sits,
    /// so a collaborator scrolled to a different part of the console still sees
    /// it on the right channel — or not at all, rather than on the wrong one.
    collab::SemanticPoint collaborationPresenceAt(const QPointF& position) const;
    std::optional<QPointF> collaborationPositionFor(
        const collab::SemanticPoint& point) const;
    /// Headless regression: a pointer keeps naming the same channel when the
    /// console is resized, the master strip is addressed without a track id,
    /// and a channel this console does not show is hidden rather than mapped
    /// onto a neighbouring strip.
    static bool checkCollaborationPresenceForTest(QString* error = nullptr);
    /// Where a track's strip sits right now, for that regression.
    QRect stripRectForTrackForTest(const QString& trackId) const;

signals:
    void trackSelected(const QString& trackId);
    void edited(bool localFileDirty = true);
    /// An insert, instrument or routing slot changed. Ordinary value edits do
    /// not emit this, so the shell can keep its existing strip widgets alive.
    void structureChanged();
    void trackRemoved(const QString& trackId);
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    void openPatternRequested(const QString& patternId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);

private:
    void applyTheme();
    bool stripIsVisible(const ChannelStrip* strip) const;
    /// The strip under a point in this widget's coordinates, and where a given
    /// track's strip currently sits. Null/empty when the console does not show
    /// that track right now.
    const ChannelStrip* stripAt(const QPoint& position) const;
    QRect stripRectFor(const ChannelStrip* strip) const;

    daw::EngineController* m_controller = nullptr;
    QLabel* m_headerCount = nullptr;
    QLabel* m_statusDot = nullptr;
    QLabel* m_statusText = nullptr;
    QWidget* m_header = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_stripsHost = nullptr;
    QHBoxLayout* m_stripsLayout = nullptr;
    QWidget* m_masterHost = nullptr;
    std::vector<ChannelStrip*> m_strips;
    QString m_selectedTrackId;
    bool m_statusInitialized = false;
    bool m_statusOnline = false;
};
