#pragma once

#include "Icons.hpp"
#include "model/Document.hpp"

#include <QAbstractButton>
#include <QColor>
#include <QRectF>
#include <QSizeF>
#include <QHash>
#include <QLineEdit>
#include <QWidget>

#include <functional>

class QLabel;
class QMenu;
class QPainter;
class QAction;
class QContextMenuEvent;
class QKeyEvent;
class QTimer;
class QVariantAnimation;

/// Custom-painted controls: flat, no bevels or fake 3-D, all tinted from the
/// active theme and repainted when it changes.
namespace daw { class EngineController; }

namespace ui {

/// Linear gain ↔ fader travel with a dB taper (−60 … +6 dB), so unity sits at
/// ~80 % of the throw and the top of the fader is usable for fine moves.
double gainFromFaderPosition(double position);
double faderPositionFromGain(double gain);
/// "−12.3 dB" / "−∞ dB"
QString formatGainDb(double gain);

/// Global, latched half of the "create automation" gesture. Alt/Option is
/// still read directly from each mouse event; the toolbar button sets this
/// half so the same double-click works without holding a modifier.
void setAutomationCreationMode(bool enabled);
bool automationCreationMode();

/// How every slider in the application is drawn, in one place.
///
/// One thick recessed track, the value filled from the start of the throw, and
/// a glass handle that rides **inside** the track instead of sitting on top of
/// it — the same pane-of-glass vocabulary as `ui::GlassPanel`, at the size of a
/// fader cap: a translucent body, a sheen over its top half and a rim graded
/// from lit to shadowed. No metal, no drop shadow, nothing embossed.
///
/// The QSS in `Theme.cpp` mirrors this for the plain `QSlider`s in settings and
/// generic plugin editors, so a slider looks the same wherever it comes from.
struct SliderPaint {
    Qt::Orientation orientation = Qt::Horizontal;
    /// 0…1 along the axis: left→right, and bottom→top for a vertical slider.
    double position = 0.0;
    /// Where the value fill starts: 0 for an ordinary level, 0.5 for a bipolar
    /// control, 1.0 for one that runs backwards (the fade-out's).
    double fillFrom = 0.0;
    /// A detent to mark inside the track (unity gain), or < 0 for none.
    double detent = -1.0;
    /// Being dragged, or under the pointer: the glass lights up.
    bool active = false;
    /// The handle fills the track instead of riding inside it — for a control
    /// that has to be as slim as its own handle.
    bool flush = false;
    /// Defaults to the theme accent when left invalid.
    QColor accent;
};

/// Paint one into `track` — the groove's full rect, thickness included.
void paintSlider(QPainter& painter, const QRectF& track, const SliderPaint& spec);

/// Diameter of the handle in a track of this thickness. The handle is a circle
/// that fits *inside* the groove, so there is only ever one number to derive.
double sliderHandleDiameter(double trackThickness);
/// Where the centre of the handle sits for `position`, along the axis.
double sliderHandleAxis(const QRectF& track, Qt::Orientation orientation,
                        double position, bool flush = false);
/// The inverse: the position 0…1 a point on the axis stands for.
double sliderPositionAt(const QRectF& track, Qt::Orientation orientation,
                        double coordinate, bool flush = false);

/// Thickness of a track: wide enough for a round handle to travel inside it.
inline constexpr double kSliderTrack = 18.0;

/// Base for widgets that need a repaint when the palette changes.
class ThemedWidget : public QWidget {
    Q_OBJECT
public:
    explicit ThemedWidget(QWidget* parent = nullptr);
};

/// A 0…1 value that eases towards its target instead of snapping, repainting
/// its owner as it goes. The shared machinery behind hover glow and press
/// depression — a control keeps one per state it wants to animate rather than
/// a bare bool.
class Fade {
public:
    explicit Fade(QWidget* owner, int durationMs = 120);

