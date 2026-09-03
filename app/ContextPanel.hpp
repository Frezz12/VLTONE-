#pragma once

#include "GlassPanel.hpp"

#include <QPointer>
#include <QString>

#include <functional>

#include <vector>

namespace daw { class EngineController; }
namespace ui { class SelectionModel; }
class PluginQuickAdder;

/// The groups the panel can show, in one list so the panel and the settings
/// page that switches them on and off can't drift apart. `id` is also the
/// QSettings key, under "contextPanel/".
struct ContextTool {
    const char* id;
    const char* context;   // the heading it lives under in settings
    const char* label;
};
const std::vector<ContextTool>& contextPanelTools();

class QAbstractAnimation;
class QPropertyAnimation;
class QHBoxLayout;
class QWidget;

/// The context panel: one floating plate under the tool strip that shows the
/// tools for whatever is selected right now, and nothing else.
///
/// Editing a clip used to mean knowing that its top corners are fade handles
/// and the notch at its bottom is gain. This surfaces those, plus the edits
/// that had no gesture at all, and swaps its whole content when the selection
/// changes kind — with enough motion that the swap is legible rather than a
/// silent redraw.
///
/// The panel is a pure view over `EngineController`. The controller emits
/// nothing, so it re-reads the document whenever the selection model says so —
/// which includes the explicit refresh() after undo, redo and edits made in
/// other views.
class ContextPanel : public ui::GlassPanel {
    Q_OBJECT
public:
    ContextPanel(daw::EngineController* controller, ui::SelectionModel* selection,
                 QWidget* parent = nullptr);

    /// Re-read the document into the current content without rebuilding it.
    void refresh();

    /// The user's View-menu toggle. An off panel stays hidden whatever is
    /// selected.
    void setPanelEnabled(bool enabled);
    bool isPanelEnabled() const { return m_enabled; }

    /// Temporarily yield the shared strip to another context source. The
    /// user's enabled preference is preserved, so releasing the strip restores
    /// the arrangement context without changing settings.
    void setSuppressed(bool suppressed);

    /// Rebuild from scratch — used when the tool profiles change.
    void rebuild();

    /// Re-centre the plate after the arrangement resizes.
    void relayout();

    /// Where the plate should sit, asked rather than pushed.
    ///
    /// Fills `centreX` — in this widget's parent's coordinates — and returns
    /// true when the selection has a horizontal extent. False for a whole
    /// track or the recording options, which have none, and the plate returns
    /// to the middle.
    ///
    /// A callback rather than a setter on purpose: the plate recomputes its
    /// geometry from inside a selection change, and a value pushed afterwards
    /// would arrive one step too late.
    void setAnchorProvider(std::function<bool(int&)> provider);

    /// The stretch of the strip the plate may occupy, in the strip's own
    /// coordinates: the arrangement's own left and right edges. Without it the
    /// plate is free to sit over the track headers, the browser or the
    /// inspector — none of which it says anything about.
    void setBoundsProvider(std::function<bool(int&, int&)> provider);

    /// Re-read the anchor and slide if it moved. Called from the UI tick, so
    /// the plate follows scrolling, zooming and a clip being dragged; it does
    /// nothing when the target has not moved.
    void followSelection();

    /// Re-read `contextPanel/followSelection`. The settings page writes it and
    /// then calls this, so the change is visible without a restart.
    void reloadFollowSetting();

    /// Record is engaged on the transport (pressed, but not yet capturing).
    /// While it is, the panel shows the recording options instead of whatever
    /// is selected — that is the whole point of engaging rather than starting.
    void setRecordEngaged(bool engaged);

