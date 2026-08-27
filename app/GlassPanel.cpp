#include "GlassPanel.hpp"

#include "Theme.hpp"

#include <QApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>

namespace ui {
namespace {

// The backdrop is captured at a third of the panel's size: the parent render
// and the blur both get ~9× cheaper, and the upscale back to full size does a
// good part of the blurring for free.
constexpr double kBackdropScale = 1.0 / 3.0;
// Blur radius in *downscaled* pixels — roughly 3× this at panel scale.
constexpr int kBlurRadius = 8;
// Never recapture more often than this. The arrangement repaints every 33 ms
// during playback; recapturing at that rate would put the whole timeline paint
// into every frame three times over.
constexpr qint64 kRecaptureMs = 80;

constexpr int kNoiseSize = 64;

bool& reduceTransparencyCache() {
    // Paints only read this in-memory value. The settings backend is consulted
    // once, then the preferences page keeps it current through the setter.
    static bool reduce =
        QSettings().value("contextPanel/reduceTransparency", false).toBool();
    return reduce;
}

/// One horizontal box-blur pass over an ARGB32 image.
void boxBlurPass(QImage& src, QImage& dst, int radius) {
    const int w = src.width();
    const int h = src.height();
    const int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        auto* out = reinterpret_cast<QRgb*>(dst.scanLine(y));
        int a = 0, r = 0, g = 0, b = 0;
        // Prime the running sum with the window centred on x = 0, clamping at
        // the edges so the border doesn't darken.
        for (int i = -radius; i <= radius; ++i) {
            const QRgb p = in[std::clamp(i, 0, w - 1)];
            a += qAlpha(p); r += qRed(p); g += qGreen(p); b += qBlue(p);
        }
        for (int x = 0; x < w; ++x) {
            out[x] = qRgba(r / span, g / span, b / span, a / span);
            const QRgb drop = in[std::clamp(x - radius, 0, w - 1)];
            const QRgb add = in[std::clamp(x + radius + 1, 0, w - 1)];
            a += qAlpha(add) - qAlpha(drop);
            r += qRed(add) - qRed(drop);
            g += qGreen(add) - qGreen(drop);
            b += qBlue(add) - qBlue(drop);
        }
    }
}

}  // namespace

GlassPanel::GlassPanel(QWidget* parent) : QWidget(parent) {
    m_accent = th().accent;
    m_sinceCapture.start();
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        invalidateBackdrop();
        update();
    });
}

bool GlassPanel::reduceTransparency() {
    return reduceTransparencyCache();
}

void GlassPanel::setReduceTransparency(bool reduce) {
    bool& cached = reduceTransparencyCache();
    if (cached == reduce) return;

    cached = reduce;
    QSettings().setValue("contextPanel/reduceTransparency", reduce);

    // Preference changes are rare; walking the live glass surfaces here keeps
    // paintEvent free of settings I/O and gives subclasses a single, immediate
    // invalidation point without making every frame poll another flag.
    if (!QApplication::instance()) return;
    for (QWidget* widget : QApplication::allWidgets()) {
        if (auto* panel = qobject_cast<GlassPanel*>(widget))
            panel->onReduceTransparencyChanged();
    }
}

void GlassPanel::onReduceTransparencyChanged() {
    invalidateBackdrop();
    update();
}

void GlassPanel::setAccentColor(const QColor& c) {
    if (m_accent == c) return;
    m_accent = c;
    update();
}

void GlassPanel::setCornerRadius(int radius) {
    if (m_radius == radius) return;
    m_radius = radius;
    update();
}

void GlassPanel::setSubtleVerticalGradient(bool subtle) {
    if (m_subtleVerticalGradient == subtle) return;
    m_subtleVerticalGradient = subtle;
    update();
}

void GlassPanel::invalidateBackdrop() {
    m_backdropValid = false;
}

