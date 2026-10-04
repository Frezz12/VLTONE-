#pragma once

#include "Controls.hpp"

#include <QPixmap>

class QPaintEvent;

namespace ui {

/// Side of one COLOR pot on a channel `stripWidth` pixels wide.
///
/// The section puts two of them side by side inside the strip's own well, so
/// the size is bounded by the strip rather than chosen for looks. Three steps
/// is what a mixer actually needs: a 75 px channel has no room for a dial big
/// enough to read, and a 150 px one looks empty without a larger one.
int colorKnobSide(int stripWidth);

/// A machined console pot: a pocket milled into the faceplate, a knurled body
/// seated in it, and the value milled into the ring around it.
///
/// `Knob` supplies everything a DAW control is required to have — vertical
/// drag, Shift precision, wheel, keyboard, double-click reset, the accessible
/// slider role, and the one-entry-per-gesture signal COLOR's Undo comes from.
/// Only the face is new, and it is new here alone: the rest of the console
/// keeps the flat controls the rest of the application uses. That is also why
/// this lives in its own file rather than as another `Knob::VisualStyle` —
/// `Controls.hpp` asks for flat controls, and a hardware face for one block is
/// an exception worth naming.
///
/// The body is a fixed piece of metal, so it is painted once into a pixmap and
/// reused across every repaint. Only the value ring, the pointer and the focus
/// outline are redrawn while a gesture is running.
class ColorKnob final : public Knob {
    Q_OBJECT
public:
    explicit ColorKnob(const QString& caption, QWidget* parent = nullptr);

    /// Re-cut the body at a new diameter, following the strip's width. Keeps the
    /// value, so widening a channel never moves a control the user is holding.
    void setDiameter(int side);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void buildFace(qreal ratio);

    QPixmap m_face;
    qreal m_faceRatio = 0.0;
    int m_side = 0;
};

} // namespace ui

