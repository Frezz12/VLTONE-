#include "SamplerPanel.hpp"
#include "FileTypes.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "PluginPickerMenu.hpp"
#include "Theme.hpp"

#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerVoice.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMimeData>
#include <QGridLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace sampler = daw::plugins::sampler;

namespace {

/// How often the panel re-reads the instance. The same 200 ms the generic
/// plugin editor uses — enough for an automated knob to look alive, cheap
/// enough to leave running while the window is open.
constexpr int kPollMs = 200;

const daw::plugins::ParameterInfo* infoFor(const QString& id) {
    const std::string needle = id.toStdString();
    for (const daw::plugins::ParameterInfo& info : sampler::parameterTable()) {
        if (info.id == needle) return &info;
    }
    return nullptr;
}

/// A titled block of controls. Everything on both pages is one of these, so the
/// panel reads as sections rather than as a field of knobs.
QWidget* sectionBox(const QString& title, QLayout* content, QWidget* parent) {
    auto* box = new QWidget(parent);
    box->setObjectName(QStringLiteral("SamplerSection"));
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(10, 8, 10, 10);
    column->setSpacing(6);
    column->addWidget(ui::sectionLabel(title, box));
    column->addLayout(content);
    return box;
}

QHBoxLayout* knobRow() {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    return row;
}

QLabel* caption(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName(QStringLiteral("SamplerCaption"));
    return label;
}

/// Resolution of the waveform strip's peak envelope. Wide enough that the
/// strip is never visibly blockier than a per-pixel scan would be, small enough
/// that mapping it onto pixels is free at paint time.
constexpr int kWaveformBuckets = 4096;

constexpr int kSamplerFxSlotHeight = 20;
constexpr int kSamplerFxActionSide = 16;
constexpr int kSamplerFxActionMargin = 2;

/// A compact insert row with the same interaction hierarchy as the mixer.
///
/// The plugin name owns the whole stable row while idle. Hovering does not add
/// widgets to a layout (which used to make the row jump and grow); it reveals
/// three actions over reserved positions: bypass on the left, open in the
/// centre and replace on the right. The name remains the click target between
/// them, and right-click keeps the complete context menu available.
class SamplerFxSlotRow final : public QWidget {
public:
    explicit SamplerFxSlotRow(QToolButton* slot, QWidget* parent = nullptr)
        : QWidget(parent), m_slot(slot), m_fullText(slot->text()) {
        m_slot->setParent(this);
        m_slot->installEventFilter(this);
        setFixedHeight(kSamplerFxSlotHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void addActionButton(QAbstractButton* button) {
        button->setParent(this);
        button->setFixedSize(kSamplerFxActionSide, kSamplerFxActionSide);
        button->hide();
        button->installEventFilter(this);
        m_actions.push_back(button);
        layoutRow();
    }

protected:
    void enterEvent(QEnterEvent*) override { refreshHover(); }
    void leaveEvent(QEvent*) override { refreshHover(); }
    void resizeEvent(QResizeEvent*) override {
        layoutRow();
        refreshHover();
    }

    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
            refreshHover();
        return false;
    }

private:
    void refreshHover() {
        static const bool forced = qEnvironmentVariableIsSet("DAW_SHOT_SLOT_HOVER");
        const bool hovered = forced || rect().contains(mapFromGlobal(QCursor::pos()));
        if (hovered == m_hovered) return;
        m_hovered = hovered;
        for (QAbstractButton* button : m_actions) button->setVisible(hovered);
        m_slot->setText(hovered ? QString() : m_fullText);
    }

    void layoutRow() {
        m_slot->setGeometry(rect());
        if (m_actions.isEmpty()) return;
        const int y = (height() - kSamplerFxActionSide) / 2;
        if (m_actions.size() >= 1)
            m_actions[0]->move(kSamplerFxActionMargin, y);
        if (m_actions.size() >= 2)
            m_actions[1]->move((width() - kSamplerFxActionSide) / 2, y);
        if (m_actions.size() >= 3)
            m_actions[2]->move(width() - kSamplerFxActionMargin -
                                   kSamplerFxActionSide,
                               y);
        for (QAbstractButton* button : m_actions) button->raise();
    }

    QToolButton* m_slot = nullptr;
    QString m_fullText;
    QVector<QAbstractButton*> m_actions;
    bool m_hovered = false;
};

} // namespace

// ── Waveform ───────────────────────────────────────────────────────────────

SamplerWaveform::SamplerWaveform(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(110);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            QOverload<>::of(&QWidget::update));
}

void SamplerWaveform::setSample(std::shared_ptr<const sampler::SampleData> sample) {
    const void* buffer = sample && sample->audio ? sample->audio.get() : nullptr;
    const bool sameAudio = buffer == m_peaksFor;
    const bool sameLength =
        m_sample && sample && m_sample->baseFrames == sample->baseFrames;
    m_sample = std::move(sample);
    if (!sameAudio) {
        rebuildPeaks();
        update();
        return;
    }
    // The panel polls; most of those polls hand back the very sample already on
    // screen. Repainting anyway would spend a gradient-filled path and two wide
    // stroked outlines, five times a second, to redraw the same picture.
    if (!sameLength) update();
}

void SamplerWaveform::setMarkers(double startOffset, double endOffset,
                                 double loopStart, double loopEnd, int loopMode,
                                 double fadeIn, double fadeOut) {
    const auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    if (same(startOffset, m_startOffset) && same(endOffset, m_endOffset) &&
        same(loopStart, m_loopStart) && same(loopEnd, m_loopEnd) &&
        loopMode == m_loopMode && same(fadeIn, m_fadeIn) &&
        same(fadeOut, m_fadeOut)) {
        return;
    }
    m_startOffset = startOffset;
    m_endOffset = endOffset;
    m_loopStart = loopStart;
    m_loopEnd = loopEnd;
    m_loopMode = loopMode;
    m_fadeIn = fadeIn;
    m_fadeOut = fadeOut;
    update();
}

void SamplerWaveform::rebuildPeaks() {
    m_minima.clear();
    m_maxima.clear();
    m_peaksFor = m_sample && m_sample->audio ? m_sample->audio.get() : nullptr;
    if (!m_peaksFor) return;

    const daw::engine::SampleBuffer& audio = *m_sample->audio;
    const daw::engine::FrameCount frames = audio.frames();
    if (frames == 0) return;

    // Fixed resolution, not one bucket per pixel: the strip is a few hundred
    // pixels wide and gets resized with the window, and this scan is the only
    // O(length of the sample) work the panel does.
    const int buckets =
        int(std::min<daw::engine::FrameCount>(frames, kWaveformBuckets));
    m_minima.resize(buckets);
    m_maxima.resize(buckets);
    const double perBucket = double(frames) / double(buckets);
    for (int b = 0; b < buckets; ++b) {
        const auto from = daw::engine::FrameCount(double(b) * perBucket);
        const auto to = std::min<daw::engine::FrameCount>(
            frames, std::max<daw::engine::FrameCount>(
                        from + 1, daw::engine::FrameCount(double(b + 1) * perBucket)));
        float low = 0.0f;
        float high = 0.0f;
        for (daw::engine::ChannelCount ch = 0; ch < audio.channels(); ++ch) {
            const float* data = audio.channel(ch);
            for (daw::engine::FrameCount i = from; i < to; ++i) {
                low = std::min(low, data[i]);
                high = std::max(high, data[i]);
            }
        }
        m_minima[b] = low;
        m_maxima[b] = high;
    }
}

double SamplerWaveform::xForFraction(double fraction) const {
    if (!m_sample || !m_sample->audio) return 0.0;
    const double total = double(m_sample->audio->frames());
    const double base = m_sample->baseFrames > 0 ? double(m_sample->baseFrames) : total;
    if (total <= 0.0) return 0.0;
    return std::clamp(fraction, 0.0, 1.0) * base / total * double(width());
}

double SamplerWaveform::fractionForX(int x) const {
    if (!m_sample || !m_sample->audio || width() <= 0) return 0.0;
    const double total = double(m_sample->audio->frames());
    const double base = m_sample->baseFrames > 0 ? double(m_sample->baseFrames) : total;
    if (base <= 0.0) return 0.0;
    return std::clamp(double(x) / double(width()) * total / base, 0.0, 1.0);
}

