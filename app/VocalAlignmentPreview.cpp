#include "VocalAlignmentPreview.hpp"
#include "EngineController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "WaveformPaint.hpp"
#include "Job/BackgroundExecutor.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <atomic>
#include <cmath>
#include <limits>

struct VocalAlignmentPreview::PeakJob {
    std::atomic<bool> cancelled{false}, done{false};
    std::shared_ptr<const daw::WaveformPeaks> guide, target;
};

VocalAlignmentPreview::VocalAlignmentPreview(QWidget* parent) : QWidget(parent) {
    setObjectName("AlignmentPreview");
    setMinimumSize(340, 220); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
    setAccessibleName(tr("Guide, before and after waveforms"));
    setToolTip(tr("Wheel to zoom · Drag to pan · Home to fit · Arrow keys to pan"));
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, qOverload<>(&QWidget::update));
    auto* timer = new QTimer(this); timer->setInterval(40);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!m_peakJob || !m_peakJob->done.load(std::memory_order_acquire)) return;
        m_guidePeaks = m_peakJob->guide; m_targetPeaks = m_peakJob->target;
        m_peakJob.reset(); update();
    });
    timer->start();
}
VocalAlignmentPreview::~VocalAlignmentPreview() { if (m_peakJob) m_peakJob->cancelled = true; }

