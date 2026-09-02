#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QJsonObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <array>
#include <bitset>
#include <memory>
#include <string>
#include <vector>

#include "EngineController.hpp"
#include "SelectionModel.hpp"
#include "UiConstants.hpp"
#include "recovery/RecoveryJournal.hpp"

class QLabel;
class QMenu;
class QResizeEvent;
class QTimer;
class QAction;
class TrackListWidget;
class TimelineWidget;
class MixerWidget;
class InspectorWidget;
class TransportBar;
class ToolPanel;
class ShortcutManager;
class PluginEditorWindow;
class SampleEditorWindow;
class AutomationEditorWindow;
class InternalEditorFrame;
class PluginManagerWindow;
class SettingsWindow;
class PianoRollWindow;
class PatternWindow;
class ContextPanel;
class NoteContextPanel;
class FileBrowserPanel;
class AiChatPanel;
class WebBrowserPanel;
class TypingKeyboard;
class MidiInputManager;
namespace collab {
class CollaborationService;
class CollaborationCommandBridge;
class AssetCache;
class CloudProjectClient;
class CloudProjectAssetHydrator;
class CloudProjectPublisher;
class CloudProjectSyncCoordinator;
class CloudSharedAssetMutationBridge;
class CloudSessionLifecycleController;
class RecordingLeaseCoordinator;
class PresenceInputRouter;
class PresenceOverlay;
class SessionStatusWidget;
enum class SurfaceKind;
struct TransportFrame;
}

/// The application shell. Top to bottom: the transport bar with its centred
/// pill and inset panel toggles, then a row of [inspector | arrangement over
/// mixer], and finally the status bar. Owns the EngineController and drives it
/// from user actions; a refresh timer pulls playhead/meters back for display.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool openDevice = true, QWidget* parent = nullptr,
                        collab::CollaborationService* collaboration = nullptr);
    ~MainWindow() override;

    /// Startup owns the initial incremental scan while this window is still
    /// hidden. These two hooks expose only the scan and its UI refresh, not the
    /// rest of the controller lifecycle.
    daw::PluginManager& pluginManagerForStartup() {
        return m_controller.pluginManager();
    }
    void applyStartupPluginScanResults();

    /// Narrow collaboration seam.  The projection adapter is owned at app
    /// scope (beside CommandGateway) while the engine is owned by this window.
    /// No editing UI should use this hook for ordinary mutations.
    daw::EngineController* collaborationEngineController() noexcept {
        return &m_controller;
    }
    /// Re-read the already projected model without marking a local file dirty
    /// or emitting the legacy projectEdited signal.
    void refreshAfterCollaborationProjection();

#ifdef DAW_ENABLE_COLLABORATION
    /// App-lifetime cloud services are constructed beside the account and
    /// transport services.  The window only owns the publication UI state and
    /// the temporary capture which keeps staged plugin bytes alive.
    void setCloudPublicationServices(
        collab::CloudProjectPublisher* publisher,
        collab::CloudProjectSyncCoordinator* synchronizer,
        collab::CloudSessionLifecycleController* sessionLifecycle,
        collab::CollaborationCommandBridge* commandBridge,
        collab::CloudProjectClient* projectClient,
        collab::RecordingLeaseCoordinator* recordingLeases,
        collab::CloudProjectAssetHydrator* assetHydrator,
        collab::AssetCache* assetCache);
    void setCloudSharedAssetMutationBridge(
        collab::CloudSharedAssetMutationBridge* bridge);
#endif

    /// Populate a few coloured tracks (used for screenshots / demos).
    void populateDemo();

    /// Open a VLT package selected by the File menu, passed on the command
    /// line, or delivered by the operating system after a double-click. Both
    /// the outer `<name>.vlt` package and its inner `Project.vlt` manifest are
    /// accepted so the same project is convenient on macOS, Windows and Linux.
    /// A `.vltt` package is routed to the new-project-from-template flow, so an
    /// operating-system double-click can never make Save overwrite a template.
    bool openProjectPath(const QString& path);

    /// Engage record without starting a take: the transport button lights and
    /// the context panel switches to the recording options. Public so a
    /// screenshot run can show that state, which is otherwise only reachable
    /// by clicking.
    void engageRecord(bool engaged) { setRecordEngaged(engaged); }

    /// Give a layer being recorded into an expanded clip a row of its own in
    /// that clip's take stack, and take it away again when the take lands.
    void syncPendingTakeRows();
    /// Whether any lane is currently making room for such a row.
    bool m_hadRecordingRows = false;