void SamplerWaveform::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();

    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QLinearGradient glass(frame.topLeft(), frame.bottomLeft());
    glass.setColorAt(0.0, mixColors(t.surfaceElevated, t.well(), 0.26));
    glass.setColorAt(0.56, mixColors(t.well(), t.background, 0.18));
    glass.setColorAt(1.0, mixColors(t.well(), t.background, 0.34));
    p.setPen(QPen(mixColors(t.separator(), t.textSecondary, 0.14), 1.0));
    p.setBrush(glass);
    p.drawRoundedRect(frame, 7.0, 7.0);

    QPainterPath clipping;
    clipping.addRoundedRect(frame.adjusted(1.0, 1.0, -1.0, -1.0), 6.0, 6.0);
    p.save();
    p.setClipPath(clipping);

    if (m_minima.isEmpty()) {
        p.setPen(t.textSecondary);
        p.drawText(rect(), Qt::AlignCenter, tr("Drop a sample, or click LOAD"));
        p.restore();
        return;
    }

    const double middle = height() / 2.0;
    const double scale = height() / 2.0 - 9.0;

    // A quiet editor grid gives the new filled waveform a precise time/level
    // frame without competing with the audio. It derives entirely from theme
    // tokens, so it remains readable in both supplied themes.
    QColor grid = mixColors(t.separator(), t.textSecondary, 0.10);
    grid.setAlphaF(t.dark ? 0.36 : 0.24);
    p.setPen(QPen(grid, 1.0));
    for (int division = 1; division < 8; ++division) {
        const double x = double(width()) * division / 8.0;
        p.drawLine(QPointF(x, 0.0), QPointF(x, double(height())));
    }
    for (int division = 1; division < 4; ++division) {
        const double y = double(height()) * division / 4.0;
        p.drawLine(QPointF(0.0, y), QPointF(double(width()), y));
    }

    // The tail the precomputed reverb added is drawn dimmer: it is real audio
    // and it plays, but it is past everything the markers can address.
    const double baseEnd = xForFraction(1.0);
    if (baseEnd < width() - 1) {
        QColor tail = mixColors(t.well(), t.background, 0.52);
        tail.setAlphaF(0.82);
        p.fillRect(QRectF(baseEnd, 0, width() - baseEnd, height()), tail);
    }

    QColor centreLine = mixColors(t.waveform, t.background, 0.60);
    centreLine.setAlphaF(0.72);
    p.setPen(QPen(centreLine, 1.0));
    p.drawLine(QPointF(0.0, middle), QPointF(double(width()), middle));

    // Buckets → pixels. A column covering several buckets takes their extremes;
    // one covering less than a bucket repeats it, which is what a sample too
    // short to fill the strip should look like.
    const int columns = std::max(1, width());
    const double perColumn = double(m_minima.size()) / double(columns);
    QVector<QPointF> highs(columns);
    QVector<QPointF> lows(columns);
    for (int x = 0; x < columns; ++x) {
        const int buckets = int(m_minima.size());
        const int from = std::min(int(double(x) * perColumn), buckets - 1);
        const int to = std::clamp(int(double(x + 1) * perColumn), from + 1, buckets);
        float low = 0.0f;
        float high = 0.0f;
        for (int b = from; b < to; ++b) {
            low = std::min(low, m_minima[b]);
            high = std::max(high, m_maxima[b]);
        }
        highs[x] = QPointF(x, middle - double(high) * scale);
        lows[x] = QPointF(x, middle - double(low) * scale);
    }

    QPainterPath upper;
    QPainterPath lower;
    upper.addPolygon(QPolygonF(highs));
    lower.addPolygon(QPolygonF(lows));

    QPainterPath body = upper;
    for (int x = columns - 1; x >= 0; --x) body.lineTo(lows[x]);
    body.closeSubpath();

    QLinearGradient signal(0.0, 5.0, 0.0, double(height()) - 5.0);
    QColor edge = mixColors(t.waveform, t.accent, 0.24);
    edge.setAlphaF(0.50);
    QColor core = mixColors(t.waveform, t.accentHighlight, 0.50);
    core.setAlphaF(0.94);
    signal.setColorAt(0.0, edge);
    signal.setColorAt(0.48, core);
    signal.setColorAt(0.52, core);
    signal.setColorAt(1.0, edge);
    p.setPen(Qt::NoPen);
    p.setBrush(signal);
    p.drawPath(body);

    QColor signalGlow = t.accent;
    signalGlow.setAlphaF(0.18);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(signalGlow, 4.0, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.drawPath(upper);
    p.drawPath(lower);
    QColor signalEdge = mixColors(t.waveform, t.accentHighlight, 0.34);
    signalEdge.setAlphaF(0.86);
    p.setPen(QPen(signalEdge, 1.0, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    p.drawPath(upper);
    p.drawPath(lower);

    // ── Fades, drawn as the ramps they apply ──
    const double start = xForFraction(m_startOffset);
    const double end = xForFraction(m_endOffset);
    QColor shade = t.background;
    shade.setAlpha(t.dark ? 176 : 118);
    if (start > 0.0) p.fillRect(QRectF(0, 0, start, height()), shade);
    if (end < baseEnd) p.fillRect(QRectF(end, 0, baseEnd - end, height()), shade);

    QColor fadeInk = t.accentHighlight;
    fadeInk.setAlphaF(0.18);
    QColor fadeEdge = t.accentHighlight;
    fadeEdge.setAlphaF(0.72);
    if (m_fadeIn > 0.0) {
        const double to = start + (end - start) * m_fadeIn;
        QPainterPath path;
        path.moveTo(start, height());
        path.lineTo(to, 0);
        path.lineTo(start, 0);
        path.closeSubpath();
        p.fillPath(path, fadeInk);
        p.setPen(QPen(fadeEdge, 1.2));
        p.drawLine(QPointF(start, height()), QPointF(to, 0));
    }
    if (m_fadeOut > 0.0) {
        const double from = end - (end - start) * m_fadeOut;
        QPainterPath path;
        path.moveTo(from, 0);
        path.lineTo(end, height());
        path.lineTo(end, 0);
        path.closeSubpath();
        p.fillPath(path, fadeInk);
        p.setPen(QPen(fadeEdge, 1.2));
        p.drawLine(QPointF(from, 0), QPointF(end, height()));
    }

    // ── Markers ──
    const auto drawMarker = [&](double x, const QColor& colour, const QString& glyph) {
        const double markerX = std::clamp(x, 0.5, double(width()) - 0.5);
        QColor line = colour;
        line.setAlphaF(0.90);
        p.setPen(QPen(line, 1.25));
        p.drawLine(QPointF(markerX, 0), QPointF(markerX, height()));
        QFont font = p.font();
        font.setPixelSize(8);
        font.setBold(true);
        p.setFont(font);
        constexpr double chipWidth = 17.0;
        constexpr double chipHeight = 13.0;
        const double chipX = markerX + chipWidth + 3.0 <= width()
                                 ? markerX + 2.0
                                 : markerX - chipWidth - 2.0;
        QColor chip = colour;
        chip.setAlphaF(0.94);
        p.setPen(QPen(mixColors(colour, t.textPrimary, 0.18), 0.8));
        p.setBrush(chip);
        p.drawRoundedRect(QRectF(chipX, 3.0, chipWidth, chipHeight), 3.0, 3.0);
        p.setPen(t.background);
        p.drawText(QRectF(chipX, 3.0, chipWidth, chipHeight), Qt::AlignCenter, glyph);
    };

    if (m_loopMode != 0) {
        const double from = xForFraction(m_loopStart);
        const double to = xForFraction(m_loopEnd);
        QColor loopInk = Theme::solo();
        loopInk.setAlphaF(0.12);
        p.fillRect(QRectF(from, 0, to - from, height()), loopInk);
        drawMarker(from, Theme::solo(), QStringLiteral("L"));
        drawMarker(to, Theme::solo(), QStringLiteral("R"));
    }
    drawMarker(start, t.cursor, QStringLiteral("S"));
    drawMarker(end, t.accent, QStringLiteral("E"));
    p.restore();
}

QString SamplerWaveform::markerAt(int x) const {
    struct Candidate {
        QString id;
        double x;
    };
    QVector<Candidate> candidates{
        {QStringLiteral("startoffset"), xForFraction(m_startOffset)},
        {QStringLiteral("endoffset"), xForFraction(m_endOffset)}};
    if (m_loopMode != 0) {
        candidates.push_back({QStringLiteral("loop.start"), xForFraction(m_loopStart)});
        candidates.push_back({QStringLiteral("loop.end"), xForFraction(m_loopEnd)});
    }
    QString best;
    double bestDistance = 7.0;
    for (const Candidate& candidate : candidates) {
        const double distance = std::abs(candidate.x - double(x));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate.id;
        }
    }
    return best;
}

void SamplerWaveform::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    m_dragging = markerAt(int(event->position().x()));
    // A click in open water moves the start offset — the marker one reaches
    // for most, and the one a sampler is normally opened to set.
    if (m_dragging.isEmpty()) m_dragging = QStringLiteral("startoffset");
    emit markerMoved(m_dragging, fractionForX(int(event->position().x())));
}

void SamplerWaveform::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging.isEmpty()) {
        setCursor(markerAt(int(event->position().x())).isEmpty() ? Qt::PointingHandCursor
                                                                 : Qt::SizeHorCursor);
        return;
    }
    emit markerMoved(m_dragging, fractionForX(int(event->position().x())));
}

void SamplerWaveform::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragging.isEmpty()) return;
    emit markerReleased(m_dragging);
    m_dragging.clear();
}

class SamplerEnvelopeView : public QWidget {
public:
    explicit SamplerEnvelopeView(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedHeight(112);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }

    void setValue(const QString& id, double value) {
        if (m_dragId == id) return;
        m_values[id] = value;
        update();
    }

    std::function<void(const QString&)> beginEdit;
    std::function<void(const QString&, double)> changeValue;
    std::function<void(const QString&)> endEdit;

protected:
    struct Handle {
        enum Kind { Time, Level, Tension } kind = Time;
        QString id;
        QPointF point;
    };

    QVector<Handle> handles() const {
        const QRectF r = rect().adjusted(16, 12, -16, -16);
        const auto val = [this](const char* id, double fallback) {
            return m_values.value(QString::fromLatin1(id), fallback);
        };
        const auto weight = [](double seconds) {
            return std::log1p(std::clamp(seconds, 0.0, 10.0)) / std::log(11.0);
        };
        const double d = weight(val("amp.delay", 0.0));
        const double a = weight(val("amp.att", 0.01));
        const double h = weight(val("amp.hold", 0.0));
        const double dec = weight(val("amp.dec", 0.1));
        const double rel = weight(val("amp.rel", 0.2));
        const double sum = std::max(0.18, d + a + h + dec + rel);
        const double timed = r.width() * 0.84;
        double x = r.left();
        auto advance = [&](double w) {
            x += timed * (0.04 + w) / (0.20 + sum);
            return x;
        };
        const double xd = advance(d);
        const double xa = advance(a);
        const double xh = advance(h);
        const double xdec = advance(dec);
        const double sustainValue = std::clamp(val("amp.sus", 1.0), 0.0, 1.0);
        const double y0 = r.bottom();
        const double y1 = r.top();
        const double ys = y0 - sustainValue * r.height();
        const double xr0 = std::min(r.right() - 24.0, xdec + r.width() * 0.10);
        const double xr = r.right();
        return {
            {Handle::Time, QStringLiteral("amp.delay"), {xd, y0}},
            {Handle::Time, QStringLiteral("amp.att"), {xa, y1}},
            {Handle::Time, QStringLiteral("amp.hold"), {xh, y1}},
            {Handle::Time, QStringLiteral("amp.dec"), {xdec, ys}},
            {Handle::Level, QStringLiteral("amp.sus"), {xr0, ys}},
            {Handle::Time, QStringLiteral("amp.rel"), {xr, y0}},
            {Handle::Tension, QStringLiteral("amp.atttens"),
             {(xd + xa) * 0.5, (y0 + y1) * 0.5}},
            {Handle::Tension, QStringLiteral("amp.dectens"),
             {(xh + xdec) * 0.5, (y1 + ys) * 0.5}},
            {Handle::Tension, QStringLiteral("amp.reltens"),
             {(xr0 + xr) * 0.5, (ys + y0) * 0.5}},
        };
    }