    void setTarget(double target);
    /// Arrive immediately, with no animation — for the reduced-motion path and
    /// for state restored at construction, which should never be seen moving.
    void jumpTo(double value);
    double value() const { return m_value; }

private:
    QWidget* m_owner = nullptr;
    QVariantAnimation* m_anim = nullptr;
    double m_value = 0.0;
};

/// The floating readout shown while a value is being dragged. A top-level
/// popup rather than something painted inside the control, so it can sit above
/// an 18-pixel-tall fader without the control having to reserve room for it.
/// One instance, reused.
class ValueBubble {
public:
    /// Show `text` centred just above `anchor` (in `owner`'s coordinates).
    static void showFor(QWidget* owner, const QPoint& anchor, const QString& text);
    static void dismiss();
};

/// Icon-only button. `prominent` gives it the filled accent treatment used for
/// the play button in the transport pill.
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(icons::Glyph glyph, const QString& tip,
                        QWidget* parent = nullptr);

    void setGlyph(icons::Glyph glyph);
    void setProminent(bool on);
    /// Tint the glyph with the accent colour without any filled circle — used
    /// for the play button so it stands out while staying flat.
    void setAccentTint(bool on) {
        if (m_accentTint == on) return;
        m_accentTint = on;
        update();
    }
    /// Colour used when the button is checked/active (defaults to the accent).
    void setActiveColor(const QColor& c) {
        if (m_activeColor == c) return;
        m_activeColor = c;
        update();
    }
    /// Breathe in the active colour. Used by the transport's Record button
    /// while record is engaged but not yet rolling: the button is lit and
    /// waiting, which a static shade cannot say apart from "recording".
    void setPulse(bool on);
    void setButtonSize(int w, int h);

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    icons::Glyph m_glyph;
    bool m_prominent = false;
    bool m_accentTint = false;
    QColor m_activeColor;
    bool m_pulse = false;
    double m_pulseValue = 0.0;
    QVariantAnimation* m_pulseAnim = nullptr;
    Fade m_hoverFade{this};
    Fade m_pressFade{this, 90};
};

/// The square M / S / R chips on track headers and channel strips. Off they are
/// a neutral well; on they light in their signal colour.
class MsrButton : public QAbstractButton {
    Q_OBJECT
public:
    MsrButton(const QString& letter, const QColor& activeColor,
              const QString& tip, QWidget* parent = nullptr);

    /// Change the letter drawn on the chip (a chip whose meaning cycles).
    void setLetter(const QString& letter);
    /// Show a small "A" in the corner: this chip's state is being managed for
    /// the user rather than set by them. Used by smart input monitoring.
    void setAutoMark(bool on);
    /// Colour the lit state, so one chip can mean different things.
    void setActiveColor(const QColor& color);
    /// Shrink (or grow) the chip. The track headers run four of these on one
    /// line beside a fader and a pan knob, and at the mixer's size they would
    /// take the width the fader needs.
    void setChipSize(int w, int h);
    /// Offer Create Automation Clip on right-click. Only meaningful for state
    /// buttons such as Mute; Solo, Record and Monitor deliberately leave it off.
    void setAutomatable(bool automatable) { m_automatable = automatable; }
    bool isAutomatable() const noexcept { return m_automatable; }

signals:
    void automateRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    QString m_letter;
    QColor m_active;
    bool m_autoMark = false;
    bool m_automatable = false;
    Fade m_hoverFade{this};
    Fade m_pressFade{this, 90};
};

/// The channel fader: a console fader, not a slider.
///
/// Deliberately its own instrument, drawn nothing like `paintSlider` above. It
/// is the one control on the strip that an engineer reads *without* touching —
/// which is what the whole shape is for: a thin recessed slot so the cap's
/// position is unambiguous, a wide milled cap that reads at a glance and gives
/// the finger something to sit on, and a printed dB scale beside it so a level
/// can be read off the panel rather than off a number. That is Logic's fader,
/// and every console it borrowed it from; what is ours is the palette, the
/// accent lit in the slot below the cap, and the glass on the cap's face.
///
/// Vertical in the mixer, horizontal in the track headers — same feel and the
/// same unity detent in both. The scale only appears where there is width for
/// it (`setScaleVisible`), so the header's short fader is just slot and cap.
class FaderWidget : public QWidget {
    Q_OBJECT
public:
    explicit FaderWidget(QWidget* parent = nullptr)
        : FaderWidget(Qt::Vertical, parent) {}
    FaderWidget(Qt::Orientation orientation, QWidget* parent = nullptr);

