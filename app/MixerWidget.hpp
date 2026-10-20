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
class QSplitter;
class QToolButton;
namespace ui { class FrameTimer; }

/// The mixer console: a scrolling row of track strips under a command bar.
/// The master lives in its own right sidebar. The console lives in
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
    static bool checkCompactForTest();
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
    void positionStrips();
    void applyChannelWidth(int width);
    void applyView(ui::MixerPreferences::View view);
    void applyDetailsWidth(int width);
    void syncDetails();
    void finishChannelEdits();
    void setMasterVisible(bool visible);
    void updateMasterToggle();
    void wireStrip(ChannelStrip* strip);
    QStringList m_channels;
    std::vector<ChannelStrip*> m_slots;
    // Sorted slot indices for materialised channels; scroll work must scale
    // with the resident viewport budget, not the whole project.
    std::vector<int> m_residentSlots;
    std::vector<ChannelStrip*> m_recycledStrips;
    bool m_syncingStrips = false;
    bool m_stripLayoutDirty = true;
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
    QSplitter* m_splitter = nullptr;
    QScrollArea* m_detailsScroll = nullptr;
    ChannelStrip* m_details = nullptr;
    ui::MixerPreferences::View m_view = ui::MixerPreferences::View::Classic;
    bool m_syncingDetails = false;
    bool m_settingDetailsWidth = false;
    std::array<int, 2> m_verticalPositions{};
    QToolButton* m_masterToggle = nullptr;
    bool m_masterVisible = true;
    int m_channelWidth = ChannelStrip::kWidth;
    QWidget* m_stripsHost = nullptr;
    ChannelStrip::RackHeights m_rackHeights{};
    int m_insertSlotCount = 2;
    quint64 m_colorHeightRevision = ~quint64(0), m_colorHeightGeneration = 0;
    int m_colorHeightWidth = -1, m_colorHeight = 0;
    std::vector<ChannelStrip*> m_strips;
    QString m_selectedTrackId;
};