    Handle nearest(const QPointF& point) const {
        Handle best;
        double distance = 13.0;
        for (const Handle& h : handles()) {
            const double d = QLineF(point, h.point).length();
            if (d < distance) { distance = d; best = h; }
        }
        return best;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        p.fillRect(rect(), mixColors(t.well(), t.background, 0.18));
        const QRectF r = rect().adjusted(16, 12, -16, -16);
        QColor grid = t.separator(); grid.setAlpha(70);
        p.setPen(QPen(grid, 1.0));
        for (int i = 1; i < 4; ++i) {
            const double y = r.top() + r.height() * i / 4.0;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        const QVector<Handle> hs = handles();
        if (hs.size() < 9) return;
        const QPointF origin(r.left(), r.bottom());
        const QPointF delay = hs[0].point;
        const QPointF attack = hs[1].point;
        const QPointF hold = hs[2].point;
        const QPointF decay = hs[3].point;
        const QPointF sustain = hs[4].point;
        const QPointF end = hs[5].point;
        QPainterPath path;
        path.moveTo(origin);
        path.lineTo(delay);
        auto curve = [&](QPointF from, QPointF to, double tension) {
            for (int i = 1; i <= 32; ++i) {
                const double u = double(i) / 32.0;
                const double shaped = sampler::applyTension(u, tension);
                path.lineTo(from.x() + (to.x() - from.x()) * u,
                            from.y() + (to.y() - from.y()) * shaped);
            }
        };
        curve(delay, attack, m_values.value(QStringLiteral("amp.atttens"), 0.0));
        path.lineTo(hold);
        curve(hold, decay, m_values.value(QStringLiteral("amp.dectens"), 0.0));
        path.lineTo(sustain);
        curve(sustain, end, m_values.value(QStringLiteral("amp.reltens"), 0.0));
        QPainterPath fill = path;
        fill.lineTo(end.x(), r.bottom());
        fill.closeSubpath();
        QColor wash = t.accent; wash.setAlpha(28);
        p.fillPath(fill, wash);
        QColor glow = t.accent; glow.setAlpha(45);
        p.setPen(QPen(glow, 6.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.setPen(QPen(t.accent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        for (const Handle& h : hs) {
            const bool tension = h.kind == Handle::Tension;
            p.setPen(QPen(tension ? t.textSecondary : t.accentHighlight, 1.0));
            p.setBrush(tension ? t.surfaceElevated : t.accent);
            if (tension) p.drawRect(QRectF(h.point.x() - 3, h.point.y() - 3, 6, 6));
            else p.drawEllipse(h.point, 4.0, 4.0);
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const Handle h = nearest(e->position());
        if (h.id.isEmpty()) return;
        m_dragId = h.id; m_dragKind = h.kind;
        m_dragOrigin = e->position(); m_dragValue = m_values.value(m_dragId);
        if (beginEdit) beginEdit(m_dragId);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_dragId.isEmpty()) {
            const Handle h = nearest(e->position());
            setCursor(h.id.isEmpty() ? Qt::ArrowCursor
                      : h.kind == Handle::Time ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            return;
        }
        const double fine = e->modifiers() & Qt::ShiftModifier ? 0.2 : 1.0;
        double value = m_dragValue;
        if (m_dragKind == Handle::Time) {
            const double initial = std::log1p(std::clamp(m_dragValue, 0.0, 10.0)) /
                                   std::log(11.0);
            const double normalized = std::clamp(initial +
                (e->position().x() - m_dragOrigin.x()) / std::max(1, width()) * fine,
                0.0, 1.0);
            value = std::exp(normalized * std::log(11.0)) - 1.0;
        } else {
            const double span = m_dragKind == Handle::Tension ? 2.0 : 1.0;
            value = m_dragValue - (e->position().y() - m_dragOrigin.y()) /
                                      std::max(1, height()) * span * fine;
            value = m_dragKind == Handle::Tension ? std::clamp(value, -1.0, 1.0)
                                                   : std::clamp(value, 0.0, 1.0);
        }
        m_values[m_dragId] = value;
        if (changeValue) changeValue(m_dragId, value);
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        if (m_dragId.isEmpty()) return;
        const QString id = m_dragId; m_dragId.clear();
        if (endEdit) endEdit(id);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const Handle h = nearest(e->position());
        if (h.id.isEmpty()) return;
        if (const auto* info = infoFor(h.id)) {
            if (beginEdit) beginEdit(h.id);
            m_values[h.id] = info->defaultValue;
            if (changeValue) changeValue(h.id, info->defaultValue);
            if (endEdit) endEdit(h.id);
            update();
        }
    }
private:
    QHash<QString, double> m_values;
    QString m_dragId;
    Handle::Kind m_dragKind = Handle::Time;
    QPointF m_dragOrigin;
    double m_dragValue = 0.0;
};

class SamplerKeyboard : public QWidget {
public:
    explicit SamplerKeyboard(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(75 * kWhiteWidth, 68);
        setMouseTracking(true);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }
    void setRoot(int pitch) { m_root = std::clamp(pitch, 0, 127); update(); }
    void stopAudition() { releasePressed(); }
    int xForPitch(int pitch) const { return whiteX(std::clamp(pitch, 0, 127)); }
    std::function<void(int)> noteOn;
    std::function<void(int)> noteOff;
    std::function<void(int)> rootChanged;

protected:
    static bool black(int pitch) {
        const int pc = pitch % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }
    static int whiteX(int pitch) {
        int whites = 0;
        for (int p = 0; p < pitch; ++p) if (!black(p)) ++whites;
        return whites * kWhiteWidth;
    }
    int pitchAt(const QPointF& point) const {
        if (point.y() < 52) {
            for (int pitch = 0; pitch < 128; ++pitch) {
                if (!black(pitch)) continue;
                if (QRectF(whiteX(pitch) - kBlackWidth / 2.0, 0,
                           kBlackWidth, 52).contains(point)) return pitch;
            }
        }
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (!black(pitch) &&
                QRectF(whiteX(pitch), 0, kWhiteWidth, height()).contains(point))
                return pitch;
        }
        return -1;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme& t = th();
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (black(pitch)) continue;
            const QRectF key(whiteX(pitch), 0, kWhiteWidth, height());
            QColor body = mixColors(t.textPrimary, t.surface, t.dark ? 0.12 : 0.04);
            if (pitch == m_root) body = mixColors(body, t.accent, 0.48);
            if (pitch == m_pressed) body = mixColors(body, t.accentHighlight, 0.58);
            p.setPen(QPen(t.separator(), 1.0)); p.setBrush(body); p.drawRect(key);
            if (pitch % 12 == 0) {
                QFont f = p.font(); f.setPixelSize(8); p.setFont(f);
                p.setPen(t.background);
                p.drawText(key.adjusted(1, 0, -1, -3),
                           Qt::AlignBottom | Qt::AlignHCenter,
                           QStringLiteral("C%1").arg(pitch / 12));
            }
        }
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (!black(pitch)) continue;
            const QRectF key(whiteX(pitch) - kBlackWidth / 2.0, 0,
                             kBlackWidth, 52);
            QColor body = mixColors(t.background, QColor(0, 0, 0), 0.35);
            if (pitch == m_root) body = mixColors(body, t.accent, 0.70);
            if (pitch == m_pressed) body = t.accentHighlight;
            p.setPen(QPen(t.separator(), 1.0)); p.setBrush(body); p.drawRect(key);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int pitch = pitchAt(e->position());
        if (pitch < 0) return;
        if (e->button() == Qt::RightButton) {
            m_root = pitch; update(); if (rootChanged) rootChanged(pitch); return;
        }
        if (e->button() != Qt::LeftButton) return;
        releasePressed(); m_pressed = pitch; update(); if (noteOn) noteOn(pitch);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        const int pitch = pitchAt(e->position());
        if (pitch == m_pressed) return;
        releasePressed();
        if (pitch >= 0) { m_pressed = pitch; if (noteOn) noteOn(pitch); update(); }
    }
    void mouseReleaseEvent(QMouseEvent*) override { releasePressed(); }
    void leaveEvent(QEvent*) override { releasePressed(); }
private:
    void releasePressed() {
        if (m_pressed < 0) return;
        const int pitch = m_pressed; m_pressed = -1;
        if (noteOff) noteOff(pitch); update();
    }
    static constexpr int kWhiteWidth = 18;
    static constexpr int kBlackWidth = 11;
    int m_root = 60;
    int m_pressed = -1;
};

// ── Panel ──────────────────────────────────────────────────────────────────

SamplerPanel::SamplerPanel(daw::EngineController* controller, QString channelId,
                           QString slotId, QWidget* parent)
    : SamplerPanel(controller, Context::Instrument, std::move(channelId),
                   std::move(slotId), parent) {}

SamplerPanel::SamplerPanel(daw::EngineController* controller, Context context,
                           QString ownerId, QString objectId, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channelId(std::move(ownerId)),
      m_slotId(std::move(objectId)), m_context(context) {
    setAcceptDrops(true);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Both contexts deliberately use the same shell.  A timeline clip is not
    // a reduced secondary dialog: it is the same Sample Editor with state
    // routed to a ClipModel instead of the built-in sampler instance.
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->addWidget(buildFxStrip());
    splitter->addWidget(buildSamplerBody());
    splitter->setSizes({118, 1002});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    outer->addWidget(splitter);

    m_poll = new QTimer(this);
    m_poll->setInterval(kPollMs);
    connect(m_poll, &QTimer::timeout, this, &SamplerPanel::refresh);
    m_poll->start();

    m_fileLabel->installEventFilter(this);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this,
            &SamplerPanel::applyTheme);
    applyTheme();
    refresh();
}

void SamplerPanel::setSnapProvider(std::function<double()> provider) {
    m_snapProvider = std::move(provider);
    // The Time knob is the one control whose value has a position on the
    // timeline attached to it, so it is the one that gets a grid detent.
    if (ui::Knob* time = m_knobs.value(QStringLiteral("stretch.time"), nullptr)) {
        if (m_context != Context::Clip || !m_snapProvider) {
            time->setDetent({});
            return;
        }
        time->setDetent([this](double wanted) {
            if (!m_controller || !m_snapProvider) return wanted;
            return m_controller->snappedStretchTime(
                m_channelId.toStdString(), m_slotId.toStdString(), wanted,
                m_snapProvider());
        });
    }
}

void SamplerPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_poll && !m_poll->isActive()) m_poll->start();
    refresh();
}

void SamplerPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (m_poll) m_poll->stop();
}

SamplerPanel::~SamplerPanel() {
    if (m_keyboard) m_keyboard->stopAudition();
    if (m_controller && m_context == Context::Clip) m_controller->stopPreview();
}

void SamplerPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (!event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile() && ui::isAudioFile(url.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void SamplerPanel::dropEvent(QDropEvent* event) {
    if (!m_controller || !event->mimeData()->hasUrls()) return;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || !ui::isAudioFile(url.toLocalFile())) continue;
        const bool loaded = m_context == Context::Instrument
            ? m_controller->loadSamplerSample(m_channelId.toStdString(),
                                              m_slotId.toStdString(),
                                              url.toLocalFile().toStdString())
            : m_controller->setClipAudioFile(m_channelId.toStdString(),
                                             m_slotId.toStdString(),
                                             url.toLocalFile().toStdString());
        if (!loaded) continue;
        event->acceptProposedAction();
        emit projectEdited();
        refresh();
        return;
    }
}

bool SamplerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_fileLabel && event->type() == QEvent::MouseButtonRelease) {
        revealSample();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SamplerPanel::applyTheme() {
    const Theme& t = th();
#ifdef Q_OS_MACOS
    QString fixedFamily = QStringLiteral("Menlo");
#else
    QString fixedFamily =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
#endif
    fixedFamily.replace('"', QStringLiteral("\\\""));
    setStyleSheet(QString(R"(
#SamplerSection { background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 8px; }
#SamplerBody { background: %BG%; }
#SamplerFxStrip { background: %FXSURFACE%; border-right: 1px solid %BORDER%; }
#SamplerAccentBar { background: %ACCENT%; border-radius: 2px; }
#SamplerStripName { color: %TEXT%; font-size: 10px; font-weight: 700; }
#SamplerSlotWell { background: %WELL%; border: 1px solid %BORDER%; border-radius: 5px; }
#SamplerMixerSlot { background: %SLOT%; border: 1px solid %BORDER%; border-radius: 3px;
    color: %TEXT2%; font-size: 8px; font-weight: 600; padding: 0 4px;
    text-align: left; }
#SamplerMixerSlot[active="true"] { color: %TEXT%; }
#SamplerMixerSlot[bypassed="true"] { color: %DIM%; border-color: %BYPASS%; }
#SamplerMixerSlot:hover { background: %HOVER%; }
#SamplerMixerSlot::menu-indicator { image: none; width: 0; }
#SamplerFxRouting { background: %WELL%; border: 1px solid %BORDER%; border-radius: 5px; }
#SamplerFxReadout { color: %TEXT2%; font-size: 8px; font-family: "%MONO%"; }
#SamplerNamePlate { color: %TEXT%; background: %NAMEPLATE%; border-radius: 4px;
    font-size: 10px; font-weight: 700; }
#SamplerStretchBlock { background: %WELL%; border: 1px solid %BORDER%; border-radius: 6px; }
#SamplerCollapse { color: %TEXT%; background: transparent; border: none;
    text-align: left; padding: 2px 0; font-size: 10px; font-weight: 700; }
#SamplerCollapse:hover { color: %ACCENT%; }
QTabBar#SamplerToolsTabs::tab { color: %TEXT2%; background: transparent;
    border: none; border-bottom: 2px solid transparent; padding: 4px 12px;
    font-size: 9px; font-weight: 700; }