    double gain() const { return m_gain; }
    void setGain(double gain);
    /// Print the dB scale down the left of the slot. Vertical faders only —
    /// there is nowhere to put it on a horizontal one.
    void setScaleVisible(bool visible);
    /// Turn a horizontal header fader into a compact round level control while
    /// preserving the same gain, signals and automation gesture.
    void setCompactKnob(bool compact);
    bool isCompactKnob() const noexcept { return m_compactKnob; }

    /// Hand Alt/Option+double-click (or a latched automation-create mode) to
    /// automation. A plain double-click always resets to unity.
    ///
    /// Off by default, so controls that drive nothing automatable expose no
    /// creation gesture or context-menu command.
    void setAutomatable(bool automatable) { m_automatable = automatable; }
    bool isAutomatable() const noexcept { return m_automatable; }
    bool isEditing() const noexcept { return m_dragging; }

signals:
    void gainChanged(double gain);
    /// The automation creation gesture or context-menu command was used.
    void automateRequested();

    /// Emitted when a drag finishes — a good point to mark the project dirty.
    void editFinished();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    /// The column the slot and cap live in: the whole widget, less the scale.
    QRectF faderColumn() const;
    /// The recessed slot, centred in that column.
    QRectF trackRect() const;
    /// The cap's size along and across the axis.
    QSizeF capSize() const;
    /// The cap at the current value.
    QRectF capRect(double position) const;
    double travel() const;
    /// Position (0…1) → pixel along the fader's axis.
    double knobPos(double position) const;
    /// Float the dB readout above the knob while dragging.
    void showBubble();
    void paintScale(QPainter& p) const;
    void paintCap(QPainter& p, const QRectF& cap) const;

    Qt::Orientation m_orientation = Qt::Vertical;
    bool m_scale = false;
    double m_gain = 1.0;
    double m_dragStartPosition = 0.0;
    int m_dragStartCoord = 0;
    bool m_dragging = false;
    bool m_hovered = false;
    bool m_automatable = false;
    bool m_compactKnob = false;
    int m_regularMinimumWidth = -1;
    int m_regularMaximumWidth = QWIDGETSIZE_MAX;
};

/// An icon that grows a slider when you point at it.
///
/// The context-panel island has room for a 24-pixel icon and nothing else, but
/// gain, pan and the fade lengths are continuous values that want a throw. So
/// the icon is all that is on the island; hovering it slides a small horizontal
/// slider out below, which stays open while the pointer is on either, and the
/// value is readable and draggable there. The icon itself is also draggable, so
/// a quick adjustment never has to wait for the flyout.
class MiniSlider : public QWidget {
    Q_OBJECT
public:
    MiniSlider(icons::Glyph glyph, const QString& tip, QWidget* parent = nullptr);
    ~MiniSlider() override;

    void setRange(double minimum, double maximum);
    /// Units per pixel of drag on the icon. The flyout maps its full width to
    /// the whole range instead.
    void setStep(double perPixel);
    /// How the value reads in the flyout and the drag bubble.
    void setFormatter(std::function<QString(double)> formatter);
    /// Value a double-click resets to.
    void setDefaultValue(double value) { m_default = value; }
    /// Run the control right-to-left: dragging *left* raises the value and the
    /// fill grows from the right edge. Used for the fade-out, so the two fade
    /// controls mirror each other the way the ramps on the clip do.
    void setInverted(bool inverted);
    /// Open as a knob rather than a slider. Pan is a knob everywhere else in
    /// the application and reads wrong as a line; the island is the same either
    /// way, only the flyout changes.
    void setRotary(bool rotary);
    /// Value grows from the middle of the range, not from its start — pan, and
    /// anything else bipolar.
    void setBipolar(bool bipolar);