public:
    /// Add `count` plain audio tracks. Screenshot/demo hook: the interesting
    /// arrangement bugs only show up once the tracks outgrow the window.
    void addDemoTracks(int count);
    /// Set the mixer pane's height, or hide it (screenshot hooks — its handle
    /// cannot be dragged from a grab).
    void setMixerHeightForShot(int height);
    void setMixerShownForShot(bool shown) { setMixerVisible(shown); }
    /// Scroll the arrangement down by `px` (screenshot hook — a grab cannot
    /// turn a wheel).
    void scrollArrangement(int px);

    /// Stage a take that is *running*, so the arrangement's growing clip can be
    /// photographed. `mode` is "layers" to punch into the demo's layered clip
    /// (the take is drawn as the next take of that stack, in its colour) or
    /// anything else to record onto empty timeline. A screenshot run has no
    /// audio device, so the capture's clock and envelope are seeded rather than
    /// captured.
    void stageRecordingShot(const QString& mode);

    /// Open the piano roll on the first MIDI clip in the project. Public for
    /// the same reason as the two above: a screenshot run has no other way to
    /// reach a window that normally opens by double-clicking a clip.
    void openFirstMidiClip();
    /// Headless screenshot only: select every note in the open piano roll, so a
    /// grab shows the note context panel rather than the empty state.
    void selectAllNotesForShot();
    void openFirstAudioClip();
    /// Build and open a representative Pattern editor (screenshot hook).
    void openDemoPattern(bool showEditor = true);

    /// Show the plugin manager, creating the window on first use. Public so the
    /// headless screenshot/selftest runs can construct it.
    void openPluginManager(int tab = 0);

    /// Select the first track and open its Context Panel plugin search. A
    /// headless screenshot hook for the expandable quick-adder.
    void openDemoPluginSearch(bool expand = true);

    /// Show one insert's editor. One window per (channel, insert): a second
    /// request raises the window that is already open rather than parenting the
    /// plugin's view twice, which no format allows.
    void openPluginEditor(const QString& channelId, const QString& insertId);
    void openSampleEditor(const QString& trackId, const QString& clipId);
    /// Show one automation clip's editor — the generators, and the three fields
    /// that re-point a curve at something else.
    void openAutomationEditor(const QString& trackId, const QString& clipId);

    /// Automate `target`, or reveal the curve that already does, and put the
    /// arrangement where it can be seen. Every "double-click a knob" in the
    /// program lands here, so one gesture has one meaning.
    void automateTarget(const daw::AutomationTarget& target);

    /// Headless check only: load the first scanned plugin whose name contains
    /// `nameFragment` onto the first track and open its editor. False when
    /// nothing matched.
    bool openDemoPluginEditor(const QString& nameFragment);

    /// Headless check only: put the built-in sampler in the instrument slot of
    /// the first track that takes notes, optionally load `samplePath` into it,
    /// and open its editor. False when the project has no such track.
    bool openDemoSampler(const QString& samplePath = {});

    /// Headless check only: add the built-in Gravity effect to the first audio
    /// track and open its branded editor.
    bool openDemoGravity();
    bool checkGravityPanelForTest();
    void resizeGravityForShot();

    /// Headless/screenshot check for the built-in VLT Equalizer editor.
    bool openDemoEqualizer();
    bool checkEqualizerPanelForTest();
    void resizeEqualizerForShot();

    /// Headless check only: switch the typing keyboard on, press and release a
    /// note key through the real event path, and report whether it sounded and
    /// then stopped. Also checks that the keys it borrows stay reported as
    /// bound while it holds them and come back afterwards. False when the
    /// project has no track to play, so the caller has to give it one first.
    ///
    /// It deliberately exits with the keyboard on and a key still held, so the
    /// teardown path for a note that outlives the window is exercised too.
    bool checkTypingKeyboard();
    /// Headless check for hardware-style MIDI parsing, source overlap and the
    /// Piano Roll's live-key state; no physical device is required.
    bool checkMidiInput();

    /// Headless check: choosing the built-in Sampler from an empty instrument
    /// slot must emit the editor request for the slot that was just created.
    bool checkPluginAutoOpenForTest();

    /// Headless check: the plugin picker focuses its search on open and keeps
    /// routing printable keys there after a category submenu takes focus.
    bool checkPluginSearchFocusForTest();

    /// Headless check: send the physical B key as the Cyrillic И produced by a
    /// Russian layout and prove a Ctrl/Cmd shortcut still fires.
    bool checkLayoutIndependentShortcuts();

    /// Headless check only: drop `paths` onto the arrangement through the real
    /// drag-and-drop events, as the browser and the desktop both do, and report
    /// whether the project grew by `expectedClips` clips. The only way to
    /// exercise the drop routing without a hand on a mouse.
    bool checkFileDrop(const QStringList& paths, int expectedClips);

    /// Put the browser on the left of the inspector or on the far right. The
    /// inspector stays glued to the arrangement either way, because it is the
    /// selected track's strip and belongs beside its lanes.
    ///
    /// Public so a screenshot run can grab both layouts — which is also why
    /// `persist` exists: a headless grab must not rewrite the user's settings.
    void setBrowserOnLeft(bool onLeft, bool persist = true);

    /// Open the unified settings window on the given tab (see
    /// `SettingsWindow::Tab`), reusing the single instance. Public so the
    /// headless runs can build every page.
    void openSettings(int tab = 0);

    /// Headless check only: point the browser at `folder` and select
    /// `selectFile` inside it, so a screenshot shows a real tree and a real
    /// waveform. Nothing is written to the user's settings.
    /// Headless check only: open the browser on its plugin folders with the
    /// first plugin selected. False when nothing has been scanned.
    bool openDemoBrowserPlugins();
    bool openDemoBrowser(const QString& folder, const QString& selectFile = {});

    /// Headless check only: exercise the browser end to end — select a file
    /// (which decodes on a worker and starts an audition), search, and flip the
    /// panel from one side of the window to the other. False when a step did
    /// not do what it claims.
    bool checkBrowser(const QString& folder, const QString& audioFile);

    /// Headless check only: run a whole assistant turn against a scripted
    /// stand-in for a provider — no key, no network — and report whether the
    /// tool calls really reached the document and folded into one undo entry.
    bool checkAiAssistant();

    /// Headless check only: run one music request against a stand-in that
    /// answers with a local file — no server — and report whether the audio
    /// reached a new track and folded into one undo entry.
    bool checkAiMusic();

    /// Push the current document into the recovery journal and wait until it is
    /// on disk. Headless only: the crash test needs a known state to be durable
    /// before it faults, rather than racing the one-second sampling timer.
    /// Returns false when no journal is running.
    bool flushRecoveryForTest();
    /// Screenshot hook: select a track so the context panel shows its channel
    /// tools, then open the level or pan flyout. `which` matches the control's
    /// tooltip ("volume" / "pan").
    bool openDemoTrackFlyout(const QString& which);
    /// Screenshot hook: pop up the plugin picker over the arrangement, with
    /// whatever the machine's scan actually found in it.
    bool openDemoPluginMenu(bool instruments, const QString& query = {});
    /// Headless check: a level changed on the context panel has to reach the
    /// mixer strip and the track header *as it moves*, not when the strip is
    /// next rebuilt. Returns false with a reason on stderr.
    bool checkContextSyncForTest();
    /// Add a track and prove the header column did not shrink to fit: every row
    /// must still be exactly its lane's height, and sit exactly where that lane
    /// starts. This is the check the "tracks squeeze when I add one" bug would
    /// have failed — it was invisible to every headless test, because it lived
    /// in a layout.
    bool checkTrackRowHeightsForTest();
    /// Drive real clicks with real modifiers over the header rows: a plain
    /// click replaces the selection, shift extends from the anchor, ctrl/cmd
    /// toggles one row — then make a summing folder out of what came out and
    /// check that its tracks really were routed into it. Every step is a
    /// gesture rather than an API call, so the modifier handling is checked the
    /// way a user meets it.
    bool checkTrackSelectionForTest();
    /// Ask which kind of folder, then make it out of whatever is selected. The
    /// single entry point behind the Track menu, the Cmd+Shift+D shortcut and
    /// the screenshot hook that photographs the dialog.
    void onNewFolder();
    /// Headless check only: give the demo's first audio track a volume
    /// automation lane with a shaped curve drawn on it, so a grab shows the
    /// lane, the clip and the three segment shapes at once.
    void stageAutomationForShot();
    /// Headless screenshot only: stage the automation curve above and open its
    /// editor, so a grab shows the editor doing its job. `mode` of "select"
    /// also drags a range out, which is the only way to see what a selection
    /// looks like in a still.
    void openAutomationEditorForShot(const QString& mode = QString());
    /// Headless check only: arm a cycle over the given seconds, so a grab shows
    /// the region on the ruler and washed over the lanes.
    void setCycleForShot(double fromSeconds, double toSeconds, bool armed);
    /// Select tracks by name (comma-separated, matched case-insensitively on a
    /// prefix), so a grab can show a multi-track selection and the colour it is
    /// washed with. The last one named becomes the primary.
    void selectTracksForShot(const QString& names);
    /// Build a chain of folders `depth` deep with a track at the bottom, so a
    /// grab shows what indentation does to a header row: the pan goes, then the
    /// fader, and the chips never do.
    void nestDemoTracksForShot(int depth);
    /// Drive an actual middle-button drag through the timeline and prove it
    /// moves both time and the track stack without an edit tool being involved.
    bool checkTimelinePanForTest();
    /// Exercise multi-lane clip movement, Shift-add/duplicate and marquee
    /// auto-scroll with real mouse events.
    bool checkTimelineClipGesturesForTest();
    /// Drag a cycle region out of the ruler's top strip with real mouse events,
    /// then arm it the way C does, and prove the playhead is held inside it.
    /// The strip, the snap, the two-step arming and the transport's jump-in are
    /// all gesture-level and invisible to the controller tests.
    bool checkCycleRegionForTest();
    /// Open a track's automation with the A command, then draw on the curve
    /// with real mouse events: add a point, drag it, and bend the run it sits
    /// on with Alt. All of it is gesture handling that no controller test can
    /// see, and all of it has to land as one undo entry per gesture.
    bool checkAutomationForTest();
    /// The automation editor: the three re-target fields, and the transforms
    /// that act through them. Gesture-level, so the headless controller tests
    /// cannot reach it.
    bool checkAutomationEditorForTest();
    /// A plain double-click resets a parameter; Alt/Option+double-click (or the
    /// latched toolbar mode) makes automation for it.
    bool checkKnobAutomationForTest();
    /// The plugin editor's parameter dock: that it widens the window instead of
    /// cropping the plugin, and that the parameter the plugin itself moves
    /// rises to the top marked active.
    bool checkParameterDockForTest();
    /// Prove the settings shell stays screen-bounded and every tab can scroll
    /// independently instead of growing the top-level window.
    bool checkSettingsViewportForTest();
    /// Build the render dialog and prove it lists every channel the engine
    /// gives a strip to, and offers no format this build cannot actually write.
    bool checkExportDialogForTest();
    /// Show the render dialog non-modally, for a screenshot: `exec()` would
    /// block the grab timer that is meant to photograph it.
    void openExportDialogForShot(bool stems = false);
    /// Emit the tempo editor's live text signal and prove the project (and
    /// therefore the grid) sees the new BPM before editing finishes.
    bool checkLiveTempoForTest();
    /// Drive the BPM field through a real vertical scrub, then double-click it
    /// and prove the normal text-entry mode is still available.
    bool checkTempoScrubForTest();
    /// Verify that application-owned modeless editors all remain bounded
    /// children of the workspace and hand focus back to the arrangement.
    bool checkAuxiliaryWindowPolicyForTest();
    bool checkPianoRollForTest();
    /// The Edit menu's chords with the piano roll in front: they must reach the
    /// notes, never the clips behind them.
    bool checkEditChordRoutingForTest();

    /// Headless counterpart of the recovery prompt: find the newest leftover
    /// session under DAW_RECOVERY_ROOT and load it through the same code the
    /// dialog uses. Returns the number of tracks recovered, or -1 if there was
    /// nothing to recover or it could not be read.
    int checkRecovery();

    /// Select demo clips by index so the context panel can be grabbed above
    /// them: "0" is the leftmost clip, "0,1" a group. Screenshots only.
    /// `settle` puts the plate where it belongs at once; pass false to leave a
    /// swap animating, so a screenshot can catch it mid-flight.
    void selectDemoClipsForShot(const QString& indices, bool settle = true);

    /// Show the recovery prompt over a plausible made-up session, without
    /// touching any real one. Screenshots only — it never restores anything.
    void openDemoRecoveryPrompt();

    /// End this run's recovery session cleanly. Headless modes return from
    /// main() without an event loop, so the aboutToQuit hook that normally does
    /// this never fires and they would leave a session behind pretending to be
    /// a crash.
    void endRecoverySessionForTest();
    /// Headless check only: open the assistant. With `withTranscript` it runs a
    /// scripted turn first, so a screenshot shows the panel doing its job;
    /// without, it shows the state a user meets before a key is set.
    void openDemoAi(bool withTranscript = true);
    /// Diagnostic only: load one plugin into a slot, open its editor, replace
    /// it with another and open that one's editor — the sequence a user runs by
    /// hand when they say "after Replace the plugin will not even open".
    /// Prints what happened at each step; true when every step held.
    bool probePluginEditorSwap(const QString& firstName, const QString& secondName);

    /// Headless check only: open the assistant in its music mode with a
    /// finished generation in the transcript.
    void openDemoAiMusic();

    /// Headless check only: fill slots so the loaded-slot rendering can be
    /// grabbed — two inserts on the first track and, if a scanned instrument
    /// exists, the instrument slot of the first track that takes notes. No
    /// editor is opened; the point is the strip, not the plugin.
    bool loadDemoPluginSlots();

    /// Headless/browser verification: feed a downloaded audio fixture through
    /// the real probe and import prompt path, choosing a new track without UI.
    bool checkWebBrowserForTest(const QString& audioFile);
    /// Screenshot hook: open the browser on its remembered/home page.
    void openWebBrowserForShot();

    /// Privacy-bounded aggregate used by the external telemetry reporter.
    /// Contains no project name, file paths, audio, MIDI or user-entered text.
    QJsonObject telemetrySnapshot();
    QString recoverySessionDir() const {
        return QString::fromStdString(m_journal.sessionDir());
    }