void GlassPanel::resizeEvent(QResizeEvent* ev) {
    invalidateBackdrop();
    QWidget::resizeEvent(ev);
}

void GlassPanel::moveEvent(QMoveEvent* ev) {
    invalidateBackdrop();
    QWidget::moveEvent(ev);
}

void GlassPanel::flashConfirm() {
    if (!m_flashAnim) {
        m_flashAnim = new QVariantAnimation(this);
        m_flashAnim->setDuration(420);
        // Up fast, down slow: a blink rather than a pulse.
        m_flashAnim->setKeyValueAt(0.0, 0.0);
        m_flashAnim->setKeyValueAt(0.18, 1.0);
        m_flashAnim->setKeyValueAt(1.0, 0.0);
        connect(m_flashAnim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) {
                    m_flash = v.toDouble();
                    update();
                });
    }
    m_flashAnim->stop();
    m_flashAnim->start();
}

const QImage& GlassPanel::noiseTexture() {
    // Built once and tiled. Real glass is never perfectly smooth, and without
    // this the gradients read as sterile banding.
    static QImage noise = [] {
        QImage img(kNoiseSize, kNoiseSize, QImage::Format_ARGB32_Premultiplied);
        auto* rng = QRandomGenerator::global();
        for (int y = 0; y < kNoiseSize; ++y) {
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < kNoiseSize; ++x) {
                const int v = int(rng->bounded(256));
                line[x] = qRgba(v, v, v, 255);
            }
        }
        return img;
    }();
    return noise;
}

QImage GlassPanel::blurAndTint(QImage image) {
    if (image.isNull()) return image;
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage scratch(image.size(), image.format());

    // Three box passes ≈ a gaussian. Each pass blurs horizontally, so the image
    // is transposed between them to cover the vertical axis too.
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(image, scratch, kBlurRadius);
        image = scratch.transformed(QTransform().rotate(90));
        scratch = QImage(image.size(), image.format());
        boxBlurPass(image, scratch, kBlurRadius);
        image = scratch.transformed(QTransform().rotate(-90));
        scratch = QImage(image.size(), image.format());
    }

    // Thick glass doesn't just soften what is behind it, it lifts it a little:
    // saturation 110 %, lightness 105 %.
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            QColor c = QColor::fromRgba(line[x]).toHsl();
            c.setHsl(c.hue(), std::min(255, int(c.saturation() * 1.10)),
                     std::min(255, int(c.lightness() * 1.05)), c.alpha());
            line[x] = c.rgba();
        }
    }
    return image;
}