QTabBar#SamplerToolsTabs::tab:selected { color: %TEXT%; border-bottom-color: %ACCENT%; }
QTabBar#SamplerToolsTabs::tab:hover { color: %ACCENT%; }
#SamplerCaption { color: %TEXT2%; font-size: 10px; }
#SamplerFile { color: %TEXT%; font-size: 12px; font-weight: 600; }
#SamplerButton { color: %TEXT%; background: %WELL%; border: none; border-radius: 4px;
                 padding: 4px 10px; font-size: 11px; }
#SamplerButton:hover { background: %ACCENT%; color: %BG%; }
QComboBox { color: %TEXT%; background: %WELL%; border: none; border-radius: 4px;
            padding: 3px 8px; font-size: 11px; }
QComboBox QAbstractItemView { background: %SURFACE%; color: %TEXT%;
                              selection-background-color: %ACCENT%; }
)")
            .replace("%MONO%", fixedFamily)
            .replace("%SURFACE%", t.surface.name())
            .replace("%WELL%", t.well().name())
            .replace("%TEXT%", t.textPrimary.name())
            .replace("%TEXT2%", t.textSecondary.name())
            .replace("%ACCENT%", t.accent.name())
            .replace("%BORDER%", t.separator().name())
            .replace("%SLOT%", mixColors(t.surfaceElevated, t.background, 0.2).name())
            .replace("%HOVER%", mixColors(t.surfaceElevated, t.textPrimary, 0.12).name())
            .replace("%DIM%", mixColors(t.textSecondary, t.background, 0.35).name())
            .replace("%BYPASS%", mixColors(Theme::mute(), t.background, 0.45).name())
            .replace("%NAMEPLATE%", mixColors(t.surface, t.accent, 0.17).name())
            .replace("%FXSURFACE%", mixColors(t.surface, t.background, 0.18).name())
            .replace("%BG%", t.background.name()));
}

sampler::SamplerInstance* SamplerPanel::sampler() const {
    if (!m_controller || m_context != Context::Instrument) return nullptr;
    return m_controller->samplerInstance(m_channelId.toStdString(),
                                         m_slotId.toStdString());
}

std::shared_ptr<const sampler::SampleData> SamplerPanel::currentSample() {
    if (!m_controller) return {};
    if (m_context == Context::Clip) {
        return m_controller->clipSampleData(m_channelId.toStdString(),
                                            m_slotId.toStdString());
    }
    if (sampler::SamplerInstance* instance = sampler()) return instance->sample();
    return {};
}

// ── Parameter binding ──

double SamplerPanel::readParameter(const QString& parameterId) {
    if (!m_controller) return 0.0;
    if (m_context == Context::Clip) {
        return m_controller->clipSampleParameter(
            m_channelId.toStdString(), m_slotId.toStdString(),
            parameterId.toStdString());
    }
    return m_controller->insertParameter(m_channelId.toStdString(),
                                         m_slotId.toStdString(),
                                         parameterId.toStdString());
}

void SamplerPanel::writeParameter(const QString& parameterId, double value) {
    if (!m_controller) return;
    if (m_context == Context::Clip) {
        m_controller->setClipSampleParameter(
            m_channelId.toStdString(), m_slotId.toStdString(),
            parameterId.toStdString(), value);
        emit liveEdited();
        return;
    }
    if (parameterId == QStringLiteral("startoffset")) {
        value = std::min(value, readParameter(QStringLiteral("endoffset")) - 0.0001);
    } else if (parameterId == QStringLiteral("endoffset")) {
        value = std::max(value, readParameter(QStringLiteral("startoffset")) + 0.0001);
    }
    m_controller->setInsertParameter(m_channelId.toStdString(), m_slotId.toStdString(),
                                     parameterId.toStdString(), value);
    emit liveEdited();
}

void SamplerPanel::beginGesture(const QString& parameterId) {
    // Read *before* the first write of the gesture: that is the value undo has
    // to come back to, and the instance still holds it at this point.
    if (!m_gestureStart.contains(parameterId)) {
        m_gestureStart.insert(parameterId, readParameter(parameterId));
    }
}

void SamplerPanel::endGesture(const QString& parameterId) {
    if (!m_gestureStart.contains(parameterId) || !m_controller) return;
    if (m_context == Context::Clip) {
        m_controller->commitClipSampleParameterEdit(
            m_channelId.toStdString(), m_slotId.toStdString(),
            parameterId.toStdString(), m_gestureStart.take(parameterId),
            "Change Clip Sample Parameter");
        emit projectEdited();
        return;
    }
    m_controller->commitInsertParameterEdit(
        m_channelId.toStdString(), m_slotId.toStdString(), parameterId.toStdString(),
        m_gestureStart.take(parameterId), "Change Sampler Parameter");
    emit projectEdited();
}

ui::Knob* SamplerPanel::knob(const QString& parameterId, const QString& captionText,
                             bool compact) {
    auto* control = new ui::Knob(captionText, this);
    const daw::plugins::ParameterInfo* info = infoFor(parameterId);
    if (info) {
        control->setRange(info->minValue, info->maxValue);
        control->setDefaultValue(info->defaultValue);
        control->setStepped(info->isStepped);
        // Bipolar is a property of the range, not of the knob: anything that
        // spans zero symmetrically reads better drawn from the middle.
        control->setBipolar(info->minValue < 0.0 && info->maxValue > 0.0);
        const std::uint32_t index = info->index;
        control->setFormatter([index](double value) {
            return QString::fromStdString(sampler::parameterText(index, value));
        });
        control->setValue(readParameter(parameterId));
        control->setToolTip(QString::fromStdString(info->name));
    }
    if (compact) control->setCompact(true);
    control->setVisualStyle(ui::Knob::VisualStyle::SamplerDigital);

    connect(control, &ui::Knob::valueChanged, this, [this, parameterId](double value) {
        beginGesture(parameterId);
        writeParameter(parameterId, value);
    });
    connect(control, &ui::Knob::editFinished, this,
            [this, parameterId] { endGesture(parameterId); });
    if (m_context == Context::Instrument) {
        control->setAutomatable(true);
        connect(control, &ui::Knob::automateRequested, this,
                [this, parameterId] { emit automationRequested(parameterId); });
    }

    m_knobs.insert(parameterId, control);
    return control;
}

ui::Led* SamplerPanel::led(const QString& parameterId, const QString& captionText) {
    auto* lamp = new ui::Led(captionText, this);
    lamp->setChecked(readParameter(parameterId) >= 0.5);
    connect(lamp, &ui::Led::toggled, this, [this, parameterId](bool on) {
        // A lamp is one gesture in itself, so it opens and closes the undo
        // entry in the same click.
        beginGesture(parameterId);
        writeParameter(parameterId, on ? 1.0 : 0.0);
        endGesture(parameterId);
        refresh();
    });
    m_leds.insert(parameterId, lamp);
    return lamp;
}

