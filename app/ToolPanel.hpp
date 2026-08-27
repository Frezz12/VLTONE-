#pragma once

#include <QHBoxLayout>
#include <QWidget>

namespace icons { enum class Glyph; }
namespace ui { class IconButton; }

/// A toolbar strip under the transport bar, divided into zones that line up
/// with the columns below it (inspector | tracks | timeline). Each zone holds
/// small icon buttons: the inspector toggle and an add-track button in their
/// own columns, and the playback switches (Restart / From clip) over the
/// timeline — the big territory used for actual arrangement work.
///
/// The context panel floats in the middle of this strip, directly under the
/// transport's position/tempo readout, so it isn't part of the zone layout —
/// it is a child that centres itself and only needs to hear about resizes.
class ToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit ToolPanel(QWidget* parent = nullptr);

    void setRestartMode(bool on);
    void setPlayFromClip(bool on);
    /// Kept for the shell's benefit; the toggle itself is in the header drawer.
    void setInspectorVisible(bool visible);
    /// Sync the left zone width with the inspector column (collapsed/expanded).
    void setInspectorZoneWidth(int width);
    /// The same for the browser column, which can also be resized.
    void setBrowserZoneWidth(int width);
    /// Keep the playback/automation zone aligned with the resizable track
    /// header column below it.
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
    /// Mirror the effective creation gesture: latched toolbar mode or a held
    /// Alt/Option key. Signal blocking keeps a physical modifier from changing
    /// the latched choice.
    void setAutomationCreationActive(bool active);

signals:
    void resized();
    void restartModeToggled(bool on);
    void playFromClipToggled(bool on);
    /// Global reveal/collapse for automation lanes. Checked is the active
    /// state, so pressing the button a second time hides them again.
    void automationVisibilityToggled(bool visible);
    /// The user clicked the creation-mode button. Alt/Option is momentary and
    /// updates the same button visually without emitting this signal.
    void automationCreationModeToggled(bool enabled);
    void addTrackRequested();
    /// Right-click on the "+": the full list of track kinds, folders included.
    /// A plain click still makes the audio track that is wanted nine times in
    /// ten; this is where the tenth lives.
    void addTrackMenuRequested(const QPoint& globalPos);

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void applyTheme();
    /// Push the assistant's zone back to the end of the row. The zones are
    /// positional, and moving the browser to the right edge would otherwise
    /// leave it sitting outside the panel it labels.
    void moveAiZoneLast();
    ui::IconButton* m_restart = nullptr;
    ui::IconButton* m_playFromClip = nullptr;
    ui::IconButton* m_createAutomation = nullptr;
    ui::IconButton* m_showAutomation = nullptr;
    QWidget* m_inspectorZone = nullptr;
    QWidget* m_browserZone = nullptr;
    QWidget* m_browserSeparator = nullptr;
    QWidget* m_aiZone = nullptr;
    QWidget* m_aiSeparator = nullptr;
    QWidget* m_trackZone = nullptr;
    QHBoxLayout* m_row = nullptr;
};
