#pragma once

#include "MidiTools.hpp"

#include <QDialog>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGridLayout;
class QTableWidget;
namespace ui { class Knob; }

// Parameter dialogs for the piano roll's Tools menu.
//
// Each one is a pure editor for a `daw::miditools` params struct: it owns no
// notes, touches no document and performs no edit. It announces
// `paramsChanged()` whenever a control moves and `applyRequested()` when the
// user commits; the piano roll window listens, runs the matching transform on
// the selection, and either paints the result as a preview or writes it through
// `EngineController::setClipNotes`.
//
// Keeping them this dumb is what makes the preview honest: the view owns the
// generated notes and Apply commits that exact preview instead of running a
// random-capable transform for a second time.

/// Shared plumbing: a "Preview" checkbox, an Apply button that stays available
/// (so a tool can be applied repeatedly) and a Close button.
class ToolDialog : public QDialog {
    Q_OBJECT
public:
    explicit ToolDialog(const QString& title, QWidget* parent = nullptr);

    /// True while the user wants the result drawn live in the grid.
    bool previewEnabled() const;

signals:
    /// A control moved — recompute the preview.
    void paramsChanged();
    /// Commit the current parameters as one undoable edit.
    void applyRequested();

protected:
    /// Where subclasses put their controls: a two-column label/control grid.
    QGridLayout* form();
    /// Call once the form is filled, to add the preview/apply/close row.
    void finishLayout();
    /// Wire any widget's "value changed" signal to `paramsChanged`.
    void watch(QWidget* widget);

    void closeEvent(QCloseEvent* event) override;

private:
    QGridLayout* m_form = nullptr;
    QCheckBox* m_preview = nullptr;
    QDialogButtonBox* m_buttons = nullptr;
};

// ── Quantize ────────────────────────────────────────────────────────────────

class QuantizeDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit QuantizeDialog(QWidget* parent = nullptr);

    daw::miditools::QuantizeParams params() const;
    /// Seed the grid from whatever the roll's snap is set to, so opening the
    /// dialog and hitting Apply does the obvious thing.
    void setGridBeats(double beats);

private:
    QComboBox* m_grid = nullptr;
    QComboBox* m_flavour = nullptr;
    QComboBox* m_target = nullptr;
    ui::Knob* m_strength = nullptr;
    ui::Knob* m_swing = nullptr;
    QComboBox* m_swingUnit = nullptr;
    ui::Knob* m_tolerance = nullptr;
    QCheckBox* m_preserveOrder = nullptr;
    ui::Knob* m_randomize = nullptr;
    QComboBox* m_groove = nullptr;
    ui::Knob* m_grooveTiming = nullptr;
    ui::Knob* m_grooveVelocity = nullptr;
};

// ── Arpeggiator ─────────────────────────────────────────────────────────────

class ArpeggiatorDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit ArpeggiatorDialog(QWidget* parent = nullptr);

    daw::miditools::ArpParams params() const;
    void setRateBeats(double beats);

private:
    void rebuildSteps(int count);

    QComboBox* m_direction = nullptr;
    ui::Knob* m_octaves = nullptr;
    QComboBox* m_rate = nullptr;
    QComboBox* m_flavour = nullptr;
    ui::Knob* m_gate = nullptr;
    ui::Knob* m_stepCount = nullptr;
    /// One row per pattern step: velocity, skip, tie, transpose.
    QTableWidget* m_steps = nullptr;
    ui::Knob* m_ramp = nullptr;
    ui::Knob* m_swing = nullptr;
    ui::Knob* m_humanizeVelocity = nullptr;
    ui::Knob* m_humanizeTiming = nullptr;
    QComboBox* m_playMode = nullptr;
    QCheckBox* m_merge = nullptr;
};

// ── Glue ────────────────────────────────────────────────────────────────────

class GlueDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit GlueDialog(QWidget* parent = nullptr);

    daw::miditools::GlueParams params() const;

private:
    QComboBox* m_mode = nullptr;
    QCheckBox* m_samePitch = nullptr;
    ui::Knob* m_gap = nullptr;
    ui::Knob* m_legatoMax = nullptr;
};

// ── Articulate ──────────────────────────────────────────────────────────────

class ArticulateDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit ArticulateDialog(QWidget* parent = nullptr);

    daw::miditools::ArticulateParams params() const;

private:
    QComboBox* m_mode = nullptr;
    ui::Knob* m_gate = nullptr;
    ui::Knob* m_amount = nullptr;
    ui::Knob* m_minLength = nullptr;
    ui::Knob* m_maxLength = nullptr;
    QCheckBox* m_accentOn = nullptr;
    ui::Knob* m_accentEvery = nullptr;
    ui::Knob* m_accentVelocity = nullptr;
    ui::Knob* m_otherVelocity = nullptr;
};

// ── Strum ───────────────────────────────────────────────────────────────────

class StrumDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit StrumDialog(QWidget* parent = nullptr);

    daw::miditools::StrumParams params() const;

private:
    QComboBox* m_direction = nullptr;
    ui::Knob* m_span = nullptr;
    QComboBox* m_shape = nullptr;
    ui::Knob* m_taper = nullptr;
    ui::Knob* m_window = nullptr;
    QCheckBox* m_adjustEnds = nullptr;
};

// ── Randomize ───────────────────────────────────────────────────────────────

class RandomizeDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit RandomizeDialog(QWidget* parent = nullptr);

    daw::miditools::RandomParams params() const;
    /// The clip's length, so "keep everything inside the clip" has a bound.
    void setRegionEndBeats(double beats);
    void setGridBeats(double beats);

private:
    QCheckBox* m_velocityOn = nullptr;
    ui::Knob* m_velocity = nullptr;
    QCheckBox* m_pitchOn = nullptr;
    ui::Knob* m_pitch = nullptr;
    QCheckBox* m_scaleAware = nullptr;
    QComboBox* m_scaleRoot = nullptr;
    QComboBox* m_scale = nullptr;
    QCheckBox* m_timingOn = nullptr;
    ui::Knob* m_timing = nullptr;
    QCheckBox* m_constrain = nullptr;
    QCheckBox* m_durationOn = nullptr;
    ui::Knob* m_duration = nullptr;
    QCheckBox* m_gaussian = nullptr;
    QCheckBox* m_preserveTotal = nullptr;
    ui::Knob* m_seed = nullptr;
    double m_regionEndBeats = 0.0;
    double m_gridBeats = 0.25;
};

// ── Chord generator ─────────────────────────────────────────────────────────

class ChordDialog : public ToolDialog {
    Q_OBJECT
public:
    explicit ChordDialog(QWidget* parent = nullptr);

    daw::miditools::ChordParams params() const;

private:
    QComboBox* m_type = nullptr;
    ui::Knob* m_inversion = nullptr;
    QCheckBox* m_addOctave = nullptr;
    QCheckBox* m_bassOctave = nullptr;
};
