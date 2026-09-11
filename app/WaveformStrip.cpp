#include "WaveformStrip.hpp"

#include "MidiFile.hpp"
#include "Theme.hpp"
#include "WaveformPaint.hpp"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace {
/// Enough height for the envelope to say something about the sound, short
/// enough that the tree above it keeps the panel.
constexpr int kHeight = 56;
/// A 56-pixel preview cannot distinguish more than this many overlapping
/// rectangles. Sampling a very dense file keeps selection responsive while
/// preserving its overall rhythm and pitch contour.
constexpr std::size_t kMaxMidiPreviewNotes = 12000;

QRectF previewArea(const QWidget& widget) {
    return QRectF(widget.rect()).adjusted(6.0, 4.0, -6.0, -2.0);
}
} // namespace

WaveformStrip::WaveformStrip(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kHeight);
    setCursor(Qt::PointingHandCursor);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            [this] {
                rebuildMidiLayer();
                update();
            });
}

void WaveformStrip::setPeaks(const daw::WaveformPeaks& peaks) {
    m_peaks = peaks;
    m_midi.reset();
    m_midiLayer = {};
    m_message.clear();
    m_playheadSeconds = -1.0;
    setCursor(Qt::PointingHandCursor);
    update();
}

void WaveformStrip::setMidi(
    std::shared_ptr<const daw::midifile::File> file) {
    m_peaks = daw::WaveformPeaks{};
    m_midi = std::move(file);
    m_message.clear();
    m_playheadSeconds = -1.0;
    setCursor(Qt::ArrowCursor);
    rebuildMidiLayer();
    update();
}

void WaveformStrip::clear(const QString& message) {
    m_peaks = daw::WaveformPeaks{};
    m_midi.reset();
    m_midiLayer = {};
    m_message = message;
    m_playheadSeconds = -1.0;
    setCursor(Qt::ArrowCursor);
    update();
}

void WaveformStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_midi) rebuildMidiLayer();
}

void WaveformStrip::rebuildMidiLayer() {
    m_midiLayer = {};
    if (!m_midi || m_midi->notes.empty() || width() <= 0 || height() <= 0)
        return;

    const qreal dpr = devicePixelRatioF();
    QPixmap layer(QSize(std::max(1, int(std::ceil(width() * dpr))),
                        std::max(1, int(std::ceil(height() * dpr)))));
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);

    int low = 127;
    int high = 0;
    for (const auto& note : m_midi->notes) {
        low = std::min(low, note.pitch);
        high = std::max(high, note.pitch);
    }
    // A single-note phrase still needs a readable vertical scale. Keep at
    // least one octave, with a little air above and below the material.
    const int centre = (low + high) / 2;
    if (high - low < 11) {
        low = centre - 6;
        high = centre + 6;
    } else {
        --low;
        ++high;
    }
    low = std::max(0, low);
    high = std::min(127, high);

    const double length = std::max(1e-6, m_midi->lengthBeats);
    const QRectF content = previewArea(*this).adjusted(1.0, 2.0, -1.0, -2.0);
    const double pitchSpan = std::max(1, high - low + 1);

    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QColor grid = th().separator();
    grid.setAlphaF(0.42);
    painter.setPen(QPen(grid, 1.0));
    const int beatStep = std::max(1, int(std::ceil(length / 24.0)));
    for (int beat = beatStep; beat < int(std::ceil(length)); beat += beatStep) {
        const qreal x = content.left() + content.width() * beat / length;
        painter.drawLine(QPointF(x, content.top()), QPointF(x, content.bottom()));
    }

    QColor notes = Theme::midiAccent();
    notes.setAlphaF(0.90);
    painter.setPen(Qt::NoPen);
    painter.setBrush(notes);
    QPainterPath shapes;
    const std::size_t stride = std::max<std::size_t>(
        1, (m_midi->notes.size() + kMaxMidiPreviewNotes - 1) /
               kMaxMidiPreviewNotes);
    const qreal rowHeight = content.height() / pitchSpan;
    for (std::size_t i = 0; i < m_midi->notes.size(); i += stride) {
        const auto& note = m_midi->notes[i];
        const qreal x = content.left() +
                        content.width() * note.startBeats / length;
        const qreal w = std::max<qreal>(1.4,
            content.width() * note.lengthBeats / length);
        const qreal y = content.top() +
            (high - std::clamp(note.pitch, low, high)) * rowHeight;
        const qreal h = std::max<qreal>(1.6, rowHeight * 0.78);
        const QRectF noteRect(x, y + (rowHeight - h) * 0.5,
                              std::min(w, content.right() - x), h);
        if (noteRect.width() > 0.0)
            shapes.addRoundedRect(noteRect, std::min<qreal>(1.5, h / 2.0),
                                  std::min<qreal>(1.5, h / 2.0));
    }
    painter.drawPath(shapes);
    m_midiLayer = std::move(layer);
}

