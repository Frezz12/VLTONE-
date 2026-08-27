#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>

class QVariantAnimation;

namespace ui {

/// A floating plate of "liquid glass": the content behind it is captured,
/// blurred and slightly saturated, then covered with a milky base fill, an
/// accent wash, a film of noise and a specular highlight along its lit edges.
/// It reads as a thick pane hovering over the arrangement rather than another
/// flat rectangle.
///
/// Qt Widgets has no `backdrop-filter`, so the refraction is done by hand: the
/// host is rendered without this plate into a small pixmap, blurred, and drawn
/// back underneath. That is not free, so the result is cached and the
/// recapture rate is capped — see captureBackdrop().
///
/// Users who don't want any of it can set `contextPanel/reduceTransparency`,
/// which drops back to a plain elevated-surface fill.
class GlassPanel : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
public:
    explicit GlassPanel(QWidget* parent = nullptr);

    QColor accentColor() const { return m_accent; }
    void setAccentColor(const QColor& c);

    /// Corner radius of the plate, also used to clip its contents.
    void setCornerRadius(int radius);
    int cornerRadius() const { return m_radius; }

    /// Use a gentler top-to-bottom grade with a lighter lower rim. The compact
    /// transport LCD needs only a hint of glass depth; the broader floating
    /// context plates keep the stronger default treatment.
    void setSubtleVerticalGradient(bool subtle);
    bool hasSubtleVerticalGradient() const { return m_subtleVerticalGradient; }

    /// Hang the plate off the top edge of its host instead of floating free.
    /// The top corners then flare outwards into the bar above with a concave
    /// fillet, so the plate reads as part of that bar rather than as a
    /// rectangle parked against it — the notch, not a sticker.
    void setTopAttached(bool attached);
    bool isTopAttached() const { return m_topAttached; }

    /// Briefly wash the plate in its accent colour — the confirmation for a
    /// discrete action like Split or Duplicate.
    void flashConfirm();

    /// Drop the cached backdrop, so the next paint re-reads what is behind the
    /// plate. Call after anything that moves the plate or repaints beneath it.
    void invalidateBackdrop();

    /// While set, the cached backdrop is stretched rather than recaptured. Held
    /// across a transition: the plate is resizing every frame, the capture is
    /// the most expensive thing it does, and the motion hides the staleness.
    void setBackdropFrozen(bool frozen) { m_backdropFrozen = frozen; }

    /// True when the user asked for the flat, opaque treatment.
    static bool reduceTransparency();
    /// Persist the treatment, refresh the paint-time cache and invalidate every
    /// live glass surface. Preference UI must go through this setter rather
    /// than writing the QSettings key directly.
    static void setReduceTransparency(bool reduce);

    /// Padding the widget reserves around the plate for its drop shadow. A
    /// child widget can't paint outside its own rect, so the shadow has to live
    /// inside it — layouts must add this to their margins, and a plate sitting
    /// in a short strip has to trim it.
    int shadowMargin() const { return m_shadowMargin; }
    void setShadowMargin(int margin);
    /// The plate itself, inset from the widget by the shadow margin. When
    /// attached at the top there is no inset there — the plate starts flush
    /// with the host's edge.
    virtual QRect plateRect() const;
    /// The plate's outline, including the flare when attached at the top.
    virtual QPainterPath plateShape() const;

protected:
    /// Called on every live glass surface when the preference changes.
    /// Subclasses with their own glass-dependent animation can extend it.
    virtual void onReduceTransparencyChanged();
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void moveEvent(QMoveEvent*) override;

private:
    /// Capture the host beneath the panel into m_backdrop, downscaled and
    /// blurred. The panel is moved outside the host only for that synchronous
    /// off-screen render so it cannot capture and tint itself.
    void captureBackdrop();
    /// Box blur three times over ≈ a gaussian, at a fraction of the cost.
    static QImage blurAndTint(QImage image);
    static const QImage& noiseTexture();

    QColor m_accent;
    int m_radius = 14;
    int m_shadowMargin = 12;
    bool m_topAttached = false;
    bool m_subtleVerticalGradient = false;
    QPixmap m_backdrop;
    QElapsedTimer m_sinceCapture;
    bool m_capturePending = false;
    bool m_backdropFrozen = false;
    bool m_backdropValid = false;
    double m_flash = 0.0;
    QVariantAnimation* m_flashAnim = nullptr;
};

}  // namespace ui