    double value() const { return m_value; }
    void setValue(double value);
    /// Open the flyout without a pointer. A grab cannot hover, and the flyout
    /// is where the value and the knob live — see `DAW_SHOT_FLYOUT`.
    void openFlyoutForTest();
    /// Give this parameter the same automation creation gestures and context
    /// command as the full-size fader and knobs.
    void setAutomatable(bool automatable) { m_automatable = automatable; }
    bool isAutomatable() const noexcept { return m_automatable; }

signals:
    /// The value is about to be changed by one physical gesture. Consumers
    /// that apply relative edits use this to snapshot their original values.
    void editStarted();
    void valueChanged(double value);
    void editFinished();
    void automateRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    friend class MiniSliderFlyout;
    QString text() const;
    void commit(double value);
    double fraction() const;
    /// True while the pointer is over the icon or the flyout.
    bool pointerNearby() const;

    icons::Glyph m_glyph;
    double m_value = 0.0;
    double m_min = 0.0;
    double m_max = 1.0;
    double m_step = 0.01;
    double m_default = 0.0;
    bool m_inverted = false;
    bool m_rotary = false;
    bool m_bipolar = false;
    bool m_automatable = false;
    std::function<QString(double)> m_formatter;
    double m_dragStartValue = 0.0;
    int m_dragStartX = 0;
    bool m_dragging = false;
    bool m_hover = false;
    Fade m_hoverFade{this};
};

/// Pan knob: a flat ring with an arc from centre to the current value.
class PanKnob : public QWidget {
    Q_OBJECT
public:
    explicit PanKnob(QWidget* parent = nullptr);

    double pan() const { return m_pan; }   // −1 … +1
    void setPan(double pan);

    /// Hand Alt/Option+double-click (or a latched automation-create mode) to
    /// automation. A plain double-click always resets to centre.
    ///
    /// Off by default, so controls that drive nothing automatable expose no
    /// creation gesture or context-menu command.
    void setAutomatable(bool automatable) { m_automatable = automatable; }
    bool isAutomatable() const noexcept { return m_automatable; }
    bool isEditing() const noexcept { return m_dragging; }

signals:
    void panChanged(double pan);
    void editFinished();
    /// The automation creation gesture or context-menu command was used.
    void automateRequested();


protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    /// Float the L/C/R readout above the knob while dragging.
    void showBubble();

    double m_pan = 0.0;
    double m_dragStart = 0.0;
    int m_dragStartY = 0;
    bool m_dragging = false;
    bool m_automatable = false;
};

/// A general-purpose rotary knob with a caption under it.
///
/// `PanKnob` above is deliberately not this: it is hard-wired to −1…+1 with a
/// centre detent and its own L/C/R readout, and lives in every channel strip.
/// This one carries an arbitrary range, a formatter and a label, which is what
/// a panel full of a hundred plugin parameters needs.
class Knob : public QWidget {
    Q_OBJECT
public:
    enum class VisualStyle { Standard, SamplerDigital, Gravity };