void WaveformStrip::setPlayheadSeconds(double seconds) {
    const auto cursorRect = [this](double at) {
        const QRectF area = previewArea(*this);
        if (at < 0.0 || area.width() <= 0.0 ||
            m_peaks.durationSeconds <= 0.0)
            return QRect{};
        const double x = area.left() +
            std::clamp(at / m_peaks.durationSeconds, 0.0, 1.0) * area.width();
        return QRect(int(std::floor(x)) - 3, int(area.top()), 7,
                     int(std::ceil(area.height())))
            .intersected(rect());
    };
    const QRect oldCursor = cursorRect(m_playheadSeconds);
    m_playheadSeconds = seconds;
    const QRect dirty = oldCursor.united(cursorRect(m_playheadSeconds));
    if (!dirty.isEmpty()) update(dirty);
}

double WaveformStrip::secondsAt(double x) const {
    const QRectF area = previewArea(*this);
    if (area.width() <= 0.0 || m_peaks.durationSeconds <= 0.0) return 0.0;
    const double fraction =
        std::clamp((x - area.left()) / area.width(), 0.0, 1.0);
    return fraction * m_peaks.durationSeconds;
}

void WaveformStrip::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const Theme& t = th();
    const QRectF area = previewArea(*this);
    QPainterPath card;
    card.addRoundedRect(area, 7.0, 7.0);
    p.fillPath(card, t.well());
    p.setPen(QPen(t.separator(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(card);
    p.save();
    p.setClipPath(card);

    if (m_midi) {
        if (!m_midiLayer.isNull()) p.drawPixmap(0, 0, m_midiLayer);
        p.restore();
        return;
    }

    if (!m_peaks.isValid()) {
        if (!m_message.isEmpty()) {
            p.setPen(t.textSecondary);
            p.drawText(area, Qt::AlignCenter, m_message);
        }
        p.restore();
        return;
    }

    ui::PeakPaint how;
    how.sourceStartSeconds = 0.0;
    how.secondsPerPixel =
        m_peaks.durationSeconds / std::max(1.0, area.width());
    // paintPeaks builds one point per horizontal pixel. Restrict it to the
    // actual dirty hairline instead of rebuilding the complete envelope on
    // every 16 ms cursor tick.
    const QRect dirty = event ? event->rect() : rect();
    how.clipLeft = std::max(area.left(), double(dirty.left() - 1));
    how.clipRight = std::min(area.right(), double(dirty.right() + 2));
    how.gain = 1.0f;
    how.color = t.waveform;
    ui::paintPeaks(p, &m_peaks, area.adjusted(1, 3, -1, -3), how);

    if (m_playheadSeconds >= 0.0 && m_peaks.durationSeconds > 0.0) {
        const double x = std::clamp(m_playheadSeconds / m_peaks.durationSeconds,
                                    0.0, 1.0) *
                         area.width() + area.left();
        p.setPen(QPen(t.cursor, 1.5));
        p.drawLine(QPointF(x, area.top() + 1.0), QPointF(x, area.bottom()));
    }
    p.restore();
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
