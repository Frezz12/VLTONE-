#include "WaveformStrip.hpp"

#include "Theme.hpp"
#include "WaveformPaint.hpp"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {
/// Enough height for the envelope to say something about the sound, short
/// enough that the tree above it keeps the panel.
constexpr int kHeight = 56;
} // namespace

WaveformStrip::WaveformStrip(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setCursor(Qt::PointingHandCursor);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void WaveformStrip::setPeaks(const daw::WaveformPeaks& peaks) {
    m_peaks = peaks;
    m_message.clear();
    m_playheadSeconds = -1.0;
    update();
}

void WaveformStrip::clear(const QString& message) {
    m_peaks = daw::WaveformPeaks{};
    m_message = message;
    m_playheadSeconds = -1.0;
    update();
}

void WaveformStrip::setPlayheadSeconds(double seconds) {
    const auto cursorRect = [this](double at) {
        if (at < 0.0 || width() <= 0 || m_peaks.durationSeconds <= 0.0)
            return QRect{};
        const double x = std::clamp(at / m_peaks.durationSeconds, 0.0, 1.0) *
                         double(width());
        return QRect(int(std::floor(x)) - 3, 0, 7, height())
            .intersected(rect());
    };
    const QRect oldCursor = cursorRect(m_playheadSeconds);
    m_playheadSeconds = seconds;
    const QRect dirty = oldCursor.united(cursorRect(m_playheadSeconds));
    if (!dirty.isEmpty()) update(dirty);
}

double WaveformStrip::secondsAt(double x) const {
    if (width() <= 0 || m_peaks.durationSeconds <= 0.0) return 0.0;
    const double fraction = std::clamp(x / double(width()), 0.0, 1.0);
    return fraction * m_peaks.durationSeconds;
}

void WaveformStrip::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const Theme& t = th();
    const QRectF area = rect().adjusted(0, 0, -1, -1);
    p.fillRect(rect(), t.well());
    p.setPen(QPen(t.separator(), 1.0));
    p.drawLine(area.topLeft(), area.topRight());

    if (!m_peaks.isValid()) {
        if (!m_message.isEmpty()) {
            p.setPen(t.textSecondary);
            p.drawText(area, Qt::AlignCenter, m_message);
        }
        return;
    }

    ui::PeakPaint how;
    how.sourceStartSeconds = 0.0;
    how.secondsPerPixel = m_peaks.durationSeconds / std::max(1.0, double(width()));
    // paintPeaks builds one point per horizontal pixel. Restrict it to the
    // actual dirty hairline instead of rebuilding the complete envelope on
    // every 16 ms cursor tick.
    const QRect dirty = event ? event->rect() : rect();
    how.clipLeft = std::max(0.0, double(dirty.left() - 1));
    how.clipRight = std::min(double(width()), double(dirty.right() + 2));
    how.gain = 1.0f;
    how.color = t.waveform;
    ui::paintPeaks(p, &m_peaks, area.adjusted(0, 3, 0, -3), how);

    if (m_playheadSeconds >= 0.0 && m_peaks.durationSeconds > 0.0) {
        const double x = std::clamp(m_playheadSeconds / m_peaks.durationSeconds,
                                    0.0, 1.0) *
                         double(width());
        p.setPen(QPen(t.cursor, 1.5));
        p.drawLine(QPointF(x, area.top() + 1.0), QPointF(x, area.bottom()));
    }
}

void WaveformStrip::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() != Qt::LeftButton || !m_peaks.isValid()) return;
    emit seekRequested(secondsAt(ev->position().x()));
    ev->accept();
}

void WaveformStrip::mouseMoveEvent(QMouseEvent* ev) {
    // Scrubbing: the same gesture continued, so dragging along the strip moves
    // the audition rather than needing a click per position.
    if (!(ev->buttons() & Qt::LeftButton) || !m_peaks.isValid()) return;
    emit seekRequested(secondsAt(ev->position().x()));
    ev->accept();
}
