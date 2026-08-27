#include "TimelineView.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>

#include <algorithm>
#include <cmath>
#include <cstdint>

static constexpr int kScrollW = 14;
static constexpr int kLabelWidth = 120;

TimelineView::TimelineView(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(300, 100);
    setMouseTracking(true);

    hScroll_ = new QScrollBar(Qt::Horizontal, this);
    hScroll_->setMinimum(0);
    hScroll_->setSingleStep(100);
    hScroll_->setPageStep(500);

    vScroll_ = new QScrollBar(Qt::Vertical, this);
    vScroll_->setMinimum(0);
    vScroll_->setSingleStep(kTrackHeight + kTrackGap);

    connect(hScroll_, &QScrollBar::valueChanged, this, [this](int val) {
        scrollOffset_ = static_cast<double>(val) * samplesPerPixel_;
        emit scrollChanged(scrollOffset_);
        update();
    });

    connect(vScroll_, &QScrollBar::valueChanged, this, [this](int) {
        update();
    });
}

void TimelineView::setSession(std::shared_ptr<daw::model::Session> session)
{
    session_ = std::move(session);
    updateScrollbars();
    update();
}

void TimelineView::setPeakFile(const std::string& audioPath,
                               std::shared_ptr<daw::graph::PeakFile> peakFile)
{
    peakFiles_[audioPath] = std::move(peakFile);
    update();
}

void TimelineView::setPlayheadPosition(std::int64_t samples)
{
    playheadSamples_ = samples;
    if (playing_) {
        const int w = viewWidth();
        const double playheadPx = samples / samplesPerPixel_;
        const double viewLeft = scrollOffset_ / samplesPerPixel_;
        const double viewRight = viewLeft + w;

        if (playheadPx > viewRight - 50) {
            hScroll_->setValue(static_cast<int>(std::round(
                (samples - w * samplesPerPixel_ * 0.25) / samplesPerPixel_)));
        } else if (playheadPx < viewLeft + 10 && hScroll_->value() > 0) {
            hScroll_->setValue(hScroll_->value() - hScroll_->singleStep());
        }
    }
    update();
}

void TimelineView::setZoom(double samplesPerPixel)
{
    samplesPerPixel_ = std::clamp(samplesPerPixel, 1.0, 100000.0);
    updateScrollbars();
    update();
}

void TimelineView::setScrollOffset(double samples)
{
    scrollOffset_ = std::max(0.0, samples);
    updateScrollbars();
    update();
}

void TimelineView::setSampleRate(double rate) { sampleRate_ = rate; }
void TimelineView::setPlaying(bool playing) { playing_ = playing; }

QSize TimelineView::sizeHint() const { return QSize(800, 400); }
QSize TimelineView::minimumSizeHint() const { return QSize(300, 100); }

int TimelineView::viewWidth() const { return width() - kLabelWidth - kScrollW; }
int TimelineView::viewHeight() const { return height() - kScrollW; }

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void TimelineView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const int w = width();
    const int h = viewHeight();

    if (w <= kLabelWidth + kScrollW || h <= 0)
        return;

    const int contentRight = w - kScrollW;

    p.fillRect(rect(), QColor(18, 18, 20));

    // Label column background
    p.fillRect(QRect(0, 0, kLabelWidth, h), QColor(28, 28, 30));
    p.setPen(QColor(42, 42, 45));
    p.drawLine(kLabelWidth, 0, kLabelWidth, h);

    if (!session_ || session_->trackCount() == 0) {
        p.setPen(QColor(90, 95, 102));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("Откройте аудиофайл (Ctrl+O)"));
        return;
    }

    const int trackH = kTrackHeight;
    const int trackStep = trackH + kTrackGap;
    const int vOffset = vScroll_->value();

    for (int ti = 0; ti < session_->trackCount(); ++ti) {
        const auto* track = session_->track(ti);
        if (!track)
            continue;

        const int y = ti * trackStep - vOffset;
        if (y + trackH < 0 || y > h)
            continue;

        // Track label
        const QRect labelRect(0, y, kLabelWidth - 1, trackH);
        p.fillRect(labelRect, QColor(32, 34, 37));
        p.setPen(QColor(180, 184, 190));
        p.drawText(labelRect.adjusted(6, 0, -4, 0),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromStdString(track->name()));

        // Track background
        const QRect trackRect(kLabelWidth, y, contentRight - kLabelWidth, trackH);
        p.fillRect(trackRect, (ti % 2 == 0) ? QColor(22, 22, 24) : QColor(26, 26, 28));

        paintTrackRow(p, y, trackH, *track);
    }

    // Playhead
    const double sampleToPx = 1.0 / samplesPerPixel_;
    const double playheadPx = (playheadSamples_ - scrollOffset_) * sampleToPx + kLabelWidth;
    if (playheadPx >= kLabelWidth && playheadPx < contentRight) {
        p.setPen(QPen(QColor(240, 180, 60), 2));
        p.drawLine(QPointF(playheadPx, 0), QPointF(playheadPx, h));
    }
}