protected:
    /// Remembers the window's size and position on the way out.
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onPlayPause();
    void onStop();
    /// The transport's Record button. It only ever engages — the take is set
    /// up in the context panel that appears, and started from there or with R.
    void onRecord();
    /// The R key. Starts the recording when record is engaged (or the
    /// preference lets R arm and start by itself), and stops one in flight.
    void onRecordKey();
    void onReturnToStart();
    void onNudge(int bars);
    void onToggleLoop(bool on);
    void onToggleMetronome(bool on);
    void onAddAudioTrack();
    void onAddMidiTrack();
    void onAddInstrumentTrack();
    void onAddPatternTrack();
    void onAddBusTrack();
    void onDuplicateSelectedTrack();
    void onRemoveSelectedTrack();
    void onImportAudio();
    void onExport();
    void onNewProject();
    void onNewProjectFromTemplate();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onSaveProjectTemplate();
    void onUndo();
    void onRedo();
    void onTempoChanged(double bpm);
    void onSelectionChanged(const QString& trackId);
    /// A track was picked in the header column or the mixer — as opposed to
    /// being selected as a side effect of clicking one of its clips.
    void selectTrackFromHeader(const QString& trackId);
    void onTracksChanged(bool localFileDirty = true);
    void onClearSolos();
    void onClearMutes();
    /// Open (or close) the selected tracks' automation lanes. The first time a
    /// track is asked, it gets a volume lane with a curve on it — the default
    /// the request asked for, and the one nobody has to go looking for.
    void toggleAutomationLanes();
    /// Expand/reveal or collapse automation for every eligible channel. The
    /// checked global button calls the same command in both directions.
    void setAllAutomationLanesVisible(bool visible);
    /// Zoom whichever view has the keyboard: the browser when the focus is
    /// inside it, the arrangement otherwise. `direction` is +1 in, −1 out.
    void zoomFocusedView(double direction);
    /// Make a folder of the selection without asking — the two context-menu
    /// items that already say which kind they mean.
    void packSelectionIntoFolder(bool summing);
    /// Everything selected in the header column, in display order; falls back
    /// to the primary track when the column has not been touched.
    QStringList selectedTrackIds() const;
    /// Global S/M act on the selected channels even while an editor window has
    /// focus. Returns false when there is no usable selection.
    bool toggleSelectedTrackState(bool solo);
    /// Tell the header column whether a recording is pending and where it
    /// would land, so the record chips can show it.
    void refreshRecordChips();
    /// Pin (or un-pin) a track as a record target. Pins outrank the selection
    /// and survive record being switched off and back on.
    void setRecordPinned(const QString& trackId, bool pinned);
    void onDetachMixer();
    void onDockMixer();
    void refreshUi();
    /// Repaint only moving readouts and cursor slivers at display cadence.
    void refreshPlayheadFrame();
    void syncPlayheadTimer();

