#pragma once

#include <QString>
#include <QWidget>

#include <functional>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace daw {
class EngineController;
namespace plugins { struct PluginDescriptor; }
} // namespace daw

class QLabel;
class QMenu;
/// Defined in the .cpp: the I/O plate the strip's routing rows are made of.
class RoutingField;
class QToolButton;
class QVBoxLayout;
namespace ui { class FaderWidget; class PanKnob; class LevelMeter; class MsrButton;
               class IconButton; }

/// A full console channel strip: colour header, I/O routing, insert (Audio FX)
/// slots, aux sends, pan, fader + stereo meter, mute/solo/record and a name
/// plate. Used both in the mixer and in the left inspector, where it shows the
/// selected track.
class ChannelStrip : public QWidget {
    Q_OBJECT
public:
    ChannelStrip(daw::EngineController* controller, const QString& trackId,
                 bool master, QWidget* parent = nullptr);

    const QString& trackId() const { return m_trackId; }
    bool isMaster() const { return m_master; }

    void setSelected(bool selected);
    /// In the mixer the strip stretches when the pane is taller than it needs.
    /// In the inspector it keeps exactly its natural height and the panel
    /// scrolls. Either way it never shrinks below the height at which every
    /// section is fully visible — a short mixer scrolls instead.
    void setStretchable(bool stretchable);
    /// Height at which the whole console (I/O, inserts, sends, pan, fader) is
    /// visible. This is also the strip's minimum.
    int naturalHeight() const { return m_naturalHeight; }
    /// Push meter levels from the UI timer.
    void refreshMeter();
    /// What the strip's fader is *showing*, for the headless check that a level
    /// changed elsewhere reaches it.
    double faderGainForTest() const;
    /// Re-read volume/pan/flags from the document (after undo, load, …).
    void syncFromModel();
    /// While the transport runs, mirror volume/pan automation at the playhead;
    /// while stopped, return to the stored static values.
    void refreshAutomationValues();

signals:
    void selectRequested(const QString& trackId);
    void edited(bool localFileDirty = true);
    /// A send was added/removed — the owner should rebuild this strip.
    void structureChanged();
    /// A loaded insert was clicked: open its editor. The strip does not own
    /// the window, so it only says which slot.
    void editorRequested(const QString& channelId, const QString& insertId);
    void patternRequested(const QString& patternId);
    void removeRequested(const QString& trackId);
    /// The fader or the pan knob requested automation. Not emitted
    /// on the master strip — the master bus is not a track in the document, so
    /// there is nothing to hang a curve on.
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automateSendRequested(const QString& trackId, const QString& sendId);

protected:
    bool eventFilter(QObject*, QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void dragEnterEvent(class QDragEnterEvent*) override;
    void dragMoveEvent(class QDragMoveEvent*) override;
    void dragLeaveEvent(class QDragLeaveEvent*) override;
    void dropEvent(class QDropEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    std::optional<daw::plugins::PluginDescriptor> pluginFromMime(
        const class QMimeData*) const;
    QString presetFromMime(const class QMimeData*) const;
    bool hasBrowserDrop(const class QMimeData*) const;
    void acceptBrowserDrop(const class QMimeData*);
    /// Say plainly that a plugin would not load. Silence here reads as a broken
    /// program rather than as a broken plugin.
    void reportPluginFailure(const daw::plugins::PluginDescriptor& descriptor);

    QWidget* buildHeader();
    QWidget* buildRouting();
    QWidget* buildInserts();
    /// Right-click menu for a loaded insert: bypass, replace, reorder, remove.
    QMenu* buildInsertMenu(QWidget* parent, const QString& insertId,
                           std::size_t index);
    /// The controller-side channel id: the track's uuid, or the reserved
    /// master id. Lets one code path serve both.
    QString channelId() const;
    /// The instrument slot at the head of a MIDI/instrument track's chain.
    /// Returns null for every other kind, which have nothing to put there.
    QWidget* buildInstrument();
    QWidget* buildSends();
    QWidget* buildFaderRow();
    QWidget* buildButtons();
    QWidget* buildNamePlate();
    /// How a well's title behaves as a drag handle, and what the well itself
    /// accepts. Grouped because a well either takes part in strip-to-strip
    /// dragging or does not, and threading five more arguments through
    /// `buildSlotWell` for it would bury the two that matter.
    struct WellDrag {
        /// Dragging the title picks this up (null: the title is just a title).
        const char* dragMime = nullptr;
        QString dragPayload;
        QString titleTip;
        /// Dropped onto the well's empty space. Given the payload and the
        /// modifiers held at the drop, so Alt can mean "copy".
        const char* dropMime = nullptr;
        std::function<void(const QString&, Qt::KeyboardModifiers)> onMimeDrop;
    };

    /// A titled well holding slot rows. With `accepts`/`onDrop` given, the well
    /// also takes a dropped file — how a sample lands on the instrument slot.
    QWidget* buildSlotWell(const QString& title, QWidget* addButton,
                           const std::vector<QWidget*>& rows,
                           std::function<bool(const QString&)> accepts,
                           std::function<void(const QString&)> onDrop,
                           WellDrag drag);
    /// The common case: a plain titled well with no drag or drop behaviour.
    QWidget* buildSlotWell(const QString& title, QWidget* addButton,
                           const std::vector<QWidget*>& rows) {
        return buildSlotWell(title, addButton, rows, {}, {}, WellDrag{});
    }
    /// The Audio FX header's menu: add a plugin, and the copy/paste that moves
    /// a whole chain — or a whole strip — from one channel to another.
    QMenu* buildChainMenu(QWidget* parent);
    /// A plugin slot was dropped at `index` of `channel`'s chain: a reorder
    /// when it came from the same channel, a move (Alt: a copy) when it came
    /// from another one.
    void dropInsertAt(const QString& channel, const QString& payload,
                      std::size_t index, Qt::KeyboardModifiers mods);
    /// Take a dropped chain / send set from another strip.
    void acceptChainDrop(const QString& sourceChannel, bool copy);
    void acceptSendsDrop(const QString& sourceTrackId, bool copy);
    /// Put the built-in sampler in the track's instrument slot with `path`
    /// loaded, as one undo entry.
    void dropSampleOnInstrument(const QString& trackId, const QString& path);
    QToolButton* makeSlotButton(const QString& text, bool active);
    /// Wrap a loaded slot's button in the row that reveals its actions on
    /// hover — bypass, open, replace — the way Logic does. `channel` addresses
    /// the chain the slot lives on and `slotId` the slot inside it; both an
    /// insert and the instrument answer to that pair. `instrument` picks which
    /// half of the plugin list the replace menu offers and which controller
    /// call swaps the plugin in.
    QWidget* buildSlotRow(QToolButton* slot, const QString& channel,
                          const QString& slotId, bool bypassed,
                          bool instrument);
    void populateInputMenu(class QMenu* menu);
    void populateOutputMenu(QMenu* menu);
    void populateAddSendMenu(QMenu* menu);
    void applyTheme();
    void updateReadouts();
    void updateNamePlate(const QString& name, std::uint32_t color);

    daw::EngineController* m_controller = nullptr;
    QString m_trackId;
    bool m_master = false;
    bool m_selected = false;

    ui::FaderWidget* m_fader = nullptr;
    ui::PanKnob* m_pan = nullptr;
    ui::LevelMeter* m_meter = nullptr;
    ui::MsrButton* m_mute = nullptr;
    ui::MsrButton* m_solo = nullptr;
    ui::MsrButton* m_monitor = nullptr;
    QLabel* m_gainLabel = nullptr;
    QLabel* m_panLabel = nullptr;
    QLabel* m_namePlate = nullptr;
    QString m_namePlateStyleKey;
    RoutingField* m_inputButton = nullptr;
    RoutingField* m_outputButton = nullptr;
    ui::IconButton* m_monoButton = nullptr;   // mono (1 ring) / stereo (2 rings)

    /// Values before the first live update of the current physical gesture.
    /// Intermediate pixels never enter history; release commits these once.
    std::optional<float> m_volumeGestureStart;
    std::optional<float> m_panGestureStart;

    /// Height at which every section fits; measured once after the strip is
    /// built, and used as the minimum so nothing is ever squeezed.
    int m_naturalHeight = 0;
    bool m_stretchable = true;
    /// A chain or a send set is being dragged over this strip right now.
    bool m_dropHighlight = false;
    /// True while a plugin or preset from the browser is routed through this
    /// strip (including one of its drop-enabled child wells).
    bool m_browserDropActive = false;
};