    Knob(const QString& caption, QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    void setDefaultValue(double value) { m_default = value; }
    /// Draw the value arc from the middle of the ring rather than from its
    /// start — right for anything bipolar (pan, tension, an envelope amount).
    void setBipolar(bool bipolar);
    /// Snap to whole numbers, for a stepped parameter.
    void setStepped(bool stepped) { m_stepped = stepped; }
    void setFormatter(std::function<QString(double)> formatter);
    void setCaption(const QString& caption);
    /// Compact mode: a smaller ring, for the dense INS matrix.
    void setCompact(bool compact);
    /// Just the ring, at an exact diameter and with no caption line under it —
    /// for a knob that has to live inside a slot row, where the row's own label
    /// is the caption and there is no vertical room for a second one. The
    /// drag readout still floats above it, so the value is never hidden.
    void setBare(int diameter);
    /// Graphite body, discrete scale and OLED hover readout used only by the
    /// built-in sampler. The rest of the DAW keeps the standard knob.
    void setVisualStyle(VisualStyle style);
    /// A pull toward values that mean something. Given the value the pointer is
    /// asking for, return the one the knob should take — usually the same, but
    /// a caller with a grid to hit can return the grid value while the pointer
    /// is near it, and the knob will sit there until the pointer leaves.
    ///
    /// The drag's own arithmetic is untouched: it runs from where the button
    /// went down, so leaving a detent costs exactly as much movement as
    /// entering it did, and a whole gesture never accumulates a drift.
    void setDetent(std::function<double(double)> detent);

    /// Hand Alt/Option+double-click (or a latched automation-create mode) to
    /// automation. A plain double-click always resets to the parameter default.
    ///
    /// Off by default, so controls that drive nothing automatable expose no
    /// creation gesture or context-menu command.
    void setAutomatable(bool automatable) { m_automatable = automatable; }
    bool isAutomatable() const noexcept { return m_automatable; }

    double value() const { return m_value; }
    void setValue(double value);
    /// True while a drag is in flight. A panel that polls its plugin for
    /// automation must not fight the knob the user is holding.
    bool isEditing() const { return m_dragging; }

signals:
    void valueChanged(double value);
    /// A drag, wheel or reset finished — where an undo entry belongs.
    void editFinished();
    /// The automation creation gesture or context-menu command was used.
    void automateRequested();


protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void commit(double value);
    double fraction() const;
    QString text() const;

    QString m_caption;
    double m_value = 0.0;
    double m_min = 0.0;
    double m_max = 1.0;
    double m_default = 0.0;
    bool m_bipolar = false;
    bool m_stepped = false;
    bool m_compact = false;
    std::function<double(double)> m_detent;
    VisualStyle m_visualStyle = VisualStyle::Standard;
    /// Non-zero once `setBare` has been called: the ring's exact diameter.
    int m_bare = 0;
    std::function<QString(double)> m_formatter;
    double m_dragStartValue = 0.0;
    int m_dragStartY = 0;
    bool m_dragging = false;
    bool m_automatable = false;
    Fade m_hoverFade{this};
};

/// The small round lamp that gates a section — FL's orange LED. Checkable, and
/// nothing more than a lamp plus an optional caption beside it.
class Led : public QAbstractButton {
    Q_OBJECT
public:
    explicit Led(const QString& caption = {}, QWidget* parent = nullptr);

    void setLedColor(const QColor& color) { m_color = color; update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    QSize sizeHint() const override;

private:
    QColor m_color;
};

/// A two-position slider that names both of its states — the shape a mode
/// picker wants, where a checkbox would only name one.
///
/// The knob slides between the halves rather than blinking, because the point
/// of the control is that the two modes are *alternatives*: one is always on,
/// and the movement is what says so. Clicking either half selects it; the
/// keyboard drives it with the arrows and Space.
class ModeSwitch : public QWidget {
    Q_OBJECT
public:
    ModeSwitch(const QString& leftLabel, const QString& rightLabel,
               QWidget* parent = nullptr);

    bool isRight() const { return m_right; }
    /// `animate` off puts the knob where it belongs with no travel — used when
    /// the remembered mode is restored at startup.
    void setRight(bool right, bool animate = true);

    /// Tint the knob. Defaults to the theme accent.
    void setActiveColor(const QColor& color);

signals:
    /// True when the right-hand state is now the active one.
    void toggled(bool right);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;
    QSize sizeHint() const override;

private:
    QString m_leftLabel;
    QString m_rightLabel;
    bool m_right = false;
    QColor m_active;
    Fade m_slide{this, 180};
    Fade m_hoverFade{this};
};

/// dB-scaled level meter with peak hold. One or two bars.
class LevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit LevelMeter(Qt::Orientation orientation = Qt::Vertical,
                        int channels = 2, QWidget* parent = nullptr);