protected:
    /// Held-key gestures the menu shortcuts cannot express: A auditions a take
    /// while it is down, and Ctrl+Shift+L inverts the recording mode only for as
    /// long as it is held. Installed on the application so they work whatever has
    /// focus, and both are released on focus loss so a key cannot get stuck down.
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    /// Register a non-modal editor/panel that belongs above the shell while it
    /// is in use. A timeline press deliberately lowers this whole family; an
    /// explicit reopen (for example, clicking a MIDI clip) presents it again.
    /// One of the arrangement's Edit-menu chords, as it arrived from the menu
    /// bar rather than from a key event.
    enum class EditChord { Cut, Copy, Paste, Repeat, Mute, Delete };

    /// Send an edit chord where it actually belongs, and say whether the
    /// arrangement should keep its hands off.
    ///
    /// On macOS the menu bar is the **system** menu bar: Cocoa fires a menu
    /// item's key equivalent itself, before Qt delivers the key anywhere, and
    /// without sending the `ShortcutOverride` a focused editor would otherwise
    /// claim. So Cmd+X/C/V/B pressed in the piano roll reached the arrangement
    /// and cut clips instead of notes. The action asks this first: another
    /// editor window in front means the chord was never meant for the clips.
    bool routeEditChord(EditChord chord);

    /// Put an application-owned modeless surface inside the shared workspace
    /// chrome. System/modal dialogs deliberately keep their native semantics.
    InternalEditorFrame* hostInternalWindow(QWidget* content,
                                            const QString& settingsKey);
    void presentInternalWindow(QWidget* content);
    void hideInternalWindow(QWidget* content);
    void closeInternalWindows();
    bool hasActiveInternalWindow() const;

    void registerAuxiliaryWindow(QWidget* window);
    void presentAuxiliaryWindow(QWidget* window);
    void raiseAuxiliaryWindows();
    void lowerAuxiliaryWindowsForWorkspace();
    void closeAuxiliaryWindows();
    /// Apply the held state of the two modifier gestures above.
    void setAuditionHeld(bool held);
    void setLayerInvertHeld(bool held);
    /// Alt/Option is the momentary half of automation creation; the toolbar
    /// button is the latched half. Both feed one effective state.
    void setAutomationModifierHeld(bool held);
    void updateAutomationCreationMode();

    /// The tracks a recording would land on: whatever is selected, falling back
    /// to the first recordable track.
    std::vector<std::string> recordTargets() const;
    /// Start a take on whatever is selected — after the count-in when one is
    /// switched on. Reports what it is recording onto, or why it could not.
    void startRecordingSelection();
    /// Drive the count-in from its own timer, and start the take when it runs
    /// out. The controller counts; this only feeds it the elapsed time.
    void tickCountIn();
    /// Drop a count-in that has not reached the take yet.
    void cancelCountIn();
    /// Shared tail of both ways in: light the transport, name the tracks in the
    /// status bar, and let the headers catch up with the arming.
    void announceRecordingStarted(const std::vector<std::string>& targets);
    /// Stop, land the takes, and open the comp editor when the preference asks.
    void stopRecordingNow();
    /// Record engaged: lit but not yet rolling.
    void setRecordEngaged(bool engaged);
    /// Switch the typing keyboard on or off, keeping the button, the menu item
    /// and the keyboard itself saying the same thing.
    void setTypingKeyboardEnabled(bool enabled);
    /// Resolve the shared live-input destination for typing and hardware MIDI.
    std::string liveInputTarget() const;
    void onLiveNoteStateChanged(const QString& trackId, int pitch, bool down);
    std::bitset<128> livePitchesForTrack(const QString& trackId) const;
    /// Lend the typing keyboard's keys to it while it is on, and take them
    /// back afterwards. Necessary because on macOS the menu bar is the
    /// system's: AppKit fires a key equivalent before any Qt event filter is
    /// asked, so E would expand take layers instead of playing a note however
    /// carefully the keyboard swallows the key. The bindings themselves are
    /// untouched — see ShortcutManager::setKeySuppressor.
    void suppressTypingKeyConflicts(bool suppress);
    /// A MIDI file just landed on the arrangement. Says what it holds, and —
    /// when the file's tempo differs from the project's — offers to adopt it,
    /// with a way to stop being asked. The notes themselves are already in
    /// beats and play correctly either way; this is only about the grid.
    void onMidiFileImported(const QString& path, double fileTempoBpm,
                            bool hasTempoChanges);
    /// Shift+L: flip Overwrite ↔ Layer recording. The mode has one home, the
    /// recording panel — this is the keyboard way into the same setting.
    void onToggleLayerMode();
    /// The mode changed (here or on the panel): say so, and keep an open
    /// settings window in step.
    void onRecordModeChanged();

    void buildMenus();
    /// Register stable, non-menu actions for permanent toolbar controls so the
    /// shortcut editor and assistant use the same handlers as their buttons.
    void buildSemanticCommands();
    /// Create a menu action, wire its shortcut through the ShortcutManager under
    /// `id`, and return it so the caller can connect triggered/toggled.
    QAction* addCommand(QMenu* menu, const QString& id, const QString& text,
                        const QString& category, const QKeySequence& def = {});

    /// Close editor windows whose slot no longer exists in the document.
    void closeOrphanedPluginEditors();
    /// The controller's warning that a slot's plugin is about to be destroyed.
    void retirePluginEditor(const QString& channelId, const QString& insertId);
    /// Show the piano roll for a MIDI clip, creating the window on first use.
    void openPianoRoll(const QString& trackId, const QString& clipId);
    /// Show the compact editor for a Pattern container.
    void openPattern(const QString& patternId);
    bool canOpenSelectedEditor() const;
    void openSelectedEditor();
    /// Set the arrangement edit tool (0 Select, 1 Knife, 2 Eraser, 3 Region)
    /// and keep the transport chip in sync — used by the tool keyboard
    /// shortcuts.
    void setEditTool(int index);
    void buildLayout();
    void buildStatusBar();
    void publishSessionTransport(bool force = false);
    void applySessionTransport(const collab::TransportFrame& frame);
    void updateWindowTitle();
    void syncViews();
    /// Rebuild the lane/header structure in the current frame, then leave
    /// expensive channel-strip reconstruction for the next paint boundary.
    void syncStructureViews();
    void scheduleDeferredStructureRefresh();
    void markDirty();
    /// Where the selected clips are, in the tool strip's coordinates, so the
    /// context panel can ride above them. False when the selection has no
    /// horizontal extent.
    bool contextPanelAnchor(int& centreX) const;
    /// How far the context plate may travel: the arrangement's left and right
    /// edges in the tool strip's coordinates.
    bool contextPanelBounds(int& left, int& right) const;
    /// Start the crash-recovery journal, after offering back any work an
    /// earlier session left behind. Skipped in headless runs unless
    /// DAW_RECOVERY_ROOT points somewhere disposable.
    void startRecovery(bool interactive);
    /// Hand the journal the current document if anything changed since the last
    /// sample. Driven by a timer rather than by markDirty() directly: an edit
    /// can fire on every mouse move, and the journal copies the whole document.
    void sampleForRecovery();
    void setMixerVisible(bool visible);
    void setInspectorVisible(bool visible);
    /// Show or hide the file browser, remembering the choice.
    void setBrowserVisible(bool visible);
    /// Create the Chromium surface on first use, then keep it alive while
    /// hidden so downloads are not cancelled.
    void ensureWebBrowser();
    void setWebVisible(bool visible, bool persist = true);
    void applyRightPanelWidths();
    /// Apply the preferred track-header width against the live arrangement
    /// bounds, and keep the tool strip aligned with it.
    void applyTrackHeaderWidth();
    /// Keep the global automation button in step with per-track toggles, undo
    /// and project loading without emitting a second command.
    void syncAutomationVisibilityButton();
    void showNextDownloadedAudioPrompt();
    /// Analyze the source range heard from one arrangement clip, persist the
    /// result on that clip, and optionally apply a user-confirmed BPM.
    void analyzeAudioClip(const QString& trackId, const QString& clipId,
                          bool detectTempo, bool detectKey);
    /// Show or hide the AI assistant, remembering the choice. Off by default —
    /// it is opt-in, and it spends the user's money. `persist` is false for the
    /// headless hooks: a screenshot run must not rewrite the user's settings.
    void setAiVisible(bool visible, bool persist = true);
    /// Lay the main row out for the current side. The only place that decides
    /// the column order, so the two sides cannot drift apart.
    void relayoutRow();
    /// Keep the floating mixer overlay pinned to the bottom of the arrangement,
    /// sized to m_mixerHeight.
    void layoutMixer();
    /// Keep the context panel centred at the top of the arrangement.
    void layoutContextPanel();
    /// Give the shared context strip to selected Piano Roll notes while the
    /// internal editor is active, otherwise return it to the arrangement.
    void syncPianoRollContextPanel();