    /// Reveal and focus the plugin search for a track or one audio clip. Used
    /// by the global Ctrl+F command as well as the inline search button.
    void openPluginSearch();

protected:
    void resizeEvent(QResizeEvent*) override;

signals:
    void projectEdited(bool localFileDirty = true);
    /// A continuous control on the panel moved — level, pan, a fade. Emitted on
    /// every step of the drag, not only at the end, so the fader in the mixer
    /// and the one in the track header move *with* it. `projectEdited` still
    /// marks the file dirty when the gesture finishes; this one only asks the
    /// other views to re-read the value.
    void liveEdited();
    /// The track set changed (a clip was duplicated onto a new lane, a track
    /// was deleted), so the other views must rebuild rather than repaint.
    void tracksChanged(bool localFileDirty = true);
    /// The panel's Start / Stop chip was pressed. It means whatever R means at
    /// that moment — start a take, cancel a count-in, land a running one —
    /// because MainWindow owns all three and the two gestures must not drift.
    void recordingToggleRequested();
    /// The global Overwrite / Layer-recording setting was changed here. The
    /// panel has already written it through; this is so the status bar and an
    /// open settings window hear about it.
    void recordModeChanged();
    /// Toggle only the selected channel's automation disclosure.
    void automationToggleRequested(const QString& trackId);
    void automateControlRequested(const QString& trackId, bool pan);
    void automateMuteRequested(const QString& trackId);
    void automationEditorRequested(const QString& trackId,
                                   const QString& clipId);
    void midiEditorRequested(const QString& trackId, const QString& clipId);
    void patternEditorRequested(const QString& patternTrackId);
    /// A newly inserted plugin asked for its native editor to be opened.
    void pluginEditorRequested(const QString& channelId, const QString& insertId);

private:
    /// What the panel is showing. `Other` covers a mixed or unsupported
    /// selection and hides the plate.
    enum class Context {
        None,
        Recording,
        AudioClip,
        AudioClipMulti,
        AutomationClip,
        AutomationClipMulti,
        MidiClip,
        MidiClipMulti,
        PatternClip,
        PatternClipMulti,
        Track,
        TrackMulti,
        Other
    };

    Context resolve() const;
    QColor accentFor(Context context) const;

    QWidget* buildContent(Context context);
    QWidget* buildAudioClip();
    QWidget* buildAudioClipMulti();
    QWidget* buildAutomationClip(bool multi);
    QWidget* buildMidiClip(bool multi);
    QWidget* buildPatternClip(bool multi);
    QWidget* buildTrack();
    /// Several tracks at once. Everything on it is a *relative* action —
    /// quieter by a decibel, shift the pan, mute them all — because the tracks
    /// start at different settings and one shared value would flatten them.
    QWidget* buildTrackMulti();
    /// Every selected track, in order. Empty when the selection is not tracks.
    std::vector<std::string> selectedTracks() const;
    QWidget* buildRecording();

    void onSelectionChanged();
    /// Swap in `next`, animating the change: the old content slides out, the
    /// plate springs to the new size, the new controls cascade in. The same
    /// motion whatever changed — clip to clip moves exactly like clip to track.
    void transitionTo(QWidget* next, Context context);
    /// The geometry the plate wants inside its host, given its content.
    QRect targetGeometry() const;
    /// Slide to a new resting place without touching the content. Used when the
    /// selection moves sideways but stays the same kind of thing.
    void driftTo(const QRect& target);
    void layoutSelf();
    /// Clip both animated rows to the painted plate, not to the larger widget
    /// rect that also contains its shadow and top flare.
    void updateContentMasks();

    /// Whether a tool is switched on for this context in the user's profile.
    bool toolEnabled(const char* toolId) const;
    /// A fresh content widget with its horizontal row layout, ready for
    /// controls. Every context is one flat row of icon-sized controls.
    QWidget* newRow(QHBoxLayout*& row);

    /// Apply an edit to every selected clip, then repaint the views once.
    void forEachSelectedClip(const std::function<void(const QString& trackId,
                                                      const QString& clipId)>& fn);
    void afterEdit(bool structural = false, bool localFileDirty = true);

    daw::EngineController* m_controller = nullptr;
    ui::SelectionModel* m_selection = nullptr;

    Context m_context = Context::None;
    QString m_contextKey;        // identity of the object, to spot a same-kind swap
    QWidget* m_content = nullptr;
    QWidget* m_outgoing = nullptr;
    // The running swap. Started with DeleteWhenStopped, so stopping it destroys
    // it — a QPointer rather than a raw one, or interrupting a swap would leave
    // a dangling handle behind.
    QPointer<QAbstractAnimation> m_transition;
    std::function<void()> m_applyValues;   // pushes document → current controls
    bool m_enabled = true;
    bool m_suppressed = false;
    bool m_recordEngaged = false;
    /// Record was just engaged and nothing has been selected since, so the take
    /// settings outrank whatever the selection would otherwise show.
    bool m_recordPinned = false;
    bool m_updating = false;     // suppresses control signals while loading

    /// Where the plate wants its centre, and whether anything asked. Follow mode
    /// is the default: the plate rides above the selected clip along the strip.
    bool m_follow = true;
    std::function<bool(int&)> m_anchorProvider;
    std::function<bool(int&, int&)> m_boundsProvider;
    /// The position-only slide, kept separate from the content swap: the two can
    /// overlap (pick a clip far to the right) and must not fight over geometry.
    QPointer<QPropertyAnimation> m_drift;
    /// The incoming row is riding in and owns its own position, so the plate's
    /// resize must not re-centre it out from under the animation.
    bool m_contentSliding = false;
    QPointer<PluginQuickAdder> m_quickAdder;
};
