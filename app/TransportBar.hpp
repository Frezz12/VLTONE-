#pragma once

#include <QList>
#include <QWidget>

#include <cstddef>

namespace daw { class EngineController; }

class QAction;
class QLabel;
class QLineEdit;
class QToolButton;
class SpectrumMeter;
namespace ui { class IconButton; }

/// The top chrome: one centred three-part cluster. Playback controls sit to the
/// left of the position/tempo LCD and editing controls sit to its right, with
/// equal gaps and matching panel treatment on both sides.
class TransportBar : public QWidget {
    Q_OBJECT
public:
    explicit TransportBar(daw::EngineController* controller,
                          QWidget* parent = nullptr);
    ~TransportBar() override;

    /// Pull position/tempo/state from the controller (called by the UI timer).
    void refresh();
    /// Update only the moving time readout. The playhead clock calls this at
    /// display cadence; meters and transport state stay on the slower UI tick.
    void refreshPosition();
    /// Re-read tempo after a project load / undo.
    void syncTempo();

    bool snapEnabled() const { return m_snapEnabled; }
    /// Grid division in beats; 0 when the grid is off.
    double gridBeats() const;
    bool showsBars() const { return m_showBars; }

    /// Select the edit tool (0 Select, 1 Knife, 2 Eraser, 3 Region): updates
    /// the chip and emits toolChanged. Lets keyboard shortcuts drive the tool
    /// selector.
    void setToolIndex(int index);
    /// The tool the modifier key borrows, by the same indices.
    void setSecondaryToolIndex(int index);
    int secondaryToolIndex() const { return m_altToolIndex; }
    /// Flip the cycle / metronome buttons (for keyboard shortcuts) — the button
    /// toggle re-uses the existing signal chain.
    void toggleCycle();
    /// Put the Cycle lamp in step with the transport without emitting — the
    /// region can be armed by dragging one out on the ruler, and the button
    /// has to show that.
    void setCycleEnabled(bool on);
    void toggleMetronome();
    /// Light the Record button without recording: the transport is engaged and
    /// waiting for R. Recording itself keeps lighting it on its own, so this is
    /// only about the armed-but-idle state in between.
    void setRecordEngaged(bool engaged);
    bool isRecordEngaged() const { return m_recordEngaged; }

    /// Light the typing-keyboard button without emitting — the keyboard can
    /// also be switched from the menu or its shortcut.
    void setTypingKeyboardActive(bool active);
    /// Show which octave the typing keyboard starts on, in its tooltip.
    void setTypingKeyboardOctave(int octave);

    /// Keep the header's window toggles in step with panels opened by menus,
    /// shortcuts, or restored workspace state. These setters never echo the
    /// state back through the corresponding signals.
    void setMixerVisible(bool visible);
    void setInspectorVisible(bool visible);
    void setBrowserVisible(bool visible);
    void setWebVisible(bool visible);
    void setAiVisible(bool visible);
    void setMixerDetached(bool detached);

    /// Smallest header width that still keeps the position/BPM readout and the
    /// trailing edit group intact. Only secondary buttons in the transport
    /// group are allowed to disappear below its preferred size.
    int minimumResponsiveWidth() const;

signals:
    void playPauseRequested();
    void stopRequested();
    void recordRequested();
    void returnToStartRequested();
    void nudgeRequested(int bars);
    void loopToggled(bool on);
    void metronomeToggled(bool on);
    void typingKeyboardToggled(bool on);
    void tempoChanged(double bpm);
    void gridChanged();
    void snapChanged(bool on);
    void timeFormatChanged();
    void toolChanged(int tool); // 0 Select, 1 Knife, 2 Eraser, 3 Region, 4 Mute
    /// The tool held under the modifier key changed.
    void secondaryToolChanged(int tool);
    void zoomRequested(int direction);   // −1 out, +1 in, 0 fit
    void saveRequested();
    void openRequested();
    void importRequested();
    void exportRequested();
    void mixerToggled(bool on);
    void inspectorToggled(bool on);
    void browserToggled(bool on);
    void webToggled(bool on);
    void aiToggled(bool on);
    void detachMixerRequested();
    void addTrackRequested();
    void settingsRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    QWidget* buildRightGroup();
    QWidget* buildLeftDock();
    QWidget* buildRightDock();
    QWidget* buildPill();
    QWidget* buildLcd();
    /// Keep the complete three-part cluster centred. Secondary transport
    /// actions disappear by priority until only Stop, Play and Record remain;
    /// the position, BPM and edit controls never disappear.
    void updateResponsiveLayout();
    void applyTheme();
    void updatePositionStyle();
    /// Apply a valid BPM while the user is still typing. A run of live values
    /// is folded back to one undo step when editing finishes.
    void previewTempo(const QString& text);
    void commitTempo();
    QString positionText() const;

    daw::EngineController* m_controller = nullptr;

    QWidget* m_pill = nullptr;
    QWidget* m_leftDock = nullptr;
    QWidget* m_rightDock = nullptr;
    QWidget* m_rightGroup = nullptr;
    QWidget* m_transportGroup = nullptr;
    QWidget* m_lcdScreen = nullptr;
    ui::IconButton* m_toStartButton = nullptr;
    ui::IconButton* m_rewindButton = nullptr;
    ui::IconButton* m_stopButton = nullptr;
    ui::IconButton* m_forwardButton = nullptr;
    ui::IconButton* m_playButton = nullptr;
    ui::IconButton* m_recordButton = nullptr;
    ui::IconButton* m_loopButton = nullptr;
    ui::IconButton* m_metroButton = nullptr;
    ui::IconButton* m_snapButton = nullptr;
    ui::IconButton* m_typingKeysButton = nullptr;
    ui::IconButton* m_mixerPanelButton = nullptr;
    ui::IconButton* m_inspectorPanelButton = nullptr;
    ui::IconButton* m_browserPanelButton = nullptr;
    ui::IconButton* m_detachMixerButton = nullptr;
    ui::IconButton* m_webPanelButton = nullptr;
    ui::IconButton* m_aiPanelButton = nullptr;

    QLabel* m_positionValue = nullptr;
    QLineEdit* m_tempoEdit = nullptr;
    QLabel* m_statusDot = nullptr;
    QLabel* m_statusText = nullptr;
    QLabel* m_deviceText = nullptr;

    QToolButton* m_timeFormatButton = nullptr;
    QToolButton* m_gridButton = nullptr;
    QToolButton* m_toolButton = nullptr;
    QList<QAction*> m_toolActions;
    /// The tool the modifier borrows — Logic's command-click tool.
    QToolButton* m_altToolButton = nullptr;
    QList<QAction*> m_altToolActions;
    int m_altToolIndex = 1;   // Knife: the one a modifier is most used for

    /// Both of these are remembered across launches (see UiConstants.hpp);
    /// the initialisers are only the factory defaults.
    SpectrumMeter* m_spectrum = nullptr;

    /// Both of these are remembered across launches (see UiConstants.hpp);
    /// the initialisers are only the factory defaults.
    int m_gridIndex = 5;      // 1/16
    int m_toolIndex = 0;      // 0 Select, 1 Knife, 2 Eraser, 3 Region
    bool m_snapEnabled = true;
    bool m_showBars = true;
    bool m_recordEngaged = false;   // armed and waiting for R
    bool m_positionRecording = false;
    int m_typingOctave = 5;         // shown in the typing keyboard's tooltip
    bool m_tempoEditing = false;
    std::size_t m_tempoUndoDepth = 0;
};