#ifdef DAW_ENABLE_COLLABORATION
    void registerPianoRollPresence();
    void registerAutomationPresence(AutomationEditorWindow* editor);
    void registerNormalizedPresenceSurface(QWidget* surface,
                                           collab::SurfaceKind kind,
                                           const QString& instanceId,
                                           const QString& ownerId,
                                           const QString& objectId);
    void onPublishCloudProject();
    void onOpenCloudProjects();
    void onInvitePeople();
    void onSessionSettings();
    bool openCloudProject(const QString& projectId,
                          const QString& localBackupPath = {});
    void persistCloudBinding();
    void restoreCloudBinding();
    void onMakeLocalCopy();
    void updateCloudPublicationAction();
    void updateCloudSessionActions();
    bool canInvitePeople() const;
    bool canOpenSessionSettings() const;
    void clearCloudProjectBinding(bool cancelPublication);
    void startCloudRecording(const std::vector<std::string>& targets);
    void stopCloudRecordingNow(bool interactiveError = true);
    void cancelPendingCloudRecording(const QString& safeNotice = {});
    void handleCloudRecordingContextChange();
    void handleRecordingLeasesAcquired();
    bool prepareCloudRecordingForProjectTransition();
    bool cloudRecordingContextIsWritable(
        const std::vector<std::string>& targets) const;
    bool ensureCloudRecordingRecoverySession();
    bool cloudRecordingRecoveryExists() const;
