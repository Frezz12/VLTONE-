#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include "CollaborationTypes.hpp"
#include "ChannelStrip.hpp"

#include <optional>
#include <vector>

namespace daw { class EngineController; }
class RackWidget;
class QStackedWidget;
class QHBoxLayout;
class QLabel;
class QScrollArea;
class QToolButton;
namespace ui { class FrameTimer; }

/// The mixer console: a scrolling row of channel strips with the master strip
/// independently scrollable and collapsible on the right, under a command bar. It lives in
/// the centre column below the timeline.
class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(daw::EngineController* controller,
                         QWidget* parent = nullptr);

    void rebuild();
    void refreshMeters();
    void syncMeterTimer();
    void refreshAutomationValues();
    /// Re-read every strip's values from the document — the cheap counterpart
    /// to `rebuild`, for when a level or a flag was changed somewhere else.
    void syncFromModel(const QStringList& trackIds = {});
    /// The gain a strip is showing, or −1 when there is no such strip.
    double faderGainForTest(const QString& trackId) const;
    void setSelectedTrack(const QString& trackId);
    int channelWidth() const { return m_channelWidth; }
    bool rackMode() const { return m_rackMode; }
    void setRackMode(bool enabled);
    bool rackCommand(const QString& command);
    RackWidget* rack() const { return m_rack; }

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
    /// Heterogeneous racks stay aligned through resize, scrolling and rebuild.
    static bool checkLayoutForTest();
    static bool checkMasterDockForTest();
    /// Where a track's strip sits right now, for that regression.
    QRect stripRectForTrackForTest(const QString& trackId) const;

signals:
    void trackSelected(const QString& trackId);
    void edited(bool localFileDirty = true);
    void channelEdited(const QString& trackId, bool localFileDirty);
    /// An insert, instrument or routing slot changed. Ordinary value edits do
    /// not emit this, so the shell can keep its existing strip widgets alive.
    void structureChanged();
    void trackCreated();
    void timelineRequested(const QString& channelId);
    void createTracksRequested();
    void trackRemoved(const QString& trackId);
    void pluginEditorRequested(const QString& channelId, const QString& insertId);
    void openPatternRequested(const QString& patternId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);
    void automatePluginRequested(const QString& trackId, const QString& slotId, const QString& parameterId);
    void settingsRequested();
    void rackModeChanged(bool enabled);

private:
    void contextMenuEvent(QContextMenuEvent*) override;
    bool eventFilter(QObject* object, QEvent* event) override;
    void syncVisibleStrips();
    void applyChannelWidth(int width);
    void setMasterVisible(bool visible);
    void updateMasterGeometry();
    void setMasterRevealWidth(int width);
    void updateMasterToggle();
    int masterExpandedWidth() const;
    void wireStrip(ChannelStrip* strip);
    QStringList m_channels;
    std::vector<ChannelStrip*> m_slots;
    bool m_syncingStrips = false;
    bool m_deferredRackChange = false;
    ui::FrameTimer* m_materializeTimer = nullptr;
    ui::FrameTimer* m_meterTimer = nullptr;
    void applyTheme();
    bool stripIsVisible(const ChannelStrip* strip) const;
    /// The strip under a point in this widget's coordinates, and where a given
    /// track's strip currently sits. Null/empty when the console does not show
    /// that track right now.
    const ChannelStrip* stripAt(const QPoint& position) const;
    QRect stripRectFor(const ChannelStrip* strip) const;

    daw::EngineController* m_controller = nullptr;
    RackWidget* m_rack = nullptr;
    QStackedWidget* m_views = nullptr;
    QToolButton* m_viewToggle = nullptr;
    QLabel* m_headerTitle = nullptr;
    bool m_rackMode = false;
    QLabel* m_headerGlyph = nullptr;
    QLabel* m_headerCount = nullptr;
    QWidget* m_headerAccent = nullptr;
    QWidget* m_header = nullptr;
    QScrollArea* m_scroll = nullptr;
    QScrollArea* m_masterScroll = nullptr;
    QWidget* m_masterDock = nullptr;
    QWidget* m_masterColumn = nullptr;
    QWidget* m_masterEdge = nullptr;
    QToolButton* m_masterToggle = nullptr;
    bool m_masterVisible = true;
    bool m_masterDragging = false;
    int m_masterDragStartWidth = 0;
    int m_channelWidth = ChannelStrip::kWidth;
    QWidget* m_stripsHost = nullptr;
    QHBoxLayout* m_stripsLayout = nullptr;
    QWidget* m_masterHost = nullptr;
    QWidget* m_masterBottomGap = nullptr;
    ChannelStrip::RackHeights m_rackHeights{};
    int m_insertSlotCount = 2;
    std::vector<ChannelStrip*> m_strips;
    QString m_selectedTrackId;
};
