#include "SpectrumMeter.hpp"

#include "Theme.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

constexpr float kSilenceFloor = 0.0008f;
constexpr float kAttack = 0.72f;
constexpr float kRelease = 0.24f;
constexpr float kPeakRelease = 0.91f;

QPainterPath topRoundedBar(const QRectF& rect) {
    const double radius = std::min(rect.width() * 0.5, rect.height());
    QPainterPath path;
    path.moveTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), rect.top() + radius);
    path.quadTo(rect.left(), rect.top(), rect.left() + radius, rect.top());
    path.lineTo(rect.right() - radius, rect.top());
    path.quadTo(rect.right(), rect.top(), rect.right(), rect.top() + radius);
    path.lineTo(rect.right(), rect.bottom());
    path.closeSubpath();
    return path;
}

} // namespace

SpectrumMeter::SpectrumMeter(QWidget* parent) : QWidget(parent), m_accent(th().accent) {
    setFixedSize(88, 26);
    setToolTip(tr("Master spectrum — low frequencies on the left, high "
                  "frequencies on the right"));
    setAccessibleName(tr("Master spectrum analyzer"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        m_accent = th().accent;
        update();
    });
}

void SpectrumMeter::setAccent(const QColor& accent) {
    if (m_accent == accent) return;
    m_accent = accent;
    update();
}

void SpectrumMeter::push(const Levels& levels, bool live) {
    bool awake = false;
    for (std::size_t band = 0; band < kBandCount; ++band) {
        const float target = live ? std::clamp(levels[band], 0.0f, 2.0f) : 0.0f;
        const float amount = target > m_levels[band] ? kAttack : kRelease;
        m_levels[band] += (target - m_levels[band]) * amount;
        if (m_levels[band] < kSilenceFloor) m_levels[band] = 0.0f;

        if (m_levels[band] >= m_peakHold[band]) {
            m_peakHold[band] = m_levels[band];
        } else {
            m_peakHold[band] *= kPeakRelease;
            if (m_peakHold[band] < kSilenceFloor) m_peakHold[band] = 0.0f;
        }
        awake = awake || m_levels[band] > 0.0f || m_peakHold[band] > 0.0f;
    }

    // Repaint the settling frame as well; once every bar is down the dormant
    // analyser is static and costs nothing until audio returns.
    if (awake || m_awake) update();
    m_awake = awake;
}

double SpectrumMeter::displayHeight(float linear) {
    // Sixty decibels fit the available 18 pixels. A logarithmic scale keeps a
    // quiet high-frequency detail visible without pinning loud bass at the top.
    if (linear <= 0.000001f) return 0.0;
    const double db = 20.0 * std::log10(double(linear));
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
}

void SpectrumMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const Theme& theme = th();
    const QRectF box(0.5, 0.5, width() - 1.0, height() - 1.0);
    constexpr double corner = 6.0;

    QPainterPath well;
    well.addRoundedRect(box, corner, corner);
    QLinearGradient bed(box.topLeft(), box.bottomLeft());
    bed.setColorAt(0.0, QColor(0, 0, 0, theme.dark ? 78 : 28));
    bed.setColorAt(1.0, QColor(0, 0, 0, theme.dark ? 38 : 13));
    painter.fillPath(well, bed);

    painter.save();
    painter.setClipPath(well);

    const QRectF plot = box.adjusted(5.0, 3.5, -5.0, -3.0);
    constexpr double gap = 2.0;
    const double barWidth =
        (plot.width() - gap * double(kBandCount - 1)) / double(kBandCount);
    const double baseline = plot.bottom();

    QColor track = theme.textSecondary;
    track.setAlpha(theme.dark ? 16 : 22);
    QColor dormant = theme.textSecondary;
    dormant.setAlpha(theme.dark ? 54 : 62);
    QColor cap = mixColors(m_accent, QColor(255, 255, 255),
                           theme.dark ? 0.38 : 0.18);
    cap.setAlpha(theme.dark ? 150 : 132);

    for (std::size_t band = 0; band < kBandCount; ++band) {
        const double x = plot.left() + double(band) * (barWidth + gap);

        // Faint full-height slots keep the analyser structure visible even
        // before playback starts; they are tracks, not invented level data.
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(QRectF(x, plot.top(), barWidth, plot.height()),
                                barWidth * 0.5, barWidth * 0.5);

        // The low dormant stub makes the frequency slots legible at silence,
        // like an armed hardware analyser rather than an empty black panel.
        painter.setBrush(dormant);
        painter.drawRoundedRect(QRectF(x, baseline - 2.0, barWidth, 2.0),
                                barWidth * 0.5, barWidth * 0.5);

        const double height = displayHeight(m_levels[band]) * plot.height();
        if (height > 0.0) {
            const QRectF bar(x, baseline - std::max(height, 1.5), barWidth,
                             std::max(height, 1.5));
            QLinearGradient fill(0.0, bar.top(), 0.0, bar.bottom());
            QColor bright = mixColors(m_accent, QColor(255, 255, 255),
                                      theme.dark ? 0.34 : 0.16);
            bright.setAlpha(theme.dark ? 238 : 220);
            QColor base = m_accent;
            base.setAlpha(theme.dark ? 126 : 144);
            fill.setColorAt(0.0, bright);
            fill.setColorAt(1.0, base);
            painter.setBrush(fill);
            painter.drawPath(topRoundedBar(bar));
        }

        const double peak = displayHeight(m_peakHold[band]);
        if (peak > 0.0) {
            const double y = baseline - peak * plot.height();
            painter.setPen(QPen(cap, 1.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(x + 0.6, y),
                             QPointF(x + barWidth - 0.6, y));
        }
    }

    // A restrained glass highlight ties the analyser to the surrounding LCD
    // without washing out the frequency bars.
    QLinearGradient sheen(box.topLeft(), QPointF(box.left(), box.center().y()));
    sheen.setColorAt(0.0, QColor(255, 255, 255, theme.dark ? 26 : 48));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(sheen);
    painter.drawRoundedRect(box.adjusted(1.0, 1.0, -1.0, -box.height() * 0.53),
                            corner, corner);
    painter.restore();

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, theme.dark ? 27 : 48), 1.0));
    painter.drawPath(well);
}