QComboBox* SamplerPanel::combo(const QString& parameterId, const QStringList& items) {
    auto* box = new QComboBox(this);
    box->addItems(items);
    box->setCurrentIndex(int(std::lround(readParameter(parameterId))));
    connect(box, &QComboBox::currentIndexChanged, this, [this, parameterId](int index) {
        beginGesture(parameterId);
        writeParameter(parameterId, double(index));
        endGesture(parameterId);
        refresh();
    });
    m_combos.insert(parameterId, box);
    return box;
}

QWidget* SamplerPanel::buildFxStrip() {
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("SamplerFxStrip"));
    strip->setMinimumWidth(112);
    strip->setMaximumWidth(128);
    auto* column = new QVBoxLayout(strip);
    column->setContentsMargins(5, 5, 5, 5);
    column->setSpacing(4);

    auto* stripHeader = new QHBoxLayout;
    stripHeader->setContentsMargins(0, 0, 0, 0);
    stripHeader->setSpacing(3);
    auto* swatch = new QWidget(strip);
    swatch->setObjectName(QStringLiteral("SamplerAccentBar"));
    swatch->setFixedSize(2, 12);
    auto* stripName = new QLabel(
        m_context == Context::Instrument ? tr("SAMPLER FX") : tr("CLIP FX"), strip);
    stripName->setObjectName(QStringLiteral("SamplerStripName"));
    stripHeader->addWidget(swatch);
    stripHeader->addWidget(stripName, 1);
    m_fxBypass = new ui::IconButton(
        icons::Glyph::Power,
        m_context == Context::Instrument ? tr("Bypass all Sampler effects")
                                         : tr("Bypass all Clip effects"), strip);
    m_fxBypass->setButtonSize(14, 14);
    m_fxBypass->setCheckable(true);
    m_fxBypass->setActiveColor(Theme::mute());
    stripHeader->addWidget(m_fxBypass);
    auto* addFx = new ui::IconButton(icons::Glyph::Plus, tr("Add effect"), strip);
    addFx->setButtonSize(14, 14);
    stripHeader->addWidget(addFx);
    column->addLayout(stripHeader);
    connect(addFx, &QAbstractButton::clicked, this, [this] {
        const std::vector<daw::InsertModel>* inserts = nullptr;
        if (m_controller) {
            if (m_context == Context::Instrument) {
                if (const auto* fx = m_controller->samplerFx(
                        m_channelId.toStdString(), m_slotId.toStdString()))
                    inserts = &fx->inserts;
            } else {
                inserts = m_controller->clipFx(m_channelId.toStdString(),
                                               m_slotId.toStdString());
            }
        }
        const int index = inserts ? int(inserts->size()) : 0;
        if (index < int(daw::EngineController::kSamplerFxSlots)) showFxMenu(index);
    });
    connect(m_fxBypass, &QAbstractButton::toggled, this, [this](bool bypassed) {
        if (!m_controller) return;
        if (m_context == Context::Instrument) {
            m_controller->setAllSamplerFxBypassed(m_channelId.toStdString(),
                                                  m_slotId.toStdString(), bypassed);
        } else {
            m_controller->setAllClipFxBypassed(m_channelId.toStdString(),
                                               m_slotId.toStdString(), bypassed);
        }
        m_fxSignature.clear(); emit projectEdited(); refresh();
    });

    m_fxSlotsHost = new QWidget(strip);
    m_fxSlotsHost->setObjectName(QStringLiteral("SamplerSlotWell"));
    m_fxSlotsLayout = new QVBoxLayout(m_fxSlotsHost);
    m_fxSlotsLayout->setContentsMargins(2, 2, 2, 2);
    m_fxSlotsLayout->setSpacing(1);
    column->addWidget(m_fxSlotsHost);

    auto* routing = new QWidget(strip);
    routing->setObjectName(QStringLiteral("SamplerFxRouting"));
    routing->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* routingLayout = new QGridLayout(routing);
    routingLayout->setContentsMargins(3, 3, 3, 3);
    routingLayout->setHorizontalSpacing(2);
    routingLayout->setVerticalSpacing(3);
    auto* panCaption = caption(tr("Pan"), routing);
    panCaption->setAlignment(Qt::AlignCenter);
    m_fxPan = new ui::PanKnob(routing);
    m_fxPan->setFixedSize(56, 56);
    m_fxPanLabel = new QLabel(QStringLiteral("C"), routing);
    m_fxPanLabel->setObjectName(QStringLiteral("SamplerFxReadout"));
    m_fxPanLabel->setAlignment(Qt::AlignCenter);
    m_fxVolume = new ui::FaderWidget(Qt::Vertical, routing);
    m_fxVolume->setMinimumHeight(80);
    m_fxVolume->setFixedWidth(22);
    m_fxVolume->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_fxMeter = new ui::LevelMeter(Qt::Vertical, 2, routing);
    m_fxMeter->setMinimumHeight(80);
    m_fxMeter->setFixedWidth(14);
    m_fxMeter->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // Three columns keep the control axis at the *geometric* centre of the
    // strip: a meter-width spacer on the left mirrors the real meter on the
    // right. Pan and Volume therefore share the exact centre line even though
    // only the right satellite paints anything.
    routingLayout->setColumnMinimumWidth(0, 14);
    routingLayout->setColumnMinimumWidth(2, 14);
    routingLayout->addWidget(panCaption, 0, 1);
    routingLayout->addWidget(m_fxPan, 1, 1, Qt::AlignHCenter);
    routingLayout->addWidget(m_fxPanLabel, 2, 1);
    // Pan is one visual group (caption, dial, readout); the empty rhythm row
    // below it prevents the long Volume rail from looking attached to the
    // dial. Sixteen pixels follows the strip's 4/8px spacing scale while still
    // leaving plenty of rail at the minimum editor height.
    routingLayout->setRowMinimumHeight(3, 16);
    routingLayout->addWidget(m_fxVolume, 4, 1, Qt::AlignHCenter);
    routingLayout->addWidget(m_fxMeter, 4, 2, Qt::AlignHCenter);
    m_fxGainLabel = new QLabel(ui::formatGainDb(1.0), routing);
    m_fxGainLabel->setObjectName(QStringLiteral("SamplerFxReadout"));
    m_fxGainLabel->setAlignment(Qt::AlignCenter);
    routingLayout->addWidget(m_fxGainLabel, 5, 1);
    routingLayout->setRowStretch(4, 1);
    // The console output block begins immediately below the inserts and owns
    // all remaining height. Its fader rail and meter therefore terminate at
    // the bottom readout instead of floating halfway down an empty sidebar.
    column->addWidget(routing, 1);

    const auto startGesture = [this] {
        if (m_fxLevelGesture || !m_controller) return;
        if (m_context == Context::Instrument) {
            if (const auto* fx = m_controller->samplerFx(m_channelId.toStdString(),
                                                         m_slotId.toStdString())) {
                m_fxGestureVolume = fx->volume; m_fxGesturePan = fx->pan;
                m_fxLevelGesture = true;
            }
        } else if (const daw::ClipModel* clip = m_controller->audioClip(
                       m_channelId.toStdString(), m_slotId.toStdString())) {
            m_fxGestureVolume = clip->gain;
            m_fxGesturePan = clip->pan;
            m_fxLevelGesture = true;
        }
    };
    connect(m_fxPan, &ui::PanKnob::panChanged, this,
            [this, startGesture](double value) {
                startGesture();
                if (m_context == Context::Instrument)
                    m_controller->setSamplerFxPan(m_channelId.toStdString(),
                                                  m_slotId.toStdString(), float(value));
                else
                    m_controller->setClipFxPan(m_channelId.toStdString(),
                                               m_slotId.toStdString(), float(value));
            });
    connect(m_fxVolume, &ui::FaderWidget::gainChanged, this,
            [this, startGesture](double value) {
                startGesture();
                if (m_context == Context::Instrument)
                    m_controller->setSamplerFxVolume(m_channelId.toStdString(),
                                                     m_slotId.toStdString(), float(value));
                else
                    m_controller->setClipFxVolume(m_channelId.toStdString(),
                                                  m_slotId.toStdString(), float(value));
            });
    const auto finishGesture = [this] {
        if (!m_fxLevelGesture || !m_controller) return;
        if (m_context == Context::Instrument) {
            m_controller->commitSamplerFxLevelEdit(
                m_channelId.toStdString(), m_slotId.toStdString(),
                m_fxGestureVolume, m_fxGesturePan, "Change Sampler FX Level");
        } else {
            m_controller->commitClipFxLevelEdit(
                m_channelId.toStdString(), m_slotId.toStdString(),
                m_fxGestureVolume, m_fxGesturePan, "Change Clip FX Level");
        }
        m_fxLevelGesture = false; emit projectEdited();
    };
    connect(m_fxPan, &ui::PanKnob::editFinished, this, finishGesture);
    connect(m_fxVolume, &ui::FaderWidget::editFinished, this, finishGesture);
    rebuildFxSlots();
    return strip;
}