#endif
    void setContextPanelVisible(bool visible);
    bool doSave(const QString& packageDir);
    bool saveProjectAs();
    bool maybeSaveChanges();
    QString chooseProjectTemplate();
    bool createProjectFromTemplatePath(const QString& packageDir);
    void addProjectTemplateTracks(const QString& packageDir);

    daw::EngineController m_controller;
    /// What is selected, in one place, for the views that need to know rather
    /// than the view that made the selection. See ui::SelectionModel.
    ui::SelectionModel m_selection;

    /// Non-owning app-lifetime service; instantiated only by feature-enabled
    /// builds. The window owns the input router/overlays/status widget.
    collab::CollaborationService* m_collaboration = nullptr;
    collab::PresenceInputRouter* m_presenceInput = nullptr;
    collab::PresenceOverlay* m_timelinePresence = nullptr;
    collab::PresenceOverlay* m_pianoRollPresence = nullptr;
    collab::SessionStatusWidget* m_sessionStatus = nullptr;
#ifdef DAW_ENABLE_COLLABORATION
    struct PublicationUiState;
    std::unique_ptr<PublicationUiState> m_publicationUi;
    collab::CloudProjectPublisher* m_cloudPublisher = nullptr;
    collab::CloudProjectSyncCoordinator* m_cloudProjectSync = nullptr;
    collab::CloudSessionLifecycleController* m_cloudSessionLifecycle = nullptr;
    collab::CollaborationCommandBridge* m_collaborationCommandBridge = nullptr;
    QPointer<collab::CloudProjectClient> m_cloudProjectClient;
    QPointer<collab::CloudProjectAssetHydrator> m_cloudAssetHydrator;
    QPointer<collab::CloudSharedAssetMutationBridge>
        m_cloudSharedAssetMutationBridge;
    QPointer<collab::AssetCache> m_collaborationAssetCache;
    QPointer<collab::RecordingLeaseCoordinator> m_recordingLeases;
    struct CloudRecordingRuntime;
    std::unique_ptr<CloudRecordingRuntime> m_cloudRecording;
    quint64 m_cloudRecordingGeneration = 0;
    bool m_preserveCloudRecoverySession = false;
    QAction* m_publishProjectAction = nullptr;
    QAction* m_cloudProjectsAction = nullptr;
    QAction* m_startSessionAction = nullptr;
    QAction* m_invitePeopleAction = nullptr;
    QAction* m_sessionSettingsAction = nullptr;
    QAction* m_endSessionAction = nullptr;
    QAction* m_leaveSessionAction = nullptr;
    QAction* m_retryAssetMutationAction = nullptr;
    QAction* m_cancelAssetMutationAction = nullptr;
    QString m_cloudProjectId;
    QString m_candidateCloudProjectId;
    QString m_candidateCloudBackupPath;
    /// The local package published into the cloud is retained as an immutable
    /// backup.  Save/Save As may create a different local copy, but may never
    /// target this path while the document remains cloud-bound.
    QString m_cloudLocalBackupPath;