void VocalAlignmentPreview::setLive(daw::EngineController* controller, const ui::ClipSel& guide, const ui::ClipSel& target) {
    const bool changed = m_controller != controller || m_guide != guide || m_target != target;
    if (m_peakJob) m_peakJob->cancelled = true;
    m_peakJob.reset(); m_guidePeaks.reset(); m_targetPeaks.reset();
    m_guideSource = {}; m_targetSource = {}; m_result.reset();
    m_controller = controller; m_guide = guide; m_target = target;
    if (changed) m_fitted = true;
    updateBounds(); update();
}
void VocalAlignmentPreview::setSnapshot(const daw::alignment::AlignmentSource& guide,
    const daw::alignment::AlignmentSource& target, const std::optional<daw::alignment::AlignmentProposal>& result) {
    const bool guideChanged = m_guideSource.audio != guide.audio, targetChanged = m_targetSource.audio != target.audio;
    m_controller = nullptr; m_guideSource = guide; m_targetSource = target; m_result = result;
    if (guideChanged || targetChanged) {
        if (m_peakJob) m_peakJob->cancelled = true;
        if (guideChanged) m_guidePeaks.reset();
        if (targetChanged) m_targetPeaks.reset();
        auto job = std::make_shared<PeakJob>(); m_peakJob = job;
        daw::engine::BackgroundExecutor::instance().submit([job, a = guide.audio, b = target.audio, readyGuide = m_guidePeaks, readyTarget = m_targetPeaks] {
            try {
                const auto keep = [job] { return !job->cancelled.load(); };
                if (readyGuide) job->guide = readyGuide;
                else if (a && keep()) { auto peaks = std::make_shared<daw::WaveformPeaks>(); daw::buildPeaks(*a, *peaks, keep); job->guide = std::move(peaks); }
                if (b && keep()) {
                    if (readyTarget) job->target = readyTarget;
                    else if (b == a) job->target = job->guide;
                    else { auto peaks = std::make_shared<daw::WaveformPeaks>(); daw::buildPeaks(*b, *peaks, keep); job->target = std::move(peaks); }
                }
            } catch (...) { job->guide.reset(); job->target.reset(); }
            job->done.store(true, std::memory_order_release);
        });
    }
    updateBounds(); update();
}
VocalAlignmentPreview::Wave VocalAlignmentPreview::wave(int lane) const {
    Wave out;
    if (m_controller) {
        if (lane == 2) return out;
        const auto& ref = lane == 0 ? m_guide : m_target;
        const auto* clip = m_controller->audioClip(ref.trackId.toStdString(), ref.clipId.toStdString());
        if (!clip) return out;
        const auto view = m_controller->clipWaveform(*clip);
        out.peaks = view.peaks; out.samples = view.samples; out.reversed = view.reversed;
        out.start = clip->startSeconds; out.duration = m_controller->clipDisplayDuration(*clip); out.tempo = m_controller->tempo();
        const bool printed = m_controller->clipDisplayFilePath(*clip) != clip->filePath;
        if (!printed && clip->warp.enabled && clip->warp.markers.size() >= 2) out.map = clip->warp;
        else {
            const double offset = printed ? 0 : clip->offsetSeconds;
            const double stretch = printed ? 1 : std::max(.001, clip->sampleEdit.stretchTime);
            out.map.markers = {{"begin", offset, 0, true}, {"end", offset + out.duration / stretch, out.duration * out.tempo / 60, true}};
        }
    } else {
        const auto& source = lane == 0 ? m_guideSource : m_targetSource;
        out.peaks = lane == 0 ? m_guidePeaks.get() : m_targetPeaks.get(); out.samples = source.audio.get();
        out.map = source.baseline; out.start = source.startSeconds; out.duration = source.durationSeconds; out.tempo = source.tempo;
        if (lane == 2) {
            if (!m_result) return {};
            out.map = m_result->map; out.start = m_result->startSeconds; out.duration = m_result->durationSeconds;
        }
    }
    return out;
}
bool VocalAlignmentPreview::waveformsReady() const { return wave(0).peaks && wave(1).peaks; }
double VocalAlignmentPreview::projectTimeForSource(int lane, double seconds) const {
    const auto w = wave(lane); return w.start + daw::warpBeatAt(w.map, seconds) * 60 / w.tempo;
}
QRectF VocalAlignmentPreview::plot() const {
    const auto fm = fontMetrics();
    const int gutter = std::max({fm.horizontalAdvance(tr("Guide")), fm.horizontalAdvance(tr("Before")), fm.horizontalAdvance(tr("After"))}) + 30;
    return QRectF(gutter, 30, std::max(1, width() - gutter - 16), std::max(1, height() - 56));
}
void VocalAlignmentPreview::updateBounds() {
    double begin = std::numeric_limits<double>::max(), end = 0;
    for (int lane = 0; lane < 3; ++lane) { const auto w = wave(lane); if (w.duration <= 0) continue; begin = std::min(begin, w.start); end = std::max(end, w.start + w.duration); }
    if (end <= begin) { begin = 0; end = 1; }
    const double padding = std::max(.015, (end - begin) * .035);
    m_begin = std::max(0., begin - padding); m_end = end + padding;
    if (m_fitted) fit(); else { m_span = std::min(m_span, m_end - m_begin); pan(m_start); }
}
void VocalAlignmentPreview::refreshLive() { if (m_controller) { updateBounds(); update(); } }
void VocalAlignmentPreview::fit() { m_fitted = true; m_start = m_begin; m_span = m_end - m_begin; update(); }
void VocalAlignmentPreview::zoom(double factor, double anchor) {
    anchor = std::clamp(anchor, 0., 1.);
    const double span = std::clamp(m_span / factor, .01, m_end - m_begin);
    const double start = m_start + (m_span - span) * anchor;
    m_span = span; m_fitted = span == m_end - m_begin; pan(start);
}
void VocalAlignmentPreview::pan(double start) { m_start = std::clamp(start, m_begin, std::max(m_begin, m_end - m_span)); update(); }
void VocalAlignmentPreview::setShowChanges(bool on) { m_showChanges = on; update(); }