void SamplerPanel::rebuildFxSlots() {
    if (!m_fxSlotsLayout || !m_controller) return;
    while (QLayoutItem* item = m_fxSlotsLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) widget->deleteLater();
        delete item;
    }
    static const std::vector<daw::InsertModel> empty;
    const std::vector<daw::InsertModel>* model = nullptr;
    if (m_context == Context::Instrument) {
        if (const daw::SamplerFxModel* fx = m_controller->samplerFx(
                m_channelId.toStdString(), m_slotId.toStdString())) {
            model = &fx->inserts;
        }
    } else {
        model = m_controller->clipFx(m_channelId.toStdString(),
                                     m_slotId.toStdString());
    }
    const auto& inserts = model ? *model : empty;
    for (int index = 0; index < int(daw::EngineController::kSamplerFxSlots); ++index) {
        if (index < int(inserts.size())) {
            const daw::InsertModel slot = inserts[std::size_t(index)];
            auto* name = new QToolButton(m_fxSlotsHost);
            name->setObjectName(QStringLiteral("SamplerMixerSlot"));
            name->setProperty("active", true);
            name->setProperty("bypassed", slot.bypassed);
            name->setText(QString::fromStdString(slot.name));
            name->setFixedHeight(kSamplerFxSlotHeight);
            name->setToolButtonStyle(Qt::ToolButtonTextOnly);
            name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            name->setCursor(Qt::PointingHandCursor);
            name->setToolTip(slot.bypassed
                                 ? tr("%1 — bypassed. Click to edit.")
                                       .arg(QString::fromStdString(slot.name))
                                 : tr("%1 — click to edit, right-click for options.")
                                       .arg(QString::fromStdString(slot.name)));
            name->setAccessibleName(
                tr("Insert %1: %2").arg(index + 1).arg(QString::fromStdString(slot.name)));
            name->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(name, &QToolButton::clicked, this, [this, slot] {
                emit pluginEditorRequested(m_channelId, QString::fromStdString(slot.id));
            });
            connect(name, &QWidget::customContextMenuRequested, this,
                    [this, name, slot, index](const QPoint& point) {
                        showFxContext(QString::fromStdString(slot.id), index,
                                      name->mapToGlobal(point));
                    });

            auto* row = new SamplerFxSlotRow(name, m_fxSlotsHost);
            const auto action = [row](icons::Glyph glyph, const QString& tip) {
                auto* button = new ui::IconButton(glyph, tip, row);
                button->setButtonSize(kSamplerFxActionSide, kSamplerFxActionSide);
                button->setCursor(Qt::PointingHandCursor);
                return button;
            };

            auto* bypass = action(icons::Glyph::Power, tr("Bypass this effect"));
            bypass->setCheckable(true);
            bypass->setChecked(slot.bypassed);
            bypass->setActiveColor(Theme::mute());
            connect(bypass, &QAbstractButton::clicked, this, [this, slot](bool on) {
                m_controller->setInsertBypassed(m_channelId.toStdString(), slot.id, on);
                m_fxSignature.clear(); emit projectEdited();
            });
            row->addActionButton(bypass);

            auto* open = action(icons::Glyph::Detach, tr("Open the effect window"));
            connect(open, &QAbstractButton::clicked, this, [this, slot] {
                emit pluginEditorRequested(m_channelId, QString::fromStdString(slot.id));
            });
            row->addActionButton(open);

            auto* replace = action(icons::Glyph::Chevron, tr("Replace this effect"));
            connect(replace, &QAbstractButton::clicked, this, [this, slot, index] {
                showFxMenu(index, QString::fromStdString(slot.id));
            });
            row->addActionButton(replace);
            m_fxSlotsLayout->addWidget(row);
        } else {
            auto* add = new QToolButton(m_fxSlotsHost);
            add->setObjectName(QStringLiteral("SamplerMixerSlot"));
            add->setProperty("active", false);
            add->setText(tr("INSERT %1").arg(index + 1));
            add->setFixedHeight(kSamplerFxSlotHeight);
            add->setToolButtonStyle(Qt::ToolButtonTextOnly);
            add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            add->setCursor(Qt::PointingHandCursor);
            add->setToolTip(tr("Empty insert slot — click to load an effect."));
            add->setAccessibleName(tr("Empty insert %1").arg(index + 1));
            connect(add, &QToolButton::clicked, this,
                    [this, index] { showFxMenu(index); });
            m_fxSlotsLayout->addWidget(add);
        }
    }
}

void SamplerPanel::showFxMenu(int index, const QString& replaceId) {
    auto* menu = ui::buildPluginMenu(
        this, m_controller, false,
        [this, index, replaceId](const daw::plugins::PluginDescriptor& descriptor) {
            bool ok = false;
            std::string addedId;
            if (m_context == Context::Instrument) {
                if (replaceId.isEmpty()) {
                    addedId = m_controller->addSamplerFxInsert(
                        m_channelId.toStdString(), m_slotId.toStdString(), descriptor,
                        std::size_t(index));
                    ok = !addedId.empty();
                } else {
                    ok = m_controller->replaceSamplerFxInsert(
                        m_channelId.toStdString(), m_slotId.toStdString(),
                        replaceId.toStdString(), descriptor);
                }
            } else {
                if (replaceId.isEmpty()) {
                    addedId = m_controller->addClipFxInsert(
                        m_channelId.toStdString(), m_slotId.toStdString(), descriptor,
                        std::size_t(index));
                    ok = !addedId.empty();
                } else {
                    ok = m_controller->replaceClipFxInsert(
                        m_channelId.toStdString(), m_slotId.toStdString(),
                        replaceId.toStdString(), descriptor);
                }
            }
            if (!ok) {
                QMessageBox::warning(this, tr("Sample Editor FX"),
                                     tr("The effect could not be loaded safely."));
                return;
            }
            m_fxSignature.clear();
            rebuildFxSlots();
            emit projectEdited();
            if (!addedId.empty())
                emit pluginEditorRequested(m_channelId,
                                           QString::fromStdString(addedId));
        });
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QCursor::pos());
}

void SamplerPanel::showFxContext(const QString& insertId, int index,
                                 const QPoint& globalPos) {
    const std::vector<daw::InsertModel>* inserts = nullptr;
    if (m_controller) {
        if (m_context == Context::Instrument) {
            if (const daw::SamplerFxModel* fx = m_controller->samplerFx(
                    m_channelId.toStdString(), m_slotId.toStdString())) {
                inserts = &fx->inserts;
            }
        } else {
            inserts = m_controller->clipFx(m_channelId.toStdString(),
                                           m_slotId.toStdString());
        }
    }
    if (!inserts || index < 0 || index >= int(inserts->size())) return;
    const daw::InsertModel slot = (*inserts)[std::size_t(index)];
    QMenu menu(this);
    QAction* open = menu.addAction(tr("Open"));
    QAction* bypass = menu.addAction(slot.bypassed ? tr("Enable") : tr("Bypass"));
    QAction* replace = menu.addAction(tr("Replace…"));
    menu.addSeparator();
    QAction* up = menu.addAction(tr("Move Up")); up->setEnabled(index > 0);
    QAction* down = menu.addAction(tr("Move Down"));
    down->setEnabled(index + 1 < int(inserts->size()));
    QAction* remove = menu.addAction(tr("Remove"));
    QAction* picked = menu.exec(globalPos);
    if (picked == open) emit pluginEditorRequested(m_channelId, insertId);
    else if (picked == bypass) m_controller->setInsertBypassed(
        m_channelId.toStdString(), insertId.toStdString(), !slot.bypassed);
    else if (picked == replace) { showFxMenu(index, insertId); return; }
    else if (picked == up) {
        if (m_context == Context::Instrument)
            m_controller->moveSamplerFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString(), std::size_t(index - 1));
        else
            m_controller->moveClipFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString(), std::size_t(index - 1));
    } else if (picked == down) {
        if (m_context == Context::Instrument)
            m_controller->moveSamplerFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString(), std::size_t(index + 1));
        else
            m_controller->moveClipFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString(), std::size_t(index + 1));
    } else if (picked == remove) {
        if (m_context == Context::Instrument)
            m_controller->removeSamplerFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString());
        else
            m_controller->removeClipFxInsert(
                m_channelId.toStdString(), m_slotId.toStdString(),
                insertId.toStdString());
    }
    else return;
    m_fxSignature.clear(); rebuildFxSlots(); emit projectEdited();
}

QWidget* SamplerPanel::buildSamplerBody() {
    auto* host = new QWidget(this);
    host->setObjectName(QStringLiteral("SamplerBody"));
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(host);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(14, 12, 14, 14);
    column->setSpacing(9);
    QWidget* envelopeSection = buildEnvelopeSection();
    if (m_context == Context::Clip) {
        envelopeSection->setEnabled(false);
        auto* disabledAppearance = new QGraphicsOpacityEffect(envelopeSection);
        disabledAppearance->setOpacity(0.48);
        envelopeSection->setGraphicsEffect(disabledAppearance);
        envelopeSection->setToolTip(
            tr("Volume Envelope is shown for layout parity with Sampler, but "
               "timeline clips use their own Start/End and fade geometry."));
        envelopeSection->setAccessibleDescription(
            tr("Unavailable in timeline Clip context"));
    }
    column->addWidget(envelopeSection);
    column->addWidget(buildToolSection());

    {
        auto* keysBox = new QWidget(page);
        keysBox->setObjectName(QStringLiteral("SamplerSection"));
        auto* keysLayout = new QVBoxLayout(keysBox);
        keysLayout->setContentsMargins(10, 5, 10, 7);
        keysLayout->setSpacing(5);
        auto* toggle = new QToolButton(keysBox);
        toggle->setObjectName(QStringLiteral("SamplerCollapse"));
        toggle->setText(tr("KEYBOARD"));
        toggle->setArrowType(Qt::RightArrow);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setCheckable(true);
        toggle->setChecked(false);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setToolTip(tr("Show root-note assignment and keyboard audition"));
        toggle->setAccessibleName(tr("Keyboard section"));
        keysLayout->addWidget(toggle);

        auto* keyBody = new QWidget(keysBox);
        auto* keyBodyLayout = new QVBoxLayout(keyBody);
        keyBodyLayout->setContentsMargins(0, 0, 0, 0);
        keyBodyLayout->setSpacing(4);
        auto* hint = caption(tr("Right click: root · Left drag: audition"), keyBody);
        hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        keyBodyLayout->addWidget(hint);
        auto* keyScroll = new QScrollArea(keyBody);
        keyScroll->setWidgetResizable(false);
        keyScroll->setFrameShape(QFrame::NoFrame);
        keyScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        keyScroll->setFixedHeight(70);
        m_keyboard = new SamplerKeyboard(keyScroll);
        keyScroll->setWidget(m_keyboard);
        m_keyboard->noteOn = [this](int pitch) {
            if (!m_controller) return;
            if (m_context == Context::Instrument) {
                m_controller->liveNoteOn(m_channelId.toStdString(), pitch, 100);
            } else if (const daw::ClipModel* clip = m_controller->audioClip(
                           m_channelId.toStdString(), m_slotId.toStdString())) {
                const double root = readParameter(QStringLiteral("rootnote"));
                const double editedPitch =
                    readParameter(QStringLiteral("stretch.pitch"));
                m_controller->previewFile(clip->filePath, false,
                                          double(pitch) - root + editedPitch);
            }
        };
        m_keyboard->noteOff = [this](int pitch) {
            if (!m_controller) return;
            if (m_context == Context::Instrument)
                m_controller->liveNoteOff(m_channelId.toStdString(), pitch);
            else
                m_controller->stopPreview();
        };
        m_keyboard->rootChanged = [this](int pitch) {
            beginGesture(QStringLiteral("rootnote"));
            writeParameter(QStringLiteral("rootnote"), pitch);
            endGesture(QStringLiteral("rootnote"));
            emit projectEdited();
        };
        keyBodyLayout->addWidget(keyScroll);
        keyBody->hide();
        connect(toggle, &QAbstractButton::toggled, keyBody,
                [toggle, keyBody](bool open) {
                    keyBody->setVisible(open);
                    toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
                    toggle->setAccessibleDescription(
                        open ? QObject::tr("Expanded") : QObject::tr("Collapsed"));
                });
        QTimer::singleShot(0, keyScroll, [this, keyScroll] {
            keyScroll->horizontalScrollBar()->setValue(
                std::max(0, m_keyboard->xForPitch(60) -
                                keyScroll->viewport()->width() / 2));
        });
        keysLayout->addWidget(keyBody);
        column->addWidget(keysBox);
    }
    column->addWidget(buildWaveformSection());
    column->addStretch(1);
    scroll->setWidget(page);
    hostLayout->addWidget(scroll);
    return host;
}