#endif
    bool m_applyingRemoteTransport = false;
    bool m_transportBroadcastInitialized = false;
    bool m_lastTransportBroadcastPlaying = false;

    TransportBar* m_transport = nullptr;
    QWidget* m_workspace = nullptr;
    ToolPanel* m_toolPanel = nullptr;
    TrackListWidget* m_trackList = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MixerWidget* m_mixer = nullptr;
    InspectorWidget* m_inspector = nullptr;
    ContextPanel* m_contextPanel = nullptr;
    NoteContextPanel* m_noteContextPanel = nullptr;
    FileBrowserPanel* m_browser = nullptr;
    QWidget* m_browserHandle = nullptr;
    QWidget* m_webContainer = nullptr;
    WebBrowserPanel* m_webPanel = nullptr;
    QWidget* m_webHandle = nullptr;
    AiChatPanel* m_aiPanel = nullptr;
    QWidget* m_aiHandle = nullptr;
    /// The row holding browser | inspector | arrangement | assistant. Kept
    /// because the browser can move from one end of it to the other.
    QHBoxLayout* m_rowLayout = nullptr;
    /// The outermost row: the workspace, the assistant's drag handle and the
    /// assistant. Held so the gap at the window's right edge can be matched to
    /// the one the handle leaves on the panel's other side.
    QHBoxLayout* m_shellLayout = nullptr;
    /// The main body row also acts as the clipping/stacking surface for
    /// internal editor frames. They are children, but not layout items.
    QWidget* m_editorHost = nullptr;
    QWidget* m_arrangementHost = nullptr;
    QWidget* m_trackHeaderHandle = nullptr;
    QWidget* m_mixerHandle = nullptr;
    QWidget* m_mixerWindow = nullptr;

    QLabel* m_statusLeft = nullptr;
    QLabel* m_statusRight = nullptr;
    QAction* m_showMixerAction = nullptr;
    QAction* m_showInspectorAction = nullptr;
    QAction* m_showContextPanelAction = nullptr;
    QAction* m_showBrowserAction = nullptr;
    QAction* m_showWebAction = nullptr;
    QAction* m_showAiAction = nullptr;
    QTimer* m_refreshTimer = nullptr;
    /// Lightweight 60-ish Hz cursor clock. It sleeps while transport is still.
    QTimer* m_playheadTimer = nullptr;
    /// Automation controls need 30 Hz updates only while the transport runs.
    /// On the first stopped tick they return to their document values once.
    bool m_wasAutomationPlaying = false;
    /// The count-in's own clock: finer than the UI refresh, and running only
    /// while a count is in flight.
    QTimer* m_countInTimer = nullptr;
    QElapsedTimer m_countInClock;
    ShortcutManager* m_shortcuts = nullptr;
    /// The computer keyboard as a MIDI keyboard. Lives here rather than in a
    /// window because it filters the whole application: a key plays a note
    /// whichever window has focus.
    TypingKeyboard* m_typingKeyboard = nullptr;
    /// Every physical MIDI input, hot-plugged and marshalled onto the UI thread.
    MidiInputManager* m_midiInput = nullptr;
    QHash<QString, std::array<unsigned int, 128>> m_livePitchCounts;
    QAction* m_typingKeyboardAction = nullptr;
    SettingsWindow* m_settingsWindow = nullptr;
    PluginManagerWindow* m_pluginManagerWindow = nullptr;
    /// Open editors, keyed by "<channelId>/<insertId>". Not a single instance:
    /// several plugins are routinely open side by side.
    QHash<QString, PluginEditorWindow*> m_pluginEditors;
    QHash<QString, SampleEditorWindow*> m_sampleEditors;
    QHash<QString, class AutomationEditorWindow*> m_automationEditors;
    PianoRollWindow* m_pianoRoll = nullptr;
    InternalEditorFrame* m_pianoRollFrame = nullptr;
    PatternWindow* m_patternWindow = nullptr;
    /// Every application-owned modeless window now uses InternalEditorFrame.
    /// The content pointer remains the feature registry's identity; the frame
    /// owns geometry, stacking and custom chrome.
    QHash<QWidget*, InternalEditorFrame*> m_internalEditorFrames;
    /// Deterministic routing owner used only while the self-test drives menu
    /// actions. Native window activation is asynchronous (and unavailable on
    /// some CI desktops), so it cannot be an assertion precondition.
    QPointer<QWidget> m_editChordRouteWindowForTest;
    /// Logical focus companion to the window override. A null value while the
    /// window override is set deliberately means "no text widget has focus".
    QPointer<QWidget> m_editChordFocusWidgetForTest;
    QList<QPointer<QWidget>> m_auxiliaryWindows;
    bool m_auxiliaryLowered = false;
    bool m_auxiliaryRaiseQueued = false;
    bool m_structureRefreshPending = false;
    /// The expensive safety sweep over open plugin/sample editors is needed
    /// only after document structure can have changed, never on every UI tick.
    bool m_orphanEditorSweepPending = false;
    /// False in the headless screenshot/selftest runs, which size themselves.
    bool m_persistGeometry = true;

    QString m_projectPath;
    QString m_selectedTrackId;
    bool m_dirty = false;
    /// Monotonic identity of document content.  Publication uses this in
    /// addition to the dirty bit so an edit followed by Save cannot make an
    /// old staged snapshot look current again.
    quint64 m_projectRevision = 0;
    /// Continuous, cheap copy of the document on disk — the only thing that
    /// actually survives a crash. Declared after m_controller so it is torn
    /// down before the engine it snapshots.
    daw::recovery::RecoveryJournal m_journal;
    QTimer* m_journalTimer = nullptr;
    /// The write end of the watchdog's parent-death pipe. Held open for the
    /// life of the process and deliberately never written to: its closing —
    /// which happens however this process dies — is the signal.
    int m_guardPipe = -1;
    /// Set by markDirty(), cleared when the journal has been handed a copy.
    bool m_journalStale = false;
    /// Content-addressed filenames from the last plugin recovery generation.
    /// While an editor is open these let us notice opaque preset/state changes
    /// that the plugin did not announce, without rewriting an unchanged journal.
    std::vector<std::string> m_recoveryPluginStateFiles;
    double m_recoveryDspPeak = 0.0;
    double m_telemetryDspPeak = 0.0;
    bool m_playFromClip = false;   // Space loops the selected clip
    // Held-key gestures (see eventFilter).
    bool m_auditionHeld = false;
    bool m_layerInvertHeld = false;
    bool m_automationModifierHeld = false;
    bool m_automationCreationLatched = false;
    // Global record engage: the transport is armed and R will start a take.
    bool m_recordEngaged = false;
    /// Tracks explicitly armed with their R chip. While this is non-empty it
    /// *is* the record target — the selection stops deciding, so picking
    /// another track to look at cannot move a take onto it. Emptied only by
    /// un-pinning, so switching record off and on again comes back to the same
    /// tracks.
    QStringList m_recordPins;
    double m_playRangeStart = -1.0;  // From-clip loop range, −1 = off
    double m_playRangeEnd = -1.0;
    int m_mixerHeight = 545;
    int m_mixerDragStartHeight = 545;
    int m_trackHeaderWidth = ui::kTrackHeaderWidth;
    int m_trackHeaderDragStartWidth = ui::kTrackHeaderWidth;
    bool m_browserOnLeft = true;
    int m_browserWidth = 240;
    int m_browserDragStartWidth = 240;
    int m_webWidth = 520;
    int m_webDragStartWidth = 520;
    int m_aiWidth = 320;
    int m_aiDragStartWidth = 320;

    struct PendingAudioDownload {
        QString path;
        double durationSeconds = 0.0;
        double sampleRate = 0.0;
        int channels = 0;
    };
    QQueue<PendingAudioDownload> m_downloadedAudio;
    bool m_audioImportDialogOpen = false;
    /// 0 = normal modal UI, 1 = New Track. Only set by the headless hook.
    int m_audioImportChoiceForTest = 0;
};