void TimelineView::paintTrackRow(QPainter& p, int y, int h, const daw::model::Track& track)
{
    for (const auto& clip : track.clips()) {
        const auto* source = clip->source();
        if (!source)
            continue;

        const std::string& filePath = source->filePath();
        auto it = peakFiles_.find(filePath);
        const auto* peakFile = (it != peakFiles_.end()) ? it->second.get() : nullptr;

        paintClip(p, *clip, peakFile, y, h);
    }
}

void TimelineView::paintClip(QPainter& p, const daw::model::Clip& clip,
                             const daw::graph::PeakFile* peakFile,
                             int trackY, int trackH)
{
    const double sampleToPx = 1.0 / samplesPerPixel_;

    const double clipStartPx = (clip.timelinePosition() - scrollOffset_) * sampleToPx + kLabelWidth;
    const double clipEndPx = (clip.timelineEnd() - scrollOffset_) * sampleToPx + kLabelWidth;

    const int contentRight = width() - kScrollW;
    if (clipEndPx < kLabelWidth || clipStartPx > contentRight)
        return;

    const double clipWPx = clipEndPx - clipStartPx;
    const QRectF clipRect(clipStartPx, trackY + 1, clipWPx, trackH - 2);

    p.fillRect(clipRect, QColor(38, 42, 50));
    p.setPen(QColor(70, 78, 90));
    p.drawRect(clipRect);

    if (!peakFile || !peakFile->isValid()) {
        p.setPen(QColor(90, 95, 102));
        p.drawText(clipRect, Qt::AlignCenter,
                   peakFile ? QStringLiteral("Peaks\u2026") : QStringLiteral("No peaks"));
        return;
    }

    const int level = chooseLevel(*peakFile);
    const auto& peaks = peakFile->level(level);
    if (peaks.empty())
        return;

    const double blockFrames = peakFile->framesPerLevel(level);
    const double offsetInSource = static_cast<double>(clip.offset());
    const double lengthInSource = static_cast<double>(clip.length());

    const int drawX = static_cast<int>(clipStartPx);
    const int drawW = static_cast<int>(clipWPx);

    if (drawW <= 0)
        return;

    const int numPixels = std::min(drawW, contentRight - drawX);
    if (numPixels <= 0)
        return;

    const double samplesPerDrawPixel = lengthInSource / std::max(1, numPixels);

    const int midY = trackY + trackH / 2;
    const int halfH = (trackH - 6) / 2;

    QPainter::PixmapFragment frags[512];
    int fragCount = 0;

    p.setPen(QColor(140, 200, 255));

    for (int px = 0; px < numPixels; ++px) {
        const double centerSample = offsetInSource + (px + 0.5) * samplesPerDrawPixel;
        const int bi = static_cast<int>(centerSample / blockFrames);
        if (bi < 0 || bi >= static_cast<int>(peaks.size()))
            continue;

        const auto& pair = peaks[bi];
        const float vMin = static_cast<float>(pair.min) / 32767.0f;
        const float vMax = static_cast<float>(pair.max) / 32767.0f;

        const int y0 = midY + static_cast<int>(vMin * halfH);
        const int y1 = midY + static_cast<int>(vMax * halfH);
        const int clampY0 = std::clamp(y0, trackY + 1, trackY + trackH - 1);
        const int clampY1 = std::clamp(y1, trackY + 1, trackY + trackH - 1);

        if (clampY0 != clampY1)
            p.drawLine(drawX + px, clampY0, drawX + px, clampY1);
        else
            p.drawPoint(drawX + px, clampY0);
    }
}