QWidget* SamplerPanel::buildEnvelopeSection() {
    auto* content = new QVBoxLayout;
    content->setSpacing(5);
    auto* row = knobRow();
    row->addWidget(led(QStringLiteral("amp.on"), tr("On")));
    row->addWidget(knob(QStringLiteral("amp.delay"), tr("Delay")));
    row->addWidget(knob(QStringLiteral("amp.att"), tr("Attack")));
    row->addWidget(knob(QStringLiteral("amp.atttens"), tr("A Tens"), true));
    row->addWidget(knob(QStringLiteral("amp.hold"), tr("Hold")));
    row->addWidget(knob(QStringLiteral("amp.dec"), tr("Decay")));
    row->addWidget(knob(QStringLiteral("amp.dectens"), tr("D Tens"), true));
    row->addWidget(knob(QStringLiteral("amp.sus"), tr("Sustain")));
    row->addWidget(knob(QStringLiteral("amp.rel"), tr("Release")));
    row->addWidget(knob(QStringLiteral("amp.reltens"), tr("R Tens"), true));
    row->addStretch(1);
    content->addLayout(row);
    m_envelope = new SamplerEnvelopeView(this);
    m_envelope->beginEdit = [this](const QString& id) { beginGesture(id); };
    m_envelope->changeValue = [this](const QString& id, double value) {
        writeParameter(id, value);
        if (ui::Knob* control = m_knobs.value(id)) control->setValue(value);
    };
    m_envelope->endEdit = [this](const QString& id) {
        endGesture(id); emit projectEdited();
    };
    content->addWidget(m_envelope);
    return sectionBox(m_context == Context::Instrument
                          ? tr("VOLUME ENVELOPE")
                          : tr("VOLUME ENVELOPE · DISABLED IN CLIP"),
                      content, this);
}

QWidget* SamplerPanel::buildToolSection() {
    auto* content = new QVBoxLayout;
    content->setSpacing(4);

    auto* tabs = new QTabBar(this);
    tabs->setObjectName(QStringLiteral("SamplerToolsTabs"));
    tabs->setExpanding(false);
    tabs->setDrawBase(false);
    tabs->addTab(tr("PLAYBACK"));
    tabs->addTab(tr("PROCESSING"));
    content->addWidget(tabs);

    auto* pages = new QStackedWidget(this);
    pages->setObjectName(QStringLiteral("SamplerToolsPages"));

    auto* playbackPage = new QWidget(pages);
    auto* playbackLayout = new QVBoxLayout(playbackPage);
    playbackLayout->setContentsMargins(0, 0, 0, 0);
    playbackLayout->setSpacing(3);
    auto* playback = knobRow();
    playback->setSpacing(4);
    auto* cutItself = led(QStringLiteral("cutitself"), tr("CUT ITSELF"));
    cutItself->setToolTip(
        m_context == Context::Instrument
            ? tr("A new trigger immediately stops every older voice in this Sampler.")
            : tr("CUT ITSELF is note-triggered and is unavailable for a timeline clip."));
    cutItself->setEnabled(m_context == Context::Instrument);
    playback->addWidget(cutItself);
    playback->addWidget(knob(QStringLiteral("startoffset"), tr("Start"), true));
    playback->addWidget(knob(QStringLiteral("endoffset"), tr("End"), true));
    playback->addWidget(knob(QStringLiteral("fadein"), tr("Fade In"), true));
    playback->addWidget(knob(QStringLiteral("fadeout"), tr("Fade Out"), true));
    auto* loopColumn = new QVBoxLayout;
    loopColumn->addWidget(caption(tr("Loop Mode"), this));
    loopColumn->addWidget(combo(QStringLiteral("loop.mode"),
                                {tr("Off"), tr("Forward"), tr("Ping-Pong")}));
    playback->addLayout(loopColumn);
    // Short enough to fit under the ring; the full names are still on the
    // tooltips, which every knob takes from the parameter table.
    playback->addWidget(knob(QStringLiteral("loop.start"), tr("L Start"), true));
    playback->addWidget(knob(QStringLiteral("loop.end"), tr("L End"), true));
    auto* tune = knob(QStringLiteral("pitch"), tr("Tune"), true);
    auto* range = knob(QStringLiteral("pitchrange"), tr("Range"), true);
    tune->setEnabled(m_context == Context::Instrument);
    range->setEnabled(m_context == Context::Instrument);
    if (m_context == Context::Clip) {
        tune->setToolTip(tr("MIDI key tracking is unavailable for a timeline clip."));
        range->setToolTip(tr("MIDI pitch range is unavailable for a timeline clip."));
    }
    playback->addWidget(tune);
    playback->addWidget(range);
    playback->addStretch(1);
    playbackLayout->addLayout(playback);

    auto* stretchBlock = new QWidget(playbackPage);
    stretchBlock->setObjectName(QStringLiteral("SamplerStretchBlock"));
    auto* stretch = new QHBoxLayout(stretchBlock);
    stretch->setContentsMargins(7, 3, 7, 3);
    stretch->setSpacing(5);
    stretch->addWidget(ui::sectionLabel(tr("STRETCH"), stretchBlock));
    auto* mode = combo(QStringLiteral("stretch.mode"),
                       {tr("Resample"), tr("Drums"), tr("Loop"),
                        tr("Vocal"), tr("Complex")});
    mode->setToolTip(tr("Selects the playback strategy, not only a control preset."));
    stretch->addWidget(mode);
    stretch->addWidget(knob(QStringLiteral("stretch.time"), tr("Time"), true));
    stretch->addWidget(knob(QStringLiteral("stretch.pitch"), tr("Pitch"), true));
    m_formantKnob = knob(QStringLiteral("formant"), tr("Formant"), true);
    m_formantKnob->setToolTip(
        tr("Tilts the spectral envelope — darker below zero, brighter above — "
           "without changing pitch or duration."));
    stretch->addWidget(m_formantKnob);
    auto* keepOnDisk = led(QStringLiteral("keepondisk"), tr("Disk"));
    keepOnDisk->setEnabled(m_context == Context::Instrument);
    if (m_context == Context::Clip) {
        keepOnDisk->setToolTip(
            tr("Timeline clips already stream from their referenced media file."));
    }
    stretch->addWidget(keepOnDisk);
    stretch->addStretch(1);
    playbackLayout->addWidget(stretchBlock);
    pages->addWidget(playbackPage);

    auto* processingPage = new QWidget(pages);
    auto* processingLayout = new QVBoxLayout(processingPage);
    processingLayout->setContentsMargins(0, 0, 0, 0);
    processingLayout->setSpacing(3);
    auto* effects = knobRow();
    effects->setSpacing(2);
    auto* modX = knob(QStringLiteral("modx"), tr("Mod X"), true);
    auto* modY = knob(QStringLiteral("mody"), tr("Mod Y"), true);
    modX->setEnabled(m_context == Context::Instrument);
    modY->setEnabled(m_context == Context::Instrument);
    if (m_context == Context::Clip) {
        modX->setToolTip(tr("Sampler modulation routing is unavailable for a timeline clip."));
        modY->setToolTip(tr("Sampler modulation routing is unavailable for a timeline clip."));
    }
    effects->addWidget(modX);
    effects->addWidget(modY);
    effects->addWidget(knob(QStringLiteral("pre.boost"), tr("Boost")));
    effects->addWidget(knob(QStringLiteral("pre.eq.low"), tr("EQ Lo")));
    effects->addWidget(knob(QStringLiteral("pre.eq.mid"), tr("EQ Mid")));
    effects->addWidget(knob(QStringLiteral("pre.eq.high"), tr("EQ Hi")));
    effects->addWidget(knob(QStringLiteral("pre.rm.mix"), tr("RM Mix")));
    effects->addWidget(knob(QStringLiteral("pre.rm.freq"), tr("RM Freq")));
    effects->addWidget(knob(QStringLiteral("pre.cut"), tr("Cut")));
    effects->addWidget(knob(QStringLiteral("pre.res"), tr("Res")));
    auto* reverbColumn = new QVBoxLayout;
    reverbColumn->addWidget(caption(tr("Reverb"), this));
    reverbColumn->addWidget(combo(QStringLiteral("pre.rev.type"),
                                  {tr("Room"), tr("Hall")}));
    effects->addLayout(reverbColumn);
    effects->addWidget(knob(QStringLiteral("pre.rev"), tr("Amount")));
    effects->addWidget(knob(QStringLiteral("pre.delay"), tr("St Delay")));
    effects->addWidget(knob(QStringLiteral("pre.pogo"), tr("Pogo")));
    effects->addStretch(1);
    processingLayout->addLayout(effects);
    auto* switches = knobRow();
    switches->addWidget(led(QStringLiteral("pre.dc"), tr("Remove DC")));
    switches->addWidget(led(QStringLiteral("pre.polarity"), tr("Polarity")));
    switches->addWidget(led(QStringLiteral("pre.normalize"), tr("Normalize")));
    switches->addWidget(led(QStringLiteral("pre.fadestereo"), tr("Fade Stereo")));
    switches->addWidget(led(QStringLiteral("pre.reverse"), tr("Reverse")));
    switches->addWidget(led(QStringLiteral("pre.swap"), tr("Swap Stereo")));
    switches->addStretch(1);
    processingLayout->addLayout(switches);
    pages->addWidget(processingPage);

    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    const int initialPage = qEnvironmentVariableIsSet("DAW_SHOT_SAMPLER_PROCESSING")
                                ? 1
                                : 0;
    tabs->setCurrentIndex(initialPage);
    pages->setCurrentIndex(initialPage);
    content->addWidget(pages);
    return sectionBox(tr("SAMPLE TOOLS"), content, this);
}

