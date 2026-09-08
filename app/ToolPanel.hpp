#pragma once

#include <QHBoxLayout>
#include <QPointer>
#include <QVector>
#include <QWidget>

namespace icons { enum class Glyph; }
namespace ui { class IconButton; }
class QToolButton;

/// A toolbar strip under the transport bar, divided into zones that line up
/// with the columns below it (inspector | tracks | timeline). Arrangement
/// actions are built here because this object owns their state and signals,
/// then moved into the ruler above the track headers.
///
/// The context panel floats in the middle of this strip, directly under the
/// transport's position/tempo readout, so it isn't part of the zone layout —
/// it is a child that centres itself and only needs to hear about resizes.
class ToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolPanel(QWidget* parent = nullptr);

    /// Move the track actions into the ruler above the track headers. The
    /// strip keeps an empty track-width zone so its other columns stay aligned.
    QWidget* takeTrackActions();

    void setRestartMode(bool on);
    void setPlayFromClip(bool on);
    void setFollowPlayhead(bool on);
    void setZoomFocusEnabled(bool on);
    /// Kept for the shell's benefit; the toggle itself is in the header drawer.
    void setInspectorVisible(bool visible);
    /// Sync the left zone width with the inspector column (collapsed/expanded).
    void setInspectorZoneWidth(int width);
    /// The same for the browser column, which can also be resized.
    void setBrowserZoneWidth(int width);
    /// Keep the empty track alignment zone synced with the resizable header.
    void setTrackZoneWidth(int width);
    void setBrowserVisible(bool visible);
    /// Move the browser's zone to whichever end the panel is on, so the strip
    /// keeps reading as a set of labels over the columns beneath it.
    void setBrowserOnLeft(bool onLeft);
    /// The assistant's own button. Its panel never moves, so unlike the browser
    /// there is no side to follow — only a visibility to mirror.
    void setAiVisible(bool visible);
    /// Mirror whether any automation lanes are currently expanded. Signal
    /// blocking keeps document refreshes from turning into user commands.
    void setAutomationVisible(bool visible);
    /// Mirror the explicit mode choice without emitting another toggle.
    void setAutomationCreationActive(bool active);
    void setAutomationCreationShortcut(const QString& shortcut);
    /// Keep a permanent boundary before the waveform control, even while it
    /// is hidden. Its slot must not move when a context panel approaches it.
    int contextRightEdge() const;
    int contextLeftEdge() const;
    void watchContextPanel(QWidget* panel);

signals:
    void resized();
    void restartModeToggled(bool on);
    void playFromClipToggled(bool on);
    void followPlayheadToggled(bool on);
    void zoomFocusToggled(bool on);
    /// Display-only scale for audio waveforms in arrangement clips.
    void waveformScaleChanged(double scale);
    /// Global reveal/collapse for automation lanes. Checked is the active
    /// state, so pressing the button a second time hides them again.
    void automationVisibilityToggled(bool visible);
    /// The user clicked the creation-mode button.
    void automationCreationModeToggled(bool enabled);
    void createTracksRequested();

protected:
    bool event(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void updateWaveformVisibility(QWidget* changingPanel = nullptr, bool showing = false);
    QVector<QPointer<QWidget>> m_contextPanels;
    void applyTheme();
    /// Push the assistant's zone back to the end of the row. The zones are
    /// positional, and moving the browser to the right edge would otherwise
    /// leave it sitting outside the panel it labels.
    void moveAiZoneLast();
    ui::IconButton* m_restart = nullptr;
    ui::IconButton* m_playFromClip = nullptr;
    ui::IconButton* m_followPlayhead = nullptr;
    ui::IconButton* m_zoomFocus = nullptr;
    ui::IconButton* m_waveformScale = nullptr;
    ui::IconButton* m_createAutomation = nullptr;
    ui::IconButton* m_showAutomation = nullptr;
    QWidget* m_trackActions = nullptr;
    ui::IconButton* m_createTrack = nullptr;
    QWidget* m_inspectorZone = nullptr;
    QWidget* m_browserZone = nullptr;
    QWidget* m_browserSeparator = nullptr;
    QWidget* m_aiZone = nullptr;
    QWidget* m_aiSeparator = nullptr;
    QWidget* m_trackZone = nullptr;
    QHBoxLayout* m_row = nullptr;
};