int TimelineView::chooseLevel(const daw::graph::PeakFile& pf) const
{
    const double blockFramesL0 = pf.framesPerLevel(0);
    if (blockFramesL0 <= 0)
        return 0;

    const double targetBlockFrames = samplesPerPixel_ * 1.5;
    int level = 0;
    while (level + 1 < pf.numLevels() && pf.framesPerLevel(level + 1) <= targetBlockFrames)
        ++level;

    return level;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void TimelineView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && event->pos().x() >= kLabelWidth) {
        const double sample = (event->pos().x() - kLabelWidth) * samplesPerPixel_ + scrollOffset_;
        emit locateRequested(static_cast<std::int64_t>(sample));
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        dragStartX_ = event->pos().x();
        dragStartScroll_ = scrollOffset_;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void TimelineView::mouseMoveEvent(QMouseEvent* event)
{
    if (dragStartX_ >= 0) {
        const int dx = event->pos().x() - dragStartX_;
        setScrollOffset(dragStartScroll_ - dx * samplesPerPixel_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton && dragStartX_ >= 0) {
        dragStartX_ = -1;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TimelineView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const double factor = (event->angleDelta().y() > 0) ? 1.0 / 1.15 : 1.15;
        double newZoom = samplesPerPixel_ * factor;
        newZoom = std::clamp(newZoom, 1.0, 100000.0);
        if (newZoom != samplesPerPixel_) {
            samplesPerPixel_ = newZoom;
            updateScrollbars();
            emit zoomChanged(samplesPerPixel_);
            update();
        }
        event->accept();
    } else if (event->modifiers() & Qt::ShiftModifier) {
        hScroll_->setValue(hScroll_->value() - event->angleDelta().y());
        event->accept();
    } else {
        QWidget::wheelEvent(event);
    }
}

void TimelineView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    const int w = width();
    const int h = height();

    hScroll_->setGeometry(kLabelWidth, h - kScrollW, w - kLabelWidth - kScrollW, kScrollW);
    vScroll_->setGeometry(w - kScrollW, 0, kScrollW, h - kScrollW);

    updateScrollbars();
}

// ---------------------------------------------------------------------------
// Scrollbars
// ---------------------------------------------------------------------------

int TimelineView::totalContentWidth() const
{
    if (!session_)
        return viewWidth();

    std::int64_t maxEnd = 0;
    for (int ti = 0; ti < session_->trackCount(); ++ti) {
        const auto* track = session_->track(ti);
        if (!track) continue;
        for (const auto& clip : track->clips()) {
            const auto end = clip->timelineEnd();
            if (end > maxEnd) maxEnd = end;
        }
    }

    return std::max(viewWidth(), static_cast<int>(maxEnd / samplesPerPixel_) + 200);
}

int TimelineView::totalContentHeight() const
{
    if (!session_)
        return viewHeight();
    const int trackStep = kTrackHeight + kTrackGap;
    return std::max(viewHeight(), session_->trackCount() * trackStep + 200);
}

void TimelineView::updateScrollbars()
{
    const int viewW = viewWidth();
    const int viewH = viewHeight();

    const int contentW = totalContentWidth();
    const int contentH = totalContentHeight();

    hScroll_->setRange(0, std::max(0, contentW - viewW));
    hScroll_->setPageStep(viewW);

    vScroll_->setRange(0, std::max(0, contentH - viewH));
    vScroll_->setPageStep(viewH);

    const bool hVis = contentW > viewW;
    const bool vVis = contentH > viewH;
    hScroll_->setVisible(hVis);
    vScroll_->setVisible(vVis);
}