QWidget* SamplerPanel::buildWaveformSection() {
    auto* content = new QVBoxLayout;
    content->setSpacing(6);
    auto* fileRow = new QHBoxLayout;
    m_fileLabel = new QLabel(this);
    m_fileLabel->setObjectName(QStringLiteral("SamplerFile"));
    m_fileLabel->setMinimumWidth(0);
    m_fileLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_fileLabel->setCursor(Qt::PointingHandCursor);
    m_fileLabel->setToolTip(tr("Click to show the sample in the file manager"));
    m_infoLabel = new QLabel(this);
    m_infoLabel->setObjectName(QStringLiteral("SamplerCaption"));
    auto* load = new QPushButton(tr("Load…"), this);
    auto* reveal = new QPushButton(tr("Show"), this);
    auto* clear = new QPushButton(tr("Clear"), this);
    for (QPushButton* button : {load, reveal, clear}) {
        button->setObjectName(QStringLiteral("SamplerButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    connect(load, &QPushButton::clicked, this, &SamplerPanel::openSampleDialog);
    connect(reveal, &QPushButton::clicked, this, &SamplerPanel::revealSample);
    connect(clear, &QPushButton::clicked, this, [this] {
        if (!m_controller) return;
        if (m_context == Context::Instrument) {
            m_controller->clearSamplerSample(
                m_channelId.toStdString(), m_slotId.toStdString());
        } else {
            m_controller->setClipAudioFile(
                m_channelId.toStdString(), m_slotId.toStdString(), {});
        }
        emit projectEdited(); refresh();
    });
    fileRow->addWidget(m_fileLabel, 1);
    fileRow->addWidget(m_infoLabel);
    fileRow->addWidget(load);
    fileRow->addWidget(reveal);
    fileRow->addWidget(clear);
    content->addLayout(fileRow);
    m_waveform = new SamplerWaveform(this);
    m_waveform->setMinimumHeight(132);
    connect(m_waveform, &SamplerWaveform::markerMoved, this,
            [this](const QString& id, double value) {
                beginGesture(id); writeParameter(id, value);
                if (ui::Knob* control = m_knobs.value(id)) control->setValue(value);
                refresh();
            });
    connect(m_waveform, &SamplerWaveform::markerReleased, this,
            [this](const QString& id) { endGesture(id); emit projectEdited(); });
    content->addWidget(m_waveform);
    m_fileLabel->installEventFilter(this);
    return sectionBox(tr("SAMPLE WAVEFORM"), content, this);
}

// ── Refresh ──

void SamplerPanel::refresh() {
    sampler::SamplerInstance* instance = sampler();
    std::shared_ptr<const sampler::SampleData> data = currentSample();

    if (instance || m_context == Context::Clip) {
        std::string path;
        std::string name;
        if (instance) {
            path = instance->samplePath();
            name = instance->sampleName();
        } else if (const daw::ClipModel* clip = m_controller->audioClip(
                       m_channelId.toStdString(), m_slotId.toStdString())) {
            path = clip->filePath;
            name = clip->name.empty() ? QFileInfo(QString::fromStdString(path))
                                            .fileName().toStdString()
                                      : clip->name;
        }
        m_fileLabel->setText(name.empty() ? tr("No sample")
                                          : QString::fromStdString(name));
        m_waveform->setSample(data);
        if (data && data->audio) {
            const double seconds =
                data->audio->sampleRate() > 0.0
                    ? double(data->baseFrames) / data->audio->sampleRate()
                    : 0.0;
            m_infoLabel->setText(tr("%1 ch · %2 kHz · %3 s")
                                     .arg(data->audio->channels())
                                     .arg(data->audio->sampleRate() / 1000.0, 0, 'f', 1)
                                     .arg(seconds, 0, 'f', 2));
        } else {
            // A path with no audio behind it is a sample whose file has moved,
            // and saying so beats an empty strip.
            m_infoLabel->setText(path.empty() ? QString() : tr("file not found"));
        }
    } else {
        m_fileLabel->setText(tr("Sampler not loaded"));
        m_infoLabel->clear();
    }

    for (auto it = m_knobs.begin(); it != m_knobs.end(); ++it) {
        if (it.value()->isEditing()) continue;   // never fight a live drag
        it.value()->setValue(readParameter(it.key()));
    }
    for (auto it = m_leds.begin(); it != m_leds.end(); ++it) {
        const bool on = readParameter(it.key()) >= 0.5;
        if (it.value()->isChecked() == on) continue;
        const QSignalBlocker block(it.value());
        it.value()->setChecked(on);
    }
    for (auto it = m_combos.begin(); it != m_combos.end(); ++it) {
        const int index = int(std::lround(readParameter(it.key())));
        if (it.value()->currentIndex() == index) continue;
        const QSignalBlocker block(it.value());
        it.value()->setCurrentIndex(index);
    }

    if (m_envelope) {
        const QStringList ids{
            QStringLiteral("amp.delay"), QStringLiteral("amp.att"),
            QStringLiteral("amp.atttens"), QStringLiteral("amp.hold"),
            QStringLiteral("amp.dec"), QStringLiteral("amp.dectens"),
            QStringLiteral("amp.sus"), QStringLiteral("amp.rel"),
            QStringLiteral("amp.reltens")};
        for (const QString& id : ids) m_envelope->setValue(id, readParameter(id));
    }
    if (m_keyboard) {
        m_keyboard->setRoot(
            int(std::lround(readParameter(QStringLiteral("rootnote")))));
    }

    if (m_controller) {
        const std::vector<daw::InsertModel>* inserts = nullptr;
        float volume = 1.0f;
        float pan = 0.0f;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        if (m_context == Context::Instrument) {
            if (const daw::SamplerFxModel* fx = m_controller->samplerFx(
                    m_channelId.toStdString(), m_slotId.toStdString())) {
                inserts = &fx->inserts;
                volume = fx->volume;
                pan = fx->pan;
                peakLeft = m_controller->samplerFxPeakLeft(
                    m_channelId.toStdString());
                peakRight = m_controller->samplerFxPeakRight(
                    m_channelId.toStdString());
            }
        } else if (const daw::ClipModel* clip = m_controller->audioClip(
                       m_channelId.toStdString(), m_slotId.toStdString())) {
            inserts = &clip->inserts;
            volume = clip->gain;
            pan = clip->pan;
            peakLeft = m_controller->clipFxPeakLeft(
                m_channelId.toStdString(), m_slotId.toStdString());
            peakRight = m_controller->clipFxPeakRight(
                m_channelId.toStdString(), m_slotId.toStdString());
            // Before the first private insert exists the track meter is the
            // closest truthful reading; once a chain exists its private meter
            // takes over without changing the strip.
            if (inserts->empty()) {
                peakLeft = peakRight = m_controller->trackPeak(
                    m_channelId.toStdString());
            }
        }
        if (inserts) {
            if (!m_fxLevelGesture) {
                if (m_fxPan) m_fxPan->setPan(pan);
                if (m_fxVolume) m_fxVolume->setGain(volume);
            }
            if (m_fxPanLabel) {
                if (std::abs(pan) < 0.01f) {
                    m_fxPanLabel->setText(QStringLiteral("C"));
                } else {
                    m_fxPanLabel->setText(
                        QStringLiteral("%1%2")
                            .arg(pan < 0.0f ? QStringLiteral("L")
                                           : QStringLiteral("R"))
                            .arg(int(std::round(std::abs(pan) * 100.0f))));
                }
            }
            if (m_fxGainLabel) m_fxGainLabel->setText(ui::formatGainDb(volume));
            if (m_fxMeter) m_fxMeter->setPeaks(peakLeft, peakRight);
            if (m_fxBypass) {
                const bool bypassed = !inserts->empty() &&
                    std::all_of(inserts->begin(), inserts->end(),
                                [](const daw::InsertModel& slot) {
                                    return slot.bypassed;
                                });
                const QSignalBlocker blocker(m_fxBypass);
                m_fxBypass->setChecked(bypassed);
            }
            QString signature;
            for (const daw::InsertModel& slot : *inserts) {
                signature += QString::fromStdString(slot.id) + QLatin1Char('|') +
                             QString::fromStdString(slot.name) + QLatin1Char('|') +
                             QString::number(slot.bypassed) + QLatin1Char(';');
            }
            if (signature != m_fxSignature) {
                m_fxSignature = signature;
                rebuildFxSlots();
            }
        }
    }

    m_waveform->setMarkers(readParameter(QStringLiteral("startoffset")),
                           readParameter(QStringLiteral("endoffset")),
                           readParameter(QStringLiteral("loop.start")),
                           readParameter(QStringLiteral("loop.end")),
                           int(std::lround(readParameter(QStringLiteral("loop.mode")))),
                           readParameter(QStringLiteral("fadein")),
                           readParameter(QStringLiteral("fadeout")));
}

void SamplerPanel::openSampleDialog() {
    if (!m_controller) return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load Sample"), QString(), ui::audioNameFilter());
    if (path.isEmpty()) return;
    const bool loaded = m_context == Context::Instrument
        ? m_controller->loadSamplerSample(m_channelId.toStdString(),
                                          m_slotId.toStdString(), path.toStdString())
        : m_controller->setClipAudioFile(m_channelId.toStdString(),
                                         m_slotId.toStdString(), path.toStdString());
    if (!loaded) {
        QMessageBox::warning(this, tr("Sample Editor"),
                             tr("The audio file could not be loaded safely."));
        return;
    }
    emit projectEdited();
    refresh();
}

void SamplerPanel::revealSample() {
    QString path;
    if (sampler::SamplerInstance* instance = sampler()) {
        path = QString::fromStdString(instance->samplePath());
    } else if (m_context == Context::Clip && m_controller) {
        if (const daw::ClipModel* clip = m_controller->audioClip(
                m_channelId.toStdString(), m_slotId.toStdString()))
            path = QString::fromStdString(clip->filePath);
    }
    if (path.isEmpty()) return;
    // The containing folder, not the file: opening the file itself would hand
    // it to whatever audio player is registered, which is not what "show" means.
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