    /// Feed a linear peak (0 … 1+). Decays and holds automatically.
    void setPeak(float peak);
    void setPeaks(float left, float right);
    void clearClip();

    /// How the meter is drawn.
    ///   Panel — a rounded, inset instrument with a clip lamp above it. What
    ///           the mixer's channel strips use.
    ///   Rail  — a single square strip filling its whole widget, meant to be
    ///           flush against an edge with no margin anywhere. The clip
    ///           indicator becomes the top of the strip itself, because a rail
    ///           has no room beside it for a separate lamp.
    enum class Style { Panel, Rail };
    void setMeterStyle(Style style);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    void drawBar(QPainter& p, const QRectF& r, float level, float hold) const;

    Qt::Orientation m_orientation;
    int m_channels;
    Style m_style = Style::Panel;
    float m_level[2] = {0.f, 0.f};
    float m_hold[2] = {0.f, 0.f};
    bool m_clipped = false;
};

/// A track-name field that stays inert until double-clicked. Clicking a track
/// header should select the track, not drop a text cursor into its name — a
/// focused line edit would swallow Space, which is play/pause.
class InlineNameEdit : public QLineEdit {
    Q_OBJECT
public:
    explicit InlineNameEdit(const QString& text, QWidget* parent = nullptr);

protected:
    bool event(QEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

private:
    void endEditing();
};

/// A thin grab strip between two panels: drag it to resize the one beside it.
///
/// The app has no splitters — panel sizes are the owner's business, and each
/// owner clamps the drag its own way (the mixer against the arrangement's
/// height, the browser against the window's width). So this reports the drag
/// and nothing else: `onDrag` receives the distance from where the gesture
/// started, in pixels along the handle's axis, and the owner decides what that
/// means.
class ResizeHandle : public QWidget {
    Q_OBJECT
public:
    /// `Qt::Horizontal` is a horizontal bar that resizes vertically (the
    /// mixer); `Qt::Vertical` is a vertical bar that resizes horizontally (a
    /// side panel).
    explicit ResizeHandle(Qt::Orientation orientation, QWidget* parent = nullptr);

    /// Distance from the start of the gesture, so the owner applies it to the
    /// size the panel had when the drag began — applying it to the running size
    /// compounds every move event and the panel jumps.
    std::function<void(int delta)> onDrag;
    std::function<void()> onDragStart;

protected:
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void paintEvent(QPaintEvent*) override;

private:
    double along(const QMouseEvent* ev) const;

    Qt::Orientation m_orientation;
    double m_start = 0.0;
    bool m_dragging = false;
};

/// Small uppercase caption used above console sections ("SENDS", "AUDIO FX").
QLabel* sectionLabel(const QString& text, QWidget* parent = nullptr);

/// A 1-px themed separator line.
QWidget* separatorLine(Qt::Orientation orientation, int length = 0,
                       QWidget* parent = nullptr);

/// Append one "Add … Track" item per track kind to `menu`, and return the map
/// from action back to kind.
///
/// One definition of the list, used by every context menu and the Track menu,
/// so they can't drift apart — and the caller matches on the action pointer
/// rather than on a position in a parallel array, which is what made adding a
/// conditional item to such a menu a hazard.
/// One entry of that menu: what it makes, and how to make it. Folders are why
/// the kind alone is no longer enough — two of the entries are folders, and
/// what separates them is not their kind but whether they sum.
struct NewTrackSpec {
    daw::TrackKind kind = daw::TrackKind::Audio;
    bool summing = false;

    /// Create it, and return the new track's id.
    std::string create(daw::EngineController& controller) const;
};

QHash<QAction*, NewTrackSpec> addTrackKindItems(QMenu& menu);

} // namespace ui