void VocalAlignmentPreview::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const auto& t = ThemeManager::instance().theme(); const auto area = plot();
    p.setPen(QPen(t.separator(), 1)); p.setBrush(t.well());
    p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1), 8, 8);
    const auto x = [&](double seconds) { return area.left() + (seconds - m_start) * area.width() / m_span; };
    const double row = area.height() / 3;
    const QColor colors[] = {t.dark ? Theme::audioAccent().lighter(135) : Theme::audioAccent().darker(120), t.textSecondary, t.accent};
    const QString labels[] = {tr("Guide"), tr("Before"), tr("After")};
    const icons::Glyph glyphs[] = {icons::Glyph::Mic, icons::Glyph::Ghost, icons::Glyph::WarpAlign};
    const double roughStep = m_span / std::max(1., area.width() / 90.);
    const double unit = std::pow(10., std::floor(std::log10(roughStep)));
    const double fraction = roughStep / unit;
    const double step = (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10) * unit;
    p.save(); p.setClipRect(QRectF(area.left(), 0, area.width(), height()));
    for (double at = std::ceil(m_start / step) * step; at < m_start + m_span; at += step) {
        p.setPen(t.gridLine); p.drawLine(QPointF(x(at), area.top()), QPointF(x(at), area.bottom()));
        p.setPen(t.textSecondary); p.drawText(QRectF(x(at) + 4, 4, 85, 22), Qt::AlignVCenter, tr("%1 s").arg(at, 0, 'f', step < .1 ? 3 : step < 1 ? 2 : 1));
    }
    p.restore();
    for (int lane = 0; lane < 3; ++lane) {
        const double top = area.top() + lane * row;
        const QRectF body(area.left(), top + 13, area.width(), row - 26);
        icons::paint(p, glyphs[lane], QRectF(16, top + row / 2 - 23, 18, 18), colors[lane]);
        p.setPen(t.textPrimary); p.drawText(QRectF(14, top + row / 2, area.left() - 22, 22), Qt::AlignLeft | Qt::AlignVCenter, labels[lane]);
        p.setPen(t.separator()); p.drawLine(QPointF(area.left(), top + row), QPointF(area.right(), top + row));
        const auto w = wave(lane);
        if (w.duration <= 0) {
            p.setPen(t.textSecondary);
            p.drawText(body.adjusted(12,0,-12,0), Qt::AlignCenter | Qt::TextWordWrap,
                lane == 2 ? tr("Press Align to see the result here") : lane == 0 ? tr("Add a guide clip") : tr("Select a target clip"));
            continue;
        }
        p.save(); p.setClipRect(body);
        auto fill = colors[lane]; fill.setAlpha(t.dark ? 14 : 12);
        p.fillRect(QRectF(x(w.start), body.top(), x(w.start + w.duration) - x(w.start), body.height()), fill);
        p.setPen(QPen(colors[lane], 1, Qt::DotLine));
        p.drawLine(QPointF(x(w.start), body.top()), QPointF(x(w.start), body.bottom()));
        p.drawLine(QPointF(x(w.start + w.duration), body.top()), QPointF(x(w.start + w.duration), body.bottom()));
        for (size_t i = 0; i + 1 < w.map.markers.size(); ++i) {
            const auto& a = w.map.markers[i]; const auto& b = w.map.markers[i + 1];
            const QRectF segment(x(w.start + a.targetBeats * 60 / w.tempo), body.top(),
                (b.targetBeats - a.targetBeats) * 60 / w.tempo * area.width() / m_span, body.height());
            if (!segment.intersects(body)) continue;
            ui::PeakPaint how; how.sourceStartSeconds = a.sourceSeconds;
            how.secondsPerPixel = (b.sourceSeconds - a.sourceSeconds) / segment.width();
            how.clipLeft = std::max(body.left(), segment.left()); how.clipRight = std::min(body.right(), segment.right());
            how.color = colors[lane]; how.samples = w.samples; how.reversed = w.reversed;
            ui::paintPeaks(p, w.peaks, segment, how);
        }
        if (!w.peaks) { p.setPen(t.textSecondary); p.drawText(body, Qt::AlignCenter, m_peakJob ? tr("Loading waveform…") : tr("Waveform unavailable")); }
        p.restore();
    }
    if (m_result && m_showChanges && m_result->status != daw::alignment::AlignmentStatus::Skipped) {
        const auto before = wave(1), after = wave(2);
        auto markers = m_result->map.markers;
        if (markers.size() == 2) { // A rigid shift has no internal Warp markers.
            const auto a = markers.front(), b = markers.back();
            for (int i = 1; i <= 3; ++i) { auto marker = a; marker.sourceSeconds = a.sourceSeconds + (b.sourceSeconds - a.sourceSeconds) * i / 4.; markers.insert(markers.end() - 1, marker); }
        }
        p.save(); p.setClipRect(area); double last = -1000;
        const double fromY = area.top() + row * 2 - 12, toY = fromY + 24;
        for (const auto& marker : markers) {
            const double from = x(before.start + daw::warpBeatAt(before.map, marker.sourceSeconds) * 60 / before.tempo);
            const double to = x(after.start + daw::warpBeatAt(after.map, marker.sourceSeconds) * 60 / after.tempo);
            if (std::abs(from - to) < .5 || (from < area.left() && to < area.left()) || (from > area.right() && to > area.right()) || to - last < 28) continue;
            last = to; p.setPen(QPen(t.accent, 1.2));
            p.drawLine(QPointF(from, fromY), QPointF(to, toY));
            p.setBrush(t.well()); p.drawEllipse(QPointF(from, fromY), 2.5, 2.5);
            p.setBrush(t.accent); p.drawEllipse(QPointF(to, toY), 2.5, 2.5);
        }
        p.restore();
    }
    if (m_hoverX >= area.left() && m_hoverX <= area.right()) {
        p.setPen(QPen(t.textPrimary, 1, Qt::DashLine)); p.drawLine(QPointF(m_hoverX, area.top()), QPointF(m_hoverX, area.bottom()));
        const double at = m_start + (m_hoverX - area.left()) / area.width() * m_span;
        QString text = tr("%1 s").arg(at, 0, 'f', 3);
        if (m_result && at >= m_result->startSeconds && at <= m_result->startSeconds + m_result->durationSeconds) {
            const double source = daw::warpSourceAt(m_result->map, (at - m_result->startSeconds) * m_targetSource.tempo / 60);
            const double shift = (at - projectTimeForSource(1, source)) * 1000;
            text += tr(" · Shift %1 ms").arg(shift, 0, 'f', 1);
        }
        p.setPen(t.textPrimary); p.drawText(QRectF(area.left(), height() - 23, area.width(), 20), Qt::AlignRight | Qt::AlignVCenter, text);
    }
}
void VocalAlignmentPreview::mousePressEvent(QMouseEvent* event) {
    if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) && plot().contains(event->position())) {
        setFocus(); m_panning = true; m_panStart = m_start; m_panX = event->position().x(); setCursor(Qt::ClosedHandCursor); event->accept();
    }
}
void VocalAlignmentPreview::mouseMoveEvent(QMouseEvent* event) {
    m_hoverX = event->position().x();
    if (m_panning) pan(m_panStart - (m_hoverX - m_panX) / plot().width() * m_span);
    update();
}
void VocalAlignmentPreview::mouseReleaseEvent(QMouseEvent*) { m_panning = false; unsetCursor(); }
void VocalAlignmentPreview::leaveEvent(QEvent*) { m_hoverX = -1; update(); }
void VocalAlignmentPreview::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::ShiftModifier)) pan(m_start - event->angleDelta().y() / 120. * m_span * .1);
    else zoom(std::pow(1.25, event->angleDelta().y() / 120.), (event->position().x() - plot().left()) / plot().width());
    event->accept();
}
void VocalAlignmentPreview::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Home: fit(); break;
    case Qt::Key_Plus: case Qt::Key_Equal: zoom(1.5); break;
    case Qt::Key_Minus: zoom(1 / 1.5); break;
    case Qt::Key_Left: pan(m_start - m_span * .1); break;
    case Qt::Key_Right: pan(m_start + m_span * .1); break;
    case Qt::Key_Escape: if (m_panning) { pan(m_panStart); m_panning = false; unsetCursor(); } else { QWidget::keyPressEvent(event); return; } break;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
}