void GlassPanel::captureBackdrop() {
    QWidget* host = parentWidget();
    const QRect plate = plateRect();
    if (!host || plate.width() <= 0 || plate.height() <= 0) return;

    const QSize small(std::max(1, int(plate.width() * kBackdropScale)),
                      std::max(1, int(plate.height() * kBackdropScale)));
    // Render the host while this plate is temporarily outside its bounds. A
    // screen grab sees the already-composited plate, so every recapture feeds
    // the previous accent wash back into the next one and the colour grows
    // brighter on every context switch. Moving away only for this synchronous
    // off-screen render keeps the widget visible and layout-managed; no event
    // loop runs before its original position is restored, so there is no
    // on-screen jump.
    const QPoint savedPosition = pos();
    const QRect source(savedPosition + plate.topLeft(), plate.size());
    const QRect clipped = source.intersected(host->rect());
    QPixmap fullResolution(plate.size());
    fullResolution.fill(Qt::transparent);
    if (!clipped.isEmpty()) {
        move(-width() - host->width() - 32,
             -height() - host->height() - 32);
        host->render(&fullResolution, -source.topLeft(), QRegion(clipped),
                     QWidget::DrawWindowBackground | QWidget::DrawChildren);
        move(savedPosition);
    }

    QPixmap raw = fullResolution.scaled(
        small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (raw.isNull()) {
        m_backdrop = {};
        m_backdropValid = true;
        m_sinceCapture.restart();
        return;
    }

    m_backdrop = QPixmap::fromImage(blurAndTint(raw.toImage()));
    m_backdropValid = true;
    m_sinceCapture.restart();
}

void GlassPanel::setShadowMargin(int margin) {
    if (m_shadowMargin == margin) return;
    m_shadowMargin = std::max(0, margin);
    invalidateBackdrop();
    update();
}

void GlassPanel::setTopAttached(bool attached) {
    if (m_topAttached == attached) return;
    m_topAttached = attached;
    invalidateBackdrop();
    update();
}

QRect GlassPanel::plateRect() const {
    // Attached at the top means flush with the host's edge: no inset there, and
    // the side inset leaves room for the flare rather than for a shadow.
    return rect().adjusted(m_shadowMargin, m_topAttached ? 0 : m_shadowMargin,
                           -m_shadowMargin, -m_shadowMargin);
}

QPainterPath GlassPanel::plateShape() const {
    const QRectF plate = QRectF(plateRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = std::min<qreal>(m_radius, plate.height() / 2.0);

    if (!m_topAttached) {
        QPainterPath rounded;
        rounded.addRoundedRect(plate, radius, radius);
        return rounded;
    }

    // The flare: a concave quarter-turn at each top corner that runs outwards
    // into the bar above, so there is no seam where the two meet. Its radius is
    // capped by the room the shadow margin left on either side.
    const qreal flare = std::min<qreal>(m_shadowMargin, plate.height() / 3.0);
    const qreal top = plate.top() - 0.5;   // hard against the host's edge

    QPainterPath path;
    path.moveTo(plate.left() - flare, top);
    path.quadTo(plate.left(), top, plate.left(), top + flare);
    path.lineTo(plate.left(), plate.bottom() - radius);
    path.quadTo(plate.left(), plate.bottom(), plate.left() + radius, plate.bottom());
    path.lineTo(plate.right() - radius, plate.bottom());
    path.quadTo(plate.right(), plate.bottom(), plate.right(), plate.bottom() - radius);
    path.lineTo(plate.right(), top + flare);
    path.quadTo(plate.right(), top, plate.right() + flare, top);
    path.closeSubpath();
    return path;
}

void GlassPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF plate = QRectF(plateRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath shape = plateShape();
    const Theme& theme = th();
    const bool flat = reduceTransparency();
    const qreal radius = std::min<qreal>(m_radius, plate.height() / 2.0);

    // ── Shadow ──
    // A stack of ever-larger, ever-fainter copies of the shape: cheaper than a
    // real gaussian and, at this size, indistinguishable from one. Attached at
    // the top there is nothing above to cast onto, so it only spreads downwards.
    for (int i = m_shadowMargin; i >= 1; --i) {
        const qreal up = m_topAttached ? 0.0 : i * 0.6;
        QPainterPath halo;
        halo.addRoundedRect(plate.adjusted(-i, -up, i, i * 1.2), radius + i,
                            radius + i);
        p.fillPath(halo, QColor(0, 0, 0, theme.dark ? 9 : 7));
    }

    p.save();
    p.setClipPath(shape);

    if (flat) {
        p.fillPath(shape, theme.surfaceElevated);
    } else {
        // ── 1. The refracted backdrop ──
        // Never captured from inside a paint: a stale backdrop schedules its
        // own host-only render and this pass draws whatever was cached.
        if (!m_backdropValid && !m_capturePending && !m_backdropFrozen &&
            m_sinceCapture.elapsed() >= kRecaptureMs) {
            m_capturePending = true;
            QTimer::singleShot(0, this, [this] {
                m_capturePending = false;
                captureBackdrop();
                update();
            });
        }
        if (!m_backdrop.isNull()) {
            p.drawPixmap(plateRect(), m_backdrop);
        } else {
            p.fillPath(shape, theme.background);
        }

        // ── 2. The body ──
        // Milky enough that text stays readable over a busy arrangement, thin
        // enough that the blur still shows through. Slightly denser at the
        // bottom, where a real pane would be thicker in the light.
        QLinearGradient body(plate.topLeft(), plate.bottomLeft());
        const QColor tint =
            theme.dark ? theme.surfaceElevated : QColor(255, 255, 255);
        QColor bodyTop = tint;
        bodyTop.setAlphaF(theme.dark ? 0.13 : 0.26);
        QColor bodyBottom = tint;
        bodyBottom.setAlphaF(
            m_subtleVerticalGradient ? (theme.dark ? 0.15 : 0.29)
                                     : (theme.dark ? 0.20 : 0.34));
        body.setColorAt(0.0, bodyTop);
        body.setColorAt(1.0, bodyBottom);
        p.fillPath(shape, body);
    }

    // ── 3. Accent wash ──
    // The colour anchor for the current kind of object. Kept faint: it should
    // read as a tint in the glass, not as a coloured button.
    QLinearGradient wash(plate.topLeft(), plate.bottomLeft());
    QColor washTop = m_accent;
    washTop.setAlphaF(flat ? 0.10 : 0.13);
    QColor washBottom = m_accent;
    washBottom.setAlphaF(0.03);
    wash.setColorAt(0.0, washTop);
    wash.setColorAt(1.0, washBottom);
    p.fillPath(shape, wash);

    // ── 4. Sheen ──
    // The one specular this needs: a soft wash across the upper third that
    // fades out, rather than a drawn line. A stroked highlight on a shape this
    // small reads as a stray scratch, which is exactly what it looked like.
    if (!flat) {
        QLinearGradient sheen(plate.topLeft(), QPointF(plate.left(),
                                                       plate.top() + plate.height() * 0.62));
        QColor lit(255, 255, 255);
        lit.setAlphaF(m_subtleVerticalGradient
                          ? (theme.dark ? 0.07 : 0.38)
                          : (theme.dark ? 0.10 : 0.55));
        sheen.setColorAt(0.0, lit);
        lit.setAlphaF(0.0);
        sheen.setColorAt(1.0, lit);
        p.fillPath(shape, sheen);
    }

    // ── 5. Noise, to take the sterility off the gradients ──
    if (!flat) {
        p.setOpacity(0.011);
        p.drawTiledPixmap(plateRect(), QPixmap::fromImage(noiseTexture()));
        p.setOpacity(1.0);
    }

    // ── 6. Confirmation flash ──
    if (m_flash > 0.001) {
        QColor flash = m_accent;
        flash.setAlphaF(0.26 * m_flash);
        p.fillPath(shape, flash);
    }

    p.restore();

    // ── 7. The rim ──
    // One stroke around the whole outline, graded from a bright edge at the top
    // to a dark one at the bottom. That grade is what gives the plate its
    // thickness; drawing two separate arcs for it, as this used to, left a
    // visible seam where they met.
    QLinearGradient edge(plate.topLeft(), plate.bottomLeft());
    QColor edgeTop = mixColors(QColor(255, 255, 255), m_accent, 0.35);
    edgeTop.setAlphaF(theme.dark ? 0.42 : 0.75);
    QColor edgeMid = m_accent;
    edgeMid.setAlphaF(0.22);
    QColor edgeBottom(0, 0, 0);
    edgeBottom.setAlphaF(
        m_subtleVerticalGradient ? (theme.dark ? 0.16 : 0.11)
                                 : (theme.dark ? 0.38 : 0.20));
    edge.setColorAt(0.0, edgeTop);
    edge.setColorAt(0.45, edgeMid);
    edge.setColorAt(1.0, edgeBottom);

    QPen rim(QBrush(edge), 1.0);
    rim.setJoinStyle(Qt::RoundJoin);
    p.setPen(rim);
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
}

}  // namespace ui
