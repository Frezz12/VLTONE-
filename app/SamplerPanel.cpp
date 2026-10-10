#include "graphics/ScenePaintSource.hpp"
#include "graphics/SceneRecordingTag.hpp"
#include "graphics/SceneRecorder.hpp"
#include <QHashFunctions>
#include "SamplerPanel.hpp"
#include "AudioEditPanel.hpp"
#include <QTabWidget>
#include "AudioImportPreparation.hpp"
#include <QThreadPool>
#include <QPointer>
#include <QApplication>
#include "FileTypes.hpp"

#include "Controls.hpp"
#include "ConsoleLevelWell.hpp"
#include "EngineController.hpp"
#include "PluginPickerMenu.hpp"
#include "Theme.hpp"

#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerVoice.hpp"

#include <QAbstractButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
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
#include <QTreeWidget>
#include <QWidgetAction>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

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
    box->setAttribute(Qt::WA_StyledBackground, true);
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(10, 9, 10, 9);
    column->setSpacing(7);
    if (!title.isEmpty()) {
        auto* label = new QLabel(title, box);
        label->setObjectName(QStringLiteral("SamplerGroupTitle"));
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        column->addWidget(label);
    }
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

// Keep the keyboard directly below the active page. QStackedWidget normally
// reserves space for the tallest hidden page as well.
class SamplerToolPages final : public QStackedWidget {
public:
    explicit SamplerToolPages(QWidget* parent) : QStackedWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this,
                [this] { updateGeometry(); });
    }

    QSize sizeHint() const override {
        QSize hint = QStackedWidget::sizeHint();
        if (currentWidget()) hint.setHeight(currentWidget()->sizeHint().height());
        return hint;
    }

    QSize minimumSizeHint() const override {
        QSize hint = QStackedWidget::minimumSizeHint();
        if (currentWidget()) hint.setHeight(currentWidget()->minimumSizeHint().height());
        return hint;
    }
};

// A small checkbox is easier to discover than an unlit LED on a dark well.
// Keep Led's binding API; only the Sampler's boolean controls use this paint.
class SamplerToggle final : public ui::Led {
public:
    using ui::Led::Led;
    QSize sizeHint() const override {
        QFont label = font();
        label.setPixelSize(10);
        return QSize(QFontMetrics(label).horizontalAdvance(text()) + 22, 24);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const Theme& t = th();
        if (!isEnabled()) p.setOpacity(0.42);
        const QRectF box(1, (height() - 12) / 2.0, 12, 12);
        p.setPen(QPen(isChecked() ? t.accent : t.textSecondary, 1));
        p.setBrush(isChecked() ? t.accent : t.well());
        p.drawRoundedRect(box, 3, 3);
        if (isChecked()) {
            p.setPen(QPen(t.background, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF{box.topLeft() + QPointF(3, 6),
                                      box.topLeft() + QPointF(5, 8),
                                      box.topLeft() + QPointF(9, 4)});
        }
        QFont label = font();
        label.setPixelSize(10);
        p.setFont(label);
        p.setPen(isChecked() ? t.textPrimary : t.textSecondary);
        p.drawText(rect().adjusted(20, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};

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

SamplerWaveform::SamplerWaveform(QWidget* parent) : ui::FrameWidget(parent) {
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
        (!m_sample && !sample) ||
        (m_sample && sample && m_sample->baseFrames == sample->baseFrames);
    // Keep the last complete envelope during an effect bake. Only clearing
    // or replacing the source file should show an empty/loading strip.
    if ((!sample || !sample->audio || !m_sample || m_sample->path != sample->path) &&
        (!m_minima.isEmpty() || !m_maxima.isEmpty())) {
        m_minima.clear();
        m_maxima.clear();
        m_peakSample.reset();
        ++m_peakRevision;
    }
    m_sample = std::move(sample);
    if (!sameAudio) {
        rebuildPeaks();
        update();
        return;
    }
    if (m_peakSample && m_sample && m_peakSample->audio == m_sample->audio)
        m_peakSample = m_sample;
    // The panel polls; repaint only when the audio or its visible length changes.
    if (!sameLength) update();
}

void SamplerWaveform::setClipColor(const QColor& color) {
    if (m_clipColor == color) return;
    m_clipColor = color;
    update();
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
    ++m_peakGeneration;
    m_peaksFor = m_sample && m_sample->audio ? m_sample->audio.get() : nullptr;
    requestPeakBuild();
}

void SamplerWaveform::requestPeakBuild() {
    if (m_peakBuildBusy || !m_sample || !m_sample->audio) return;
    m_peakBuildBusy = true;

    const auto sample = m_sample->audio;
    const quint64 generation = m_peakGeneration;
    const QPointer<SamplerWaveform> guard(this);
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(2); pool.setThreadPriority(QThread::LowPriority);
        pool.setExpiryTimeout(5000); return true;
    }();
    Q_UNUSED(configured);
    pool.start([sample, generation, guard] {
    const daw::engine::SampleBuffer& audio = *sample;
    const daw::engine::FrameCount frames = audio.frames();

    // Fixed resolution, not one bucket per pixel: the strip is a few hundred
    // pixels wide and gets resized with the window, and this scan is the only
    // O(length of the sample) work the panel does.
    const int buckets =
        int(std::min<daw::engine::FrameCount>(frames, kWaveformBuckets));
    QVector<float> minima(buckets), maxima(buckets);
    const double perBucket = buckets ? double(frames) / double(buckets) : 0.;
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
        minima[b] = low;
        maxima[b] = high;
    }
        QMetaObject::invokeMethod(qApp, [guard, generation, minima = std::move(minima), maxima = std::move(maxima)]() mutable {
            if (!guard) return;
            guard->m_peakBuildBusy = false;
            if (guard->m_peakGeneration != generation) { guard->requestPeakBuild(); return; }
            guard->m_minima = std::move(minima);
            guard->m_maxima = std::move(maxima);
            guard->m_peakSample = guard->m_sample;
            ++guard->m_peakRevision;
            guard->update();
        }, Qt::QueuedConnection);
    });
}

double SamplerWaveform::xForFraction(double fraction) const {
    const auto& sample = m_peakSample ? m_peakSample : m_sample;
    if (!sample || !sample->audio) return 0.0;
    const double total = double(sample->audio->frames());
    const double base = sample->baseFrames > 0 ? double(sample->baseFrames) : total;
    if (total <= 0.0) return 0.0;
    return std::clamp(fraction, 0.0, 1.0) * base / total * double(width());
}

double SamplerWaveform::fractionForX(int x) const {
    const auto& sample = m_peakSample ? m_peakSample : m_sample;
    if (!sample || !sample->audio || width() <= 0) return 0.0;
    const double total = double(sample->audio->frames());
    const double base = sample->baseFrames > 0 ? double(sample->baseFrames) : total;
    if (base <= 0.0) return 0.0;
    return std::clamp(double(x) / double(width()) * total / base, 0.0, 1.0);
}

void SamplerWaveform::paintEvent(QPaintEvent*) {
    QPainter p(this);
    paintScene(p, QRegion(rect()));
}

void SamplerWaveform::paintWaveformBase(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const Theme& t = th();

    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    // Match the arrangement: the track/clip colour carries the identity, and
    // a plain white envelope stays legible on that saturated surface.
    const QColor clip = m_clipColor.isValid() ? m_clipColor : t.accent;
    const QColor surface = m_minima.isEmpty() ? t.well() : clip;
    p.setPen(QPen(mixColors(surface, Qt::black, 0.45), 1.0));
    p.setBrush(surface);
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

    const QColor grid(255, 255, 255, 20);
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
        p.fillRect(QRectF(baseEnd, 0, width() - baseEnd, height()), QColor(0, 0, 0, 36));
    }

    p.setPen(QPen(QColor(255, 255, 255, 56), 1.0));
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

    QPainterPath body;
    body.addPolygon(QPolygonF(highs));
    for (int x = columns - 1; x >= 0; --x) body.lineTo(lows[x]);
    body.closeSubpath();
    p.fillPath(body, Qt::white);

    p.restore();
}

void SamplerWaveform::paintScene(QPainter& p, const QRegion&) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const quint64 key = qHashMulti(0, m_peakRevision, m_minima.size(),
        m_peakSample ? m_peakSample->baseFrames : 0,
        m_peakSample && m_peakSample->audio ? m_peakSample->audio->frames() : 0,
        width(), height(), m_clipColor.rgba(),
        th().well().rgba(), th().accent.rgba(), th().textSecondary.rgba());
    auto* scene = ui::graphics::sceneGeometrySink(p);
    if (!scene || scene->beginRetainedSection(1002, key != m_gpuWaveformKey)) {
        p.save(); paintWaveformBase(p); p.restore();
        if (scene) { scene->endRetainedSection(); m_gpuWaveformKey = key; }
    }
    if (m_minima.isEmpty()) return;
    const Theme& t = th();
    const double baseEnd = xForFraction(1.0);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clipping;
    clipping.addRoundedRect(frame.adjusted(1.0, 1.0, -1.0, -1.0), 6.0, 6.0);
    p.save(); p.setClipPath(clipping);

    // ── Fades, drawn as the ramps they apply ──
    const double start = xForFraction(m_startOffset);
    const double end = xForFraction(m_endOffset);
    QColor shade = t.background;
    shade.setAlpha(t.dark ? 176 : 118);
    if (start > 0.0) p.fillRect(QRectF(0, 0, start, height()), shade);
    if (end < baseEnd) p.fillRect(QRectF(end, 0, baseEnd - end, height()), shade);

    const QColor fadeInk(0, 0, 0, 40);
    const QColor fadeEdge(255, 255, 255, 220);
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
        p.setPen(colour.lightnessF() > .55 ? QColor(20, 20, 20) : QColor(Qt::white));
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

class SamplerEnvelopeView : public QWidget, public ui::graphics::ScenePaintSource {
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

    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
    void paintScene(QPainter& p, const QRegion&) override {
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

class SamplerKeyboard : public QWidget, public ui::graphics::ScenePaintSource {
public:
    explicit SamplerKeyboard(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(75 * kWhiteWidth, 68);
        setMouseTracking(true);
        connect(&ThemeManager::instance(), &ThemeManager::changed, this,
                QOverload<>::of(&QWidget::update));
    }
    void setRoot(int pitch) {
        pitch = std::clamp(pitch, 0, 127);
        if (m_root == pitch) return;
        m_root = pitch;
        update();
    }
    int root() const { return m_root; }
    void stopAudition() { releasePressed(); }
    int xForPitch(int pitch) const {
        pitch = std::clamp(pitch, 0, 127);
        return whiteX(pitch) + (black(pitch) ? 0 : kWhiteWidth / 2);
    }
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
    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
    void paintScene(QPainter& p, const QRegion&) override {
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
            setRoot(pitch); if (rootChanged) rootChanged(pitch); return;
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

void SamplerPanel::showDefaultPage() {
    if(auto* tabs=findChild<QTabWidget*>("SamplerTabs"))tabs->setCurrentIndex(m_context==Context::Clip?1:0);
}

SamplerPanel::SamplerPanel(daw::EngineController* controller, Context context,
                           QString ownerId, QString objectId, QWidget* parent)
    : QWidget(parent), m_controller(controller), m_channelId(std::move(ownerId)),
      m_slotId(std::move(objectId)), m_context(context) {
    setObjectName(QStringLiteral("SamplerPanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAcceptDrops(true);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    outer->setSpacing(0);
    // Both contexts deliberately use the same shell.  A timeline clip is not
    // a reduced secondary dialog: it is the same Sample Editor with state
    // routed to a ClipModel instead of the built-in sampler instance.
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(0);
    splitter->addWidget(buildFxStrip());
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("SamplerTabs"));
    tabs->setDocumentMode(true);
    tabs->addTab(buildSamplerBody(), tr("Sampler"));
    m_audioEditor = new AudioEditPanel(m_controller,
        {m_channelId.toStdString(),m_slotId.toStdString(),m_context == Context::Instrument},tabs);
    tabs->addTab(m_audioEditor,tr("Editor"));
    connect(m_audioEditor,&AudioEditPanel::projectEdited,this,&SamplerPanel::projectEdited);
    connect(m_audioEditor,&AudioEditPanel::liveEdited,this,&SamplerPanel::liveEdited);
    tabs->setCurrentIndex(m_context == Context::Clip ? 1 : 0);
    splitter->addWidget(tabs);
    splitter->setSizes({118, 842});
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
    if (m_pitchCancelled) m_pitchCancelled->store(true);
    endGesture(QStringLiteral("finepitch"));
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
        const QPointer<SamplerPanel> guard(this);
        if (!ui::prepareAudioImport(this, *m_controller, url.toLocalFile()) || !guard) return;
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
    if (event->type() == QEvent::Wheel &&
        watched->objectName().startsWith(QStringLiteral("SamplerParameter."))) {
        // Propagate to the surrounding scroll area, never to the control.
        event->ignore();
        return true;
    }
    if (watched == m_fileLabel && event->type() == QEvent::MouseButtonRelease) {
        revealSample();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SamplerPanel::applyTheme() {
    const Theme& t = th();
    setStyleSheet(QString(R"(
#SamplerPanel, #SamplerBody, #SamplerBodyScroll, #SamplerPage { background: %BG%; }
#SamplerSection { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %TOP%,stop:1 %BOTTOM%);
                  border: 1px solid %BORDER%; border-top-color: %EDGE%; border-bottom-color: %SHADOW%; border-radius: %RADIUS%px; }
#SamplerGroupTitle { color: %TEXT2%; font-size: 10px; font-weight: 600; }
#SamplerFxStrip { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %TOP%,stop:1 %SURFACE%);
                  border: 1px solid %BORDER%; border-top-color: %EDGE%; border-bottom-color: %SHADOW%; border-radius: %RADIUS%px; }
#SamplerAccentBar { background: %ACCENT%; border-radius: 2px; }
#SamplerStripName { color: %TEXT%; font-size: 10px; font-weight: 700; }
#SamplerSlotWell { background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %WELL_TOP%,stop:1 %WELL_BOTTOM%);
    border: 1px solid %BORDER%; border-top-color: %WELL_EDGE%; border-bottom-color: %EDGE%; border-radius: %RADIUS%px; }
#SamplerMixerSlot { background: transparent; border: 1px solid transparent;
    border-bottom-color: %RACK_LINE%; border-radius: 2px;
    color: %TEXT2%; font-size: 9px; font-weight: 400; padding: 0 5px;
    text-align: left; }
#SamplerMixerSlot[active="true"] { color: %TEXT%; border-left-color: %ACCENT%; }
#SamplerMixerSlot[bypassed="true"] { color: %DIM%; border-color: %BYPASS%; }
#SamplerMixerSlot:hover { background: %HOVER%; color: %TEXT%; }
#SamplerMixerSlot::menu-indicator { image: none; width: 0; }
#SamplerInsertAddArea { background: transparent; border: none; border-radius: 6px; padding: 0; }
#SamplerInsertAddArea:hover, #SamplerInsertAddArea:focus { background: %HOVER%; }
#SamplerInsertAddArea:pressed { background: %WELL%; }
#SamplerInsertAddArea::menu-indicator { image: none; width: 0; }
#SamplerFxReadout { color: %TEXT%; font-size: 10px; font-weight: 500; }
#SamplerNamePlate { color: %TEXT%; background: %NAMEPLATE%; border-radius: %RADIUS%px;
    font-size: 10px; font-weight: 700; }
#SamplerStretchBlock { background: %WELL%; border: 1px solid %BORDER%; border-radius: %RADIUS%px; }
#SamplerCollapse { color: %TEXT%; background: transparent; border: none;
    text-align: left; padding: 2px 0; font-size: 10px; font-weight: 700; }
#SamplerCollapse:hover { color: %TEXT%; background: %HOVER%; }
QTabBar#SamplerToolsTabs::tab { color: %TEXT2%; background: transparent;
    border: 1px solid transparent; border-radius: 6px; padding: 6px 12px; margin-right: 4px;
    font-size: 11px; font-weight: 500; }
QTabBar#SamplerToolsTabs::tab:selected { color: %TEXT%;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %CONTROL_TOP%,stop:1 %CONTROL_BOTTOM%);
    border-color: %BORDER%; border-top-color: %EDGE%; border-bottom-color: %ACCENT%; }
QTabBar#SamplerToolsTabs::tab:hover { color: %TEXT%; background: %HOVER%; }
#SamplerCaption { color: %TEXT2%; font-size: 10px; }
#SamplerFile { color: %TEXT%; font-size: 12px; font-weight: 600; }
#SamplerButton { padding: 3px 10px; min-height: 18px; font-size: 11px; }
QComboBox { font-size: 11px; }
)").replace("%RADIUS%", QString::number(Theme::cornerRadius))
            .replace("%SURFACE%", t.surface.name())
            .replace("%TOP%", t.panelTop().name())
            .replace("%BOTTOM%", t.panelBottom().name())
            .replace("%EDGE%", t.edgeLight(t.panelTop()).name())
            .replace("%SHADOW%", t.edgeDark(t.panelBottom()).name())
            .replace("%CONTROL_TOP%", t.controlTop().name())
            .replace("%CONTROL_BOTTOM%", t.controlBottom().name())
            .replace("%WELL_TOP%", t.wellTop().name())
            .replace("%WELL_BOTTOM%", t.wellBottom().name())
            .replace("%WELL_EDGE%", t.edgeDark(t.well()).name())
            .replace("%RACK_LINE%", mixColors(t.well(), t.textPrimary, .10).name())
            .replace("%WELL%", t.well().name())
            .replace("%TEXT%", t.textPrimary.name())
            .replace("%TEXT2%", t.textSecondary.name())
            .replace("%ACCENT%", t.accent.name())
            .replace("%BORDER%", t.separator().name())
            .replace("%HOVER%", t.controlTop().name())
            .replace("%DIM%", mixColors(t.textSecondary, t.background, 0.35).name())
            .replace("%BYPASS%", mixColors(Theme::mute(), t.background, 0.45).name())
            .replace("%NAMEPLATE%", mixColors(t.surface, t.accent, 0.17).name())
            .replace("%BG%", t.background.name()));
    for (auto* add : findChildren<QToolButton*>(QStringLiteral("SamplerInsertAddArea")))
        add->setIcon(icons::icon(icons::Glyph::Plus, t.textSecondary, 14));
}

daw::SamplerSnapshot SamplerPanel::sampler() const {
    if (!m_controller || m_context != Context::Instrument) return {};
    return m_controller->samplerSnapshot(m_channelId.toStdString(),
                                        m_slotId.toStdString());
}

std::shared_ptr<const sampler::SampleData> SamplerPanel::currentSample() {
    if (!m_controller) return {};
    if (m_context == Context::Clip) {
        const auto track = m_channelId.toStdString(), clip = m_slotId.toStdString();
        auto data = m_controller->cachedClipSampleData(track, clip);
        if (!data) m_controller->requestClipSampleData(track, clip);
        return data;
    }
    return sampler().sample;
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
    // The sampler and clip editor retain their original digital controls.
    control->setVisualStyle(ui::Knob::VisualStyle::SamplerDigital);
    control->setObjectName(QStringLiteral("SamplerParameter.") + parameterId);
    if (m_context == Context::Instrument) control->setProperty("parameterId", parameterId);
    control->installEventFilter(this);
    control->setAccessibleName(captionText);
    control->setAccessibleDescription(tr("Drag vertically to adjust. Hold Shift for fine control. Double click to reset."));

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
    auto* lamp = new SamplerToggle(captionText, this);
    lamp->setChecked(readParameter(parameterId) >= 0.5);
    connect(lamp, &ui::Led::toggled, this, [this, parameterId](bool on) {
        // A lamp is one gesture in itself, so it opens and closes the undo
        // entry in the same click.
        beginGesture(parameterId);
        writeParameter(parameterId, on ? 1.0 : 0.0);
        endGesture(parameterId);
        refresh();
    });
    lamp->setObjectName(QStringLiteral("SamplerParameter.") + parameterId);
    lamp->setAccessibleName(captionText);
    lamp->setFocusPolicy(Qt::TabFocus);
    lamp->setMinimumHeight(24);
    m_leds.insert(parameterId, lamp);
    return lamp;
}

QComboBox* SamplerPanel::combo(const QString& parameterId, const QStringList& items) {
    auto* box = new QComboBox(this);
    box->setObjectName(QStringLiteral("SamplerParameter.") + parameterId);
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    box->setMinimumContentsLength(5);
    box->setMinimumWidth(0);
    box->setFixedWidth(120);
    box->setMinimumHeight(26);
    if (const auto* info = infoFor(parameterId))
        box->setAccessibleName(QString::fromStdString(info->name));
    box->installEventFilter(this);
    box->addItems(items);
    box->setCurrentIndex(int(std::lround(readParameter(parameterId))));
    connect(box, &QComboBox::currentIndexChanged, this, [this, parameterId, box](int index) {
        beginGesture(parameterId);
        writeParameter(parameterId, box->itemData(index).isValid() ? box->itemData(index).toDouble() : double(index));
        endGesture(parameterId);
        refresh();
    });
    m_combos.insert(parameterId, box);
    return box;
}

QWidget* SamplerPanel::buildFxStrip() {
    auto* strip = new QWidget(this);
    strip->setObjectName(QStringLiteral("SamplerFxStrip"));
    strip->setAttribute(Qt::WA_StyledBackground, true);
    strip->setMinimumWidth(112);
    strip->setMaximumWidth(128);
    auto* column = new QVBoxLayout(strip);
    column->setContentsMargins(8, 10, 8, 8);
    column->setSpacing(6);

    auto* stripHeader = new QHBoxLayout;
    stripHeader->setContentsMargins(0, 0, 0, 0);
    stripHeader->setSpacing(3);
    auto* swatch = new QWidget(strip);
    swatch->setObjectName(QStringLiteral("SamplerAccentBar"));
    swatch->setFixedSize(2, 12);
    auto* stripName = new QLabel(
        m_context == Context::Clip ? tr("Clip FX") : tr("Effects"), strip);
    stripName->setObjectName(QStringLiteral("SamplerStripName"));
    stripHeader->addWidget(swatch);
    stripHeader->addWidget(stripName, 1);
    column->addLayout(stripHeader);
    auto* fxActions = new QHBoxLayout;
    fxActions->setSpacing(2);
    fxActions->addWidget(caption(QStringLiteral("FX"), strip));
    fxActions->addStretch();
    m_fxBypass = new ui::IconButton(
        icons::Glyph::Power,
        m_context == Context::Instrument ? tr("Bypass all Sampler effects")
                                         : tr("Bypass all Clip effects"), strip);
    m_fxBypass->setButtonSize(20, 24);
    m_fxBypass->setCheckable(true);
    m_fxBypass->setActiveColor(Theme::mute());
    fxActions->addWidget(m_fxBypass);
    auto* addFx = new ui::IconButton(icons::Glyph::Plus, tr("Add effect"), strip);
    addFx->setButtonSize(20, 24);
    fxActions->addWidget(addFx);
    column->addLayout(fxActions);
    if (m_context == Context::Clip) {
        m_offlineHistory = new QToolButton(strip);
        m_offlineHistory->setObjectName(QStringLiteral("SamplerOfflineHistory"));
        m_offlineHistory->setAccessibleName(tr("Offline render history"));
        m_offlineHistory->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_offlineHistory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_offlineHistory->setFixedHeight(24);
        m_offlineHistory->setText(tr("History ▾"));
        m_offlineHistory->setToolTip(tr("Choose the original audio or an offline-rendered version"));
        column->addWidget(m_offlineHistory);
        connect(m_offlineHistory, &QToolButton::clicked, this, &SamplerPanel::showOfflineHistory);
    }
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

    auto* routing = new ui::ConsoleLevelWell(strip);
    routing->setObjectName(QStringLiteral("SamplerFxRouting"));
    routing->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* routingLayout = new QVBoxLayout(routing);
    routingLayout->setContentsMargins(3, 4, 3, 2);
    routingLayout->setSpacing(2);
    auto* panRow = new QHBoxLayout;
    panRow->setContentsMargins(0, 0, 0, 0);
    panRow->setSpacing(3);
    m_fxPan = new ui::PanKnob(routing);
    m_fxPan->setFixedSize(30, 30);
    m_fxPan->setAccessibleName(tr("Pan"));
    m_fxPanLabel = new QLabel(QStringLiteral("C"), routing);
    m_fxPanLabel->setObjectName(QStringLiteral("SamplerFxReadout"));
    m_fxPanLabel->setAlignment(Qt::AlignCenter);
    m_fxPanLabel->setFixedWidth(23);
    m_fxVolume = new ui::FaderWidget(Qt::Vertical, routing);
    m_fxVolume->setAccessibleName(tr("Volume"));
    m_fxMeter = new ui::LevelMeter(Qt::Vertical, 2, routing);
    ui::configureConsoleLevel(m_fxVolume, m_fxMeter);
    m_fxMeter->setFixedWidth(18);
    connect(m_fxMeter, &ui::LevelMeter::peakResetRequested, m_fxMeter,
            &ui::LevelMeter::clearClip);
    panRow->addStretch();
    panRow->addWidget(m_fxPan);
    panRow->addWidget(m_fxPanLabel);
    panRow->addStretch();
    routingLayout->addLayout(panRow);
    auto* levelRow = new QHBoxLayout;
    levelRow->setContentsMargins(0, 0, 0, 0);
    levelRow->setSpacing(5);
    levelRow->addStretch();
    levelRow->addWidget(m_fxVolume);
    levelRow->addWidget(m_fxMeter);
    levelRow->addStretch();
    routingLayout->addLayout(levelRow, 1);
    m_fxGainLabel = new QLabel(ui::formatGainDb(1.0), routing);
    m_fxGainLabel->setObjectName(QStringLiteral("SamplerFxReadout"));
    m_fxGainLabel->setAlignment(Qt::AlignCenter);
    m_fxGainLabel->setFixedHeight(24);
    routingLayout->addWidget(m_fxGainLabel);
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

void SamplerPanel::showOfflineHistory() {
    if (!m_controller || !m_offlineHistory) return;
    const auto* clip = m_controller->audioClip(m_channelId.toStdString(), m_slotId.toStdString());
    if (!clip) return;
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* tree = new QTreeWidget(menu);
    tree->setObjectName(QStringLiteral("OfflineHistoryTree"));
    tree->setAccessibleName(tr("Offline render versions"));
    tree->setHeaderHidden(true);
    tree->setIndentation(14);
    tree->setUniformRowHeights(true);
    tree->setMinimumWidth(330);
    tree->setFixedHeight(std::clamp(int(clip->offlineHistory.size() + 1) * 28, 90, 280));
    tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* root = new QTreeWidgetItem(tree, {tr("Original audio")});
    root->setSizeHint(0, QSize(0, 26));
    root->setData(0, Qt::UserRole, QString());
    if (!clip->offlineHistory.empty()) {
        root->setData(0, Qt::UserRole, QString::fromStdString(clip->offlineHistory.front().id));
        for (std::size_t index = 1; index < clip->offlineHistory.size(); ++index) {
            const auto& version = clip->offlineHistory[index];
            auto* item = new QTreeWidgetItem(root, {tr("Version %1 · %2").arg(index)
                .arg(QString::fromStdString(version.label))});
            item->setSizeHint(0, QSize(0, 26));
            item->setData(0, Qt::UserRole, QString::fromStdString(version.id));
            const auto parent = std::find_if(clip->offlineHistory.begin(), clip->offlineHistory.end(),
                [&](const auto& prior) { return prior.id == version.parentId; });
            item->setToolTip(0, tr("Based on version %1\n%2")
                .arg(std::distance(clip->offlineHistory.begin(), parent))
                .arg(QString::fromStdString(version.label)));
            if (version.id == clip->offlineVersionId) tree->setCurrentItem(item);
        }
        if (clip->offlineVersionId == clip->offlineHistory.front().id) tree->setCurrentItem(root);
    } else if (!clip->offlineProcess.empty()) {
        auto* current = new QTreeWidgetItem(root, {tr("Version 1 · Offline Render")});
        current->setFlags(Qt::NoItemFlags);
        tree->setCurrentItem(current);
    }
    root->setExpanded(true);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(tree);
    menu->addAction(action);
    const auto choose = [this, menu](QTreeWidgetItem* item) {
        if (!item || !(item->flags() & Qt::ItemIsEnabled) || menu->property("chosen").toBool()) return;
        menu->setProperty("chosen", true);
        const auto id = item->data(0, Qt::UserRole).toString().toStdString();
        menu->close();
        const daw::EngineController::ClipAddress address{m_channelId.toStdString(), m_slotId.toStdString()};
        const auto result = id.empty() ? m_controller->restoreOfflineRenderOriginal(address)
                                      : m_controller->selectOfflineRenderVersion(address, id);
        if (!result) {
            QMessageBox::warning(this, tr("Offline render history"), QString::fromStdString(result.message()));
            return;
        }
        emit projectEdited();
        refresh();
    };
    connect(tree, &QTreeWidget::itemClicked, menu, [choose](QTreeWidgetItem* item, int) { choose(item); });
    connect(tree, &QTreeWidget::itemActivated, menu, [choose](QTreeWidgetItem* item, int) { choose(item); });
    menu->popup(m_offlineHistory->mapToGlobal(QPoint(0, m_offlineHistory->height())));
    tree->setFocus(Qt::PopupFocusReason);
    if (tree->currentItem()) tree->scrollToItem(tree->currentItem());
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
            add->setObjectName(QStringLiteral("SamplerInsertAddArea"));
            add->setProperty("active", false);
            add->setIcon(icons::icon(icons::Glyph::Plus, th().textSecondary, 14));
            add->setIconSize(QSize(14, 14));
            add->setFixedHeight(inserts.empty() ? 2 * kSamplerFxSlotHeight : kSamplerFxSlotHeight);
            add->setToolButtonStyle(Qt::ToolButtonIconOnly);
            add->setFocusPolicy(Qt::StrongFocus);
            add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            add->setCursor(Qt::PointingHandCursor);
            add->setToolTip(tr("Empty insert slot — click to load an effect."));
            add->setAccessibleName(tr("Empty insert %1").arg(index + 1));
            connect(add, &QToolButton::clicked, this,
                    [this, index] { showFxMenu(index); });
            m_fxSlotsLayout->addWidget(add);
            break;
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
            ui::rememberRecentPlugin(descriptor);
            emit projectEdited();
            if (!addedId.empty())
                emit pluginEditorRequested(m_channelId,
                                           QString::fromStdString(addedId));
        }, {m_channelId, replaceId, [this] {
            m_fxSignature.clear();
            rebuildFxSlots();
            emit projectEdited();
        }});
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
    ui::addPluginControlsAction(&menu, m_controller, m_channelId, insertId);
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
    scroll->setObjectName(QStringLiteral("SamplerBodyScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("SamplerPage"));
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(8, 0, 0, 0);
    column->setSpacing(8);
    column->addWidget(buildWaveformSection());
    column->addWidget(buildToolSection());

    {
        auto* keysBox = new QWidget(page);
        keysBox->setObjectName(QStringLiteral("SamplerSection"));
        keysBox->setAttribute(Qt::WA_StyledBackground, true);
        auto* keysLayout = new QVBoxLayout(keysBox);
        keysLayout->setContentsMargins(10, 5, 10, 7);
        keysLayout->setSpacing(5);
        auto* toggle = new QToolButton(keysBox);
        toggle->setObjectName(QStringLiteral("SamplerCollapse"));
        toggle->setText(tr("Keyboard"));
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
        m_keyboardScroll = keyScroll;
        keyScroll->setObjectName(QStringLiteral("SamplerKeyboardScroll"));
        keyScroll->setWidgetResizable(false);
        keyScroll->setFrameShape(QFrame::NoFrame);
        keyScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_keyboard = new SamplerKeyboard(keyScroll);
        keyScroll->setWidget(m_keyboard);
        keyScroll->setFixedHeight(m_keyboard->height() +
                                  keyScroll->horizontalScrollBar()->sizeHint().height());
        m_keyboard->noteOn = [this](int pitch) {
            if (!m_controller) return;
            if (m_context == Context::Instrument) {
                m_controller->liveNoteOn(m_channelId.toStdString(), pitch, 127);
            } else if (const daw::ClipModel* clip = m_controller->audioClip(
                           m_channelId.toStdString(), m_slotId.toStdString())) {
                const double root = readParameter(QStringLiteral("rootnote"));
                const double editedPitch =
                    readParameter(QStringLiteral("stretch.pitch"));
                if (auto data = m_controller->cachedClipSampleData(m_channelId.toStdString(),m_slotId.toStdString()); data && data->audio)
                    m_controller->previewBuffer(data->audio,clip->filePath,false,double(pitch)-root+editedPitch);
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
            refresh();
        };
        keyBodyLayout->addWidget(keyScroll);
        keyBody->hide();
        connect(toggle, &QAbstractButton::toggled, keyBody,
                [this, toggle, keyBody, keyScroll](bool open) {
                    keyBody->setVisible(open);
                    toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
                    toggle->setAccessibleDescription(
                        open ? QObject::tr("Expanded") : QObject::tr("Collapsed"));
                    if (open) QTimer::singleShot(0, keyScroll, [this, keyScroll] {
                        keyScroll->ensureVisible(m_keyboard->xForPitch(m_keyboard->root()), 0, 24, 0);
                    });
                });
        QTimer::singleShot(0, keyScroll, [this, keyScroll] {
            keyScroll->horizontalScrollBar()->setValue(
                std::max(0, m_keyboard->xForPitch(m_keyboard->root()) -
                                keyScroll->viewport()->width() / 2));
        });
        keysLayout->addWidget(keyBody);
        column->addWidget(keysBox);
    }
    column->addStretch(1);
    scroll->setWidget(page);
    page->setAutoFillBackground(false);
    hostLayout->addWidget(scroll);
    return host;
}

QWidget* SamplerPanel::buildEnvelopeSection() {
    auto* row = knobRow();
    auto* shape = new QVBoxLayout;
    shape->setSpacing(8);
    m_envelope = new SamplerEnvelopeView(this);
    m_envelope->setFixedHeight(114);
    m_envelope->beginEdit = [this](const QString& id) { beginGesture(id); };
    m_envelope->changeValue = [this](const QString& id, double value) {
        writeParameter(id, value);
        if (ui::Knob* control = m_knobs.value(id)) control->setValue(value);
    };
    m_envelope->endEdit = [this](const QString& id) {
        endGesture(id); emit projectEdited();
    };
    shape->addWidget(m_envelope);
    auto* curves = knobRow();
    curves->addWidget(knob(QStringLiteral("amp.atttens"), tr("Attack curve")));
    curves->addWidget(knob(QStringLiteral("amp.dectens"), tr("Decay curve")));
    curves->addWidget(knob(QStringLiteral("amp.reltens"), tr("Release curve")));
    curves->addStretch();
    shape->addLayout(curves);
    shape->addStretch();
    row->addWidget(sectionBox(tr("Envelope shape"), shape, this), 1);

    auto* levels = new QVBoxLayout;
    auto* enabled = led(QStringLiteral("amp.on"), tr("On"));
    levels->addWidget(enabled);
    auto* grid = new QGridLayout;
    grid->setSpacing(6);
    const std::pair<const char*, QString> parameters[] = {
        {"amp.delay", tr("Delay")}, {"amp.att", tr("Attack")},
        {"amp.hold", tr("Hold")}, {"amp.dec", tr("Decay")},
        {"amp.sus", tr("Sustain")}, {"amp.rel", tr("Release")}};
    for (int i = 0; i < 6; ++i)
        grid->addWidget(knob(QString::fromLatin1(parameters[i].first),
                             parameters[i].second), i / 3, i % 3);
    levels->addLayout(grid);
    levels->addStretch();
    row->addWidget(sectionBox(tr("Volume envelope"), levels, this));
    auto* page = new QWidget(this);
    page->setLayout(row);
    return page;
}

QWidget* SamplerPanel::buildToolSection() {
    auto* host = new QWidget(this);
    auto* content = new QVBoxLayout(host);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(8);
    auto* tabs = new QTabBar(host);
    tabs->setObjectName(QStringLiteral("SamplerToolsTabs"));
    tabs->setExpanding(false);
    tabs->setDrawBase(false);
    tabs->setAccessibleName(tr("Sample settings"));
    auto* pages = new SamplerToolPages(host);
    pages->setObjectName(QStringLiteral("SamplerToolsPages"));
    content->addWidget(tabs);
    content->addWidget(pages);

    // Small groups give every dial a stable caption and readout without
    // requiring the whole processing chain to fit into one horizontal row.
    const auto group = [this](const QString& title, QLayout* layout) {
        return sectionBox(title, layout, this);
    };
    const auto choice = [this](const QString& title, QComboBox* box) {
        auto* column = new QVBoxLayout;
        auto* label = caption(title, this);
        label->setBuddy(box);
        column->addStretch();
        column->addWidget(label);
        column->addWidget(box);
        column->addStretch();
        return column;
    };
    const auto addPage = [tabs, pages](const QString& name, QWidget* page) {
        tabs->addTab(name);
        pages->addWidget(page);
    };
    auto* playbackPage = new QWidget(pages);
    auto* playback = new QVBoxLayout(playbackPage);
    playback->setContentsMargins(0, 0, 0, 0);
    playback->setSpacing(8);
    auto* playbackGrid = new QGridLayout;
    playbackGrid->setContentsMargins(0, 0, 0, 0);
    playbackGrid->setSpacing(8);
    playbackGrid->setColumnStretch(0, 1);
    playbackGrid->setColumnStretch(1, 1);
    auto* region = knobRow();
    region->addWidget(knob(QStringLiteral("startoffset"), tr("Start")));
    region->addWidget(knob(QStringLiteral("endoffset"), tr("End")));
    region->addWidget(knob(QStringLiteral("fadein"), tr("Fade In")));
    region->addWidget(knob(QStringLiteral("fadeout"), tr("Fade Out")));
    region->addStretch();
    playbackGrid->addWidget(group(tr("Sample region"), region), 0, 0);
    auto* loop = knobRow();
    loop->addLayout(choice(tr("Loop Mode"), combo(QStringLiteral("loop.mode"),
                            {tr("Off"), tr("Forward"), tr("Ping-Pong")})));
    loop->addWidget(knob(QStringLiteral("loop.start"), tr("Start")));
    loop->addWidget(knob(QStringLiteral("loop.end"), tr("End")));
    playbackGrid->addWidget(group(tr("Loop"), loop), 0, 1);

    auto* stretch = knobRow();
    auto* mode = combo(QStringLiteral("stretch.mode"),
                       {tr("Resample"), tr("Stretch"), tr("Loop"),
                        tr("Vocal"), tr("Complex")});
    mode->setToolTip(tr("Stretch: general audio. Loop: repeated phrases. Vocal: voice with formant preservation. Complex: full mixes. These modes keep clip length in beats when BPM changes."));
    stretch->addLayout(choice(tr("Mode"), mode));
    stretch->addWidget(knob(QStringLiteral("stretch.time"), tr("Time")));
    stretch->addWidget(knob(QStringLiteral("stretch.pitch"), tr("Pitch")));
    m_formantKnob = knob(QStringLiteral("formant"), tr("Formant"));
    m_formantKnob->setToolTip(tr("Shifts the vocal character without changing pitch or duration. Vocal mode also preserves formants when pitch changes; Resample uses a tonal tilt."));
    stretch->addWidget(m_formantKnob);
    stretch->addStretch();
    playbackGrid->addWidget(group(tr("Time & pitch"), stretch), 1, 0, 1,
                            m_context == Context::Instrument ? 1 : 2);
    if (m_context == Context::Instrument) {
        auto* tuning = knobRow();
        tuning->addWidget(knob(QStringLiteral("pitch"), tr("Tune")));
        tuning->addWidget(knob(QStringLiteral("pitchrange"), tr("Range")));
        auto* switches = new QVBoxLayout;
        auto* cutItself = led(QStringLiteral("cutitself"), tr("CUT ITSELF"));
        cutItself->setToolTip(tr("A new trigger immediately stops every older voice in this Sampler."));
        switches->addWidget(cutItself);
        switches->addWidget(led(QStringLiteral("keepondisk"), tr("Disk")));
        tuning->addLayout(switches);
        playbackGrid->addWidget(group(tr("Voice"), tuning), 1, 1);
    }
    playback->addLayout(playbackGrid);
    if (m_context == Context::Instrument)
        playback->addWidget(buildTuningSection());
    playback->addStretch();
    addPage(tr("Playback"), playbackPage);
    if (m_context == Context::Instrument) {
        auto* slidePage = new QWidget(pages);
        slidePage->setObjectName("SamplerSlidePage");
        auto* layout = new QVBoxLayout(slidePage);
        layout->setContentsMargins(0,0,0,0);
        layout->setSpacing(8);
        auto* glide = knobRow();
        glide->addWidget(led(QStringLiteral("slide.legato"), tr("Legato")));
        glide->addWidget(knob(QStringLiteral("slide.time"), tr("Legato time")));
        glide->addWidget(led(QStringLiteral("slide.sync"), tr("Sync")));
        auto* divisions = combo(QStringLiteral("slide.beats"),
            {QStringLiteral("1/256"),QStringLiteral("1/128"),QStringLiteral("1/64"),
             QStringLiteral("1/32"),QStringLiteral("1/16"),QStringLiteral("1/8"),
             QStringLiteral("1/4"),QStringLiteral("1/2"),QStringLiteral("1/1")});
        for (int i=0;i<divisions->count();++i) divisions->setItemData(i,std::exp2(i-6.));
        glide->addLayout(choice(tr("Division"), divisions));
        glide->addLayout(choice(tr("Shape"), combo(QStringLiteral("slide.shape"), {tr("Linear"), tr("S-Curve")})));
        layout->addWidget(group(tr("Legato"), glide));
        auto* pitch = knobRow();
        pitch->addWidget(knob(QStringLiteral("slide.smoothing"), tr("Smoothing")));
        auto* bendRange = knob(QStringLiteral("slide.bendrange"), tr("MIDI Bend Range"));
        bendRange->setFixedWidth(132); // Keep the complete range label visible in both locales.
        pitch->addWidget(bendRange);
        auto* continuity = new QVBoxLayout;
        continuity->addLayout(pitch);
        auto* help = new QLabel(tr("Slide notes control each voice independently. Their duration sets the transition time. Legato connects overlapping notes without restarting the sample. MIDI Bend Range applies to incoming Pitch Bend."), slidePage);
        help->setToolTip(help->text());
        help->setText(tr("Slide duration sets the transition; the voice keeps playing.\nLegato connects overlapping notes. MIDI Bend Range affects incoming MIDI."));
        continuity->addWidget(help);
        layout->addWidget(group(tr("Pitch continuity"), continuity));
        addPage(tr("Slide / Legato"), slidePage);
    }

    if (m_context == Context::Instrument)
        addPage(tr("Envelope"), buildEnvelopeSection());

    auto* processingPage = new QWidget(pages);
    auto* processing = new QVBoxLayout(processingPage);
    processing->setContentsMargins(0, 0, 0, 0);
    processing->setSpacing(8);
    auto* toneRow = knobRow();
    auto* tone = knobRow();
    tone->addWidget(knob(QStringLiteral("pre.boost"), tr("Boost")));
    tone->addWidget(knob(QStringLiteral("pre.eq.low"), tr("Low")));
    tone->addWidget(knob(QStringLiteral("pre.eq.mid"), tr("Mid")));
    tone->addWidget(knob(QStringLiteral("pre.eq.high"), tr("High")));
    tone->addStretch();
    toneRow->addWidget(group(tr("Tone"), tone), 1);
    auto* filter = knobRow();
    filter->addWidget(knob(QStringLiteral("pre.cut"), tr("Cutoff")));
    filter->addWidget(knob(QStringLiteral("pre.res"), tr("Resonance")));
    toneRow->addWidget(group(tr("Filter"), filter));
    auto* ring = knobRow();
    ring->addWidget(knob(QStringLiteral("pre.rm.mix"), tr("Mix")));
    ring->addWidget(knob(QStringLiteral("pre.rm.freq"), tr("Frequency")));
    toneRow->addWidget(group(tr("Ring modulation"), ring));
    processing->addLayout(toneRow);

    auto* spaceRow = knobRow();
    auto* space = knobRow();
    space->addLayout(choice(tr("Reverb"), combo(QStringLiteral("pre.rev.type"),
                                                {tr("Room"), tr("Hall")})));
    space->addWidget(knob(QStringLiteral("pre.rev"), tr("Amount")));
    space->addWidget(knob(QStringLiteral("pre.delay"), tr("St Delay")));
    space->addWidget(knob(QStringLiteral("pre.pogo"), tr("Pogo")));
    if (m_context == Context::Instrument) {
        space->addWidget(knob(QStringLiteral("modx"), tr("Mod X")));
        space->addWidget(knob(QStringLiteral("mody"), tr("Mod Y")));
    }
    space->addStretch();
    spaceRow->addWidget(group(tr("Space & modulation"), space), 1);
    processing->addLayout(spaceRow);
    auto* switches = new QGridLayout;
    switches->setHorizontalSpacing(12);
    switches->setVerticalSpacing(0);
    const std::pair<const char*, QString> options[] = {
        {"pre.dc", tr("Remove DC")}, {"pre.polarity", tr("Polarity")},
        {"pre.normalize", tr("Normalize")}, {"pre.fadestereo", tr("Fade Stereo")},
        {"pre.reverse", tr("Reverse")}, {"pre.swap", tr("Swap Stereo")}};
    for (int i = 0; i < 6; ++i)
        switches->addWidget(led(QString::fromLatin1(options[i].first),
                                options[i].second), i / 3, i % 3);
    processing->addLayout(switches);
    processing->addStretch();
    addPage(tr("Processing"), processingPage);

    connect(tabs, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    const int initialPage = qEnvironmentVariableIsSet("DAW_SHOT_SAMPLER_PROCESSING")
                                ? pages->count() - 1
                                : qEnvironmentVariableIsSet("DAW_SHOT_SAMPLER_ENVELOPE") &&
                                          m_context == Context::Instrument ? 1 : 0;
    tabs->setCurrentIndex(initialPage);
    pages->setCurrentIndex(initialPage);
    return host;
}

QWidget* SamplerPanel::buildTuningSection() {
    auto* content = new QVBoxLayout;
    content->setSpacing(7);
    auto* analysis = new QHBoxLayout;
    m_detectPitch = new QPushButton(tr("Detect note"), this);
    m_detectPitch->setObjectName("SamplerDetectPitch");
    m_detectPitch->setMinimumWidth(std::max(m_detectPitch->fontMetrics().horizontalAdvance(tr("Detect note")),
        m_detectPitch->fontMetrics().horizontalAdvance(tr("Analyzing…"))) + 24);
    m_detectPitch->setToolTip(tr("Analyze one sustained note between Start and End, before Tune, Stretch Pitch and modulation. Reference: A = 440 Hz."));
    m_pitchResult = new QLabel(this);
    m_pitchResult->setObjectName("SamplerPitchResult");
    m_pitchResult->setMinimumWidth(0);
    m_pitchResult->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pitchResult->setAccessibleName(tr("Detected sample note"));
    m_pitchResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    analysis->addWidget(m_detectPitch);
    analysis->addWidget(m_pitchResult, 1);
    content->addLayout(analysis);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(7);
    auto* rootLabel = new QLabel(tr("Root"), this);
    QStringList notes;
    for (int note = 0; note < 128; ++note)
        notes.append(QString::fromStdString(sampler::parameterText(
            sampler::indexOf(sampler::Param::RootNote), note)));
    auto* root = combo(QStringLiteral("rootnote"), notes);
    root->setFixedWidth(74);
    root->setAccessibleName(tr("Root note"));
    rootLabel->setBuddy(root);
    actions->addWidget(rootLabel);
    actions->addWidget(root);

    auto* fineLabel = new QLabel(tr("Fine tune"), this);
    m_fineTune = new QDoubleSpinBox(this);
    m_fineTune->setObjectName("SamplerParameter.finepitch");
    m_fineTune->setRange(-100, 100);
    m_fineTune->setDecimals(1);
    m_fineTune->setSingleStep(.1);
    m_fineTune->setSuffix(tr(" ct"));
    m_fineTune->setKeyboardTracking(false);
    m_fineTune->setFixedWidth(100);
    m_fineTune->setMinimumHeight(26);
    m_fineTune->setAccessibleName(tr("Fine tune in cents"));
    m_fineTune->setToolTip(tr("One semitone is 100 cents. Fine Tune adds to Tune independently of its Range."));
    m_fineTune->installEventFilter(this);
    fineLabel->setBuddy(m_fineTune);
    actions->addWidget(fineLabel);
    actions->addWidget(m_fineTune);
    actions->addStretch();
    m_applyRoot = new QPushButton(tr("Set root"), this);
    m_applyRoot->setObjectName("SamplerApplyRoot");
    m_applyRoot->setToolTip(tr("Assign the detected note as the key that plays the sample without transposition."));
    m_correctTuning = new QPushButton(tr("Correct tuning"), this);
    m_correctTuning->setObjectName("SamplerCorrectTuning");
    m_correctTuning->setToolTip(tr("Set Fine Tune to cancel the detected cents offset. Tune and Stretch Pitch keep their values."));
    actions->addWidget(m_applyRoot);
    actions->addWidget(m_correctTuning);
    content->addLayout(actions);
    connect(m_detectPitch, &QPushButton::clicked, this, &SamplerPanel::detectPitch);
    connect(m_applyRoot, &QPushButton::clicked, this, [this] { applyDetectedPitch(true); });
    connect(m_correctTuning, &QPushButton::clicked, this, [this] { applyDetectedPitch(false); });
    connect(m_fineTune, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        beginGesture(QStringLiteral("finepitch"));
        writeParameter(QStringLiteral("finepitch"), value);
    });
    connect(m_fineTune, &QDoubleSpinBox::editingFinished, this, [this] {
        endGesture(QStringLiteral("finepitch"));
        refresh();
    });
    return sectionBox(tr("Note & tuning"), content, this);
}

void SamplerPanel::refreshPitchAnalysis() {
    if (!m_detectPitch) return;
    auto data = currentSample();
    const double start = readParameter(QStringLiteral("startoffset"));
    const double end = readParameter(QStringLiteral("endoffset"));
    if (data != m_pitchSample || start != m_pitchStart || end != m_pitchEnd) {
        if (m_pitchCancelled) m_pitchCancelled->store(true);
        ++m_pitchGeneration;
        m_pitchSample = data;
        m_pitchStart = start; m_pitchEnd = end;
        m_pitchBusy = m_pitchAnalyzed = false;
        m_pitchEstimate = {};
    }
    const auto snapshot = sampler();
    const bool sourceReady = snapshot.available && data && data->audio && data->baseFrames &&
        !snapshot.precomputePending;
    const bool editable = m_controller && m_controller->sharedEditingAllowed() && snapshot.available;
    m_detectPitch->setEnabled(sourceReady && !m_pitchBusy);
    m_detectPitch->setText(m_pitchBusy ? tr("Analyzing…") : tr("Detect note"));
    const bool detected = sourceReady && !m_pitchBusy && m_pitchAnalyzed &&
        m_pitchEstimate.status == daw::analysis::SamplePitchStatus::Detected;
    m_applyRoot->setEnabled(editable && detected &&
        readParameter(QStringLiteral("rootnote")) != m_pitchEstimate.midiNote);
    const double correction = std::round(-m_pitchEstimate.cents * 10) / 10;
    m_correctTuning->setEnabled(editable && detected &&
        std::abs(readParameter(QStringLiteral("finepitch")) - correction) > .049);
    m_fineTune->setEnabled(editable);
    m_combos.value(QStringLiteral("rootnote"))->setEnabled(editable);
    if (!m_fineTune->hasFocus() && !m_fineTune->isAncestorOf(QApplication::focusWidget())) {
        const QSignalBlocker blocker(m_fineTune);
        m_fineTune->setValue(readParameter(QStringLiteral("finepitch")));
    }
    QString message;
    if (m_pitchBusy) message = tr("Analyzing the selected sample region…");
    else if (!sourceReady) message = data ? tr("Waiting for sample processing…") : tr("Load a sample to detect its note.");
    else if (!m_pitchAnalyzed) message = tr("Detect the note of a sustained sound.");
    else if (detected) {
        const QString note = QString::fromStdString(sampler::parameterText(
            sampler::indexOf(sampler::Param::RootNote), m_pitchEstimate.midiNote));
        const double rounded = std::round(m_pitchEstimate.cents * 10) / 10;
        const QString cents = (rounded > 0 ? QStringLiteral("+") : QString()) + QString::number(rounded, 'f', 1);
        message = tr("Source: %1 · %2 Hz · %3 cents").arg(note)
            .arg(m_pitchEstimate.frequencyHz, 0, 'f', 2).arg(cents);
    } else if (m_pitchEstimate.status == daw::analysis::SamplePitchStatus::TooShort)
        message = tr("The region is too short. Select more of the note.");
    else if (m_pitchEstimate.status == daw::analysis::SamplePitchStatus::Unstable)
        message = tr("Pitch varies. Select a steady part of one note.");
    else message = tr("No stable tone found. Try a tonal sample.");
    m_pitchResult->setText(message);
    m_pitchResult->setToolTip(message);
}

void SamplerPanel::detectPitch() {
    refreshPitchAnalysis();
    if (!m_detectPitch || !m_detectPitch->isEnabled()) return;
    const auto sample = m_pitchSample;
    const auto frames = std::min(sample->baseFrames, sample->audio->frames());
    const auto first = daw::engine::FrameCount(std::clamp(m_pitchStart, 0.0, 1.0) * frames);
    const auto end = daw::engine::FrameCount(std::clamp(m_pitchEnd, 0.0, 1.0) * frames);
    if (m_pitchCancelled) m_pitchCancelled->store(true);
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    m_pitchCancelled = cancelled;
    const auto generation = ++m_pitchGeneration;
    m_pitchBusy = true;
    refreshPitchAnalysis();
    const QPointer<SamplerPanel> guard(this);
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(1); pool.setThreadPriority(QThread::LowPriority);
        pool.setExpiryTimeout(5000); return true;
    }();
    Q_UNUSED(configured);
    pool.start([sample, first, end, cancelled, generation, guard] {
        daw::analysis::SamplePitchEstimate result;
        try {
            result = daw::analysis::detectSamplePitch(*sample->audio, first, end,
                [cancelled] { return !cancelled->load(); });
        } catch (const std::exception&) {
            // Keep allocation/read failures on the worker from escaping into Qt.
        }
        QMetaObject::invokeMethod(qApp, [guard, generation, result] {
            if (!guard) return;
            guard->refreshPitchAnalysis(); // Validate source/region even while hidden.
            if (guard->m_pitchGeneration != generation) return;
            guard->m_pitchBusy = false;
            guard->m_pitchAnalyzed = true;
            guard->m_pitchEstimate = result;
            guard->refreshPitchAnalysis();
        }, Qt::QueuedConnection);
    });
}

void SamplerPanel::applyDetectedPitch(bool setRoot) {
    refreshPitchAnalysis();
    if (!(setRoot ? m_applyRoot : m_correctTuning)->isEnabled()) return;
    const QString id = setRoot ? QStringLiteral("rootnote") : QStringLiteral("finepitch");
    const double value = setRoot ? double(m_pitchEstimate.midiNote) :
        std::round(-m_pitchEstimate.cents * 10) / 10;
    const double before = readParameter(id);
    writeParameter(id, value);
    m_controller->commitInsertParameterEdit(m_channelId.toStdString(), m_slotId.toStdString(),
        id.toStdString(), before, setRoot ? "Set Sampler Root Note" : "Correct Sampler Tuning");
    emit projectEdited();
    refresh();
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
    m_waveform->setFixedHeight(116);
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
    return sectionBox(QString(), content, this);
}

// ── Refresh ──

void SamplerPanel::refresh() {
    if (m_controller) {
        QColor color = th().accent;
        if (const auto* track = m_controller->project().findTrack(m_channelId.toStdString()))
            color = colorFromRgb(track->color);
        if (m_context == Context::Clip) {
            if (const auto* clip = m_controller->audioClip(m_channelId.toStdString(), m_slotId.toStdString());
                clip && clip->muted)
                color = QColor(101, 105, 113);
        }
        m_waveform->setClipColor(color);
    }
    if (m_offlineHistory && m_controller) {
        const auto* clip = m_controller->audioClip(m_channelId.toStdString(), m_slotId.toStdString());
        const bool hasHistory = clip && (!clip->offlineHistory.empty() || !clip->offlineProcess.empty());
        m_offlineHistory->setEnabled(hasHistory && m_controller->sharedEditingAllowed() &&
                                     !m_controller->offlineRenderInProgress());
        QString caption = tr("History ▾");
        if (clip && !clip->offlineHistory.empty()) {
            const auto active = std::find_if(clip->offlineHistory.begin(), clip->offlineHistory.end(),
                [&](const auto& version) { return version.id == clip->offlineVersionId; });
            if (active != clip->offlineHistory.end())
                caption = tr("History · %1 ▾").arg(std::distance(clip->offlineHistory.begin(), active));
        }
        m_offlineHistory->setText(caption);
    }
    const auto snapshot = sampler();
    std::shared_ptr<const sampler::SampleData> data = currentSample();
    refreshPitchAnalysis();

    if (snapshot.available || m_context == Context::Clip) {
        std::string path;
        std::string name;
        if (snapshot.available) {
            path = snapshot.path;
            name = snapshot.name;
        } else if (const daw::ClipModel* clip = m_controller->audioClip(
                       m_channelId.toStdString(), m_slotId.toStdString())) {
            path = clip->filePath;
            name = clip->name.empty() ? QFileInfo(QString::fromStdString(path))
                                            .fileName().toStdString()
                                      : clip->name;
        }
        m_fileLabel->setToolTip(QString::fromStdString(path));
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
        const double value = readParameter(it.key());
        int index = int(std::lround(value));
        if (it.value()->itemData(0).isValid()) {
            index = 0;
            for (int i=1;i<it.value()->count();++i)
                if (std::abs(it.value()->itemData(i).toDouble()-value) <
                    std::abs(it.value()->itemData(index).toDouble()-value)) index=i;
        }
        if (it.value()->currentIndex() == index) continue;
        const QSignalBlocker block(it.value());
        it.value()->setCurrentIndex(index);
    }

    if (auto* slidePage = findChild<QWidget*>("SamplerSlidePage")) {
        const bool available = m_controller && m_controller->canEditSlideNotes();
        slidePage->setEnabled(available);
        auto* tabs = findChild<QTabBar*>("SamplerToolsTabs");
        tabs->setTabEnabled(1, available);
        tabs->setTabToolTip(1, available ? QString() : tr("Slide editing requires collaboration protocol 5. Reconnect to an updated session."));
    }
    if (m_context == Context::Instrument && m_knobs.contains("slide.time")) {
        const bool sync = readParameter("slide.sync") >= .5;
        m_knobs.value("slide.time")->setEnabled(!sync);
        m_combos.value("slide.beats")->setEnabled(sync);
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
        const int root = int(std::lround(readParameter(QStringLiteral("rootnote"))));
        if (m_keyboard->root() != root) {
            m_keyboard->setRoot(root);
            m_keyboardScroll->ensureVisible(m_keyboard->xForPitch(root), 0, 24, 0);
        }
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
                const auto levels = m_controller->meterSnapshot(m_channelId.toStdString(), {}, true);
                peakLeft = levels.left;
                peakRight = levels.right;
            }
        } else if (const daw::ClipModel* clip = m_controller->audioClip(
                       m_channelId.toStdString(), m_slotId.toStdString())) {
            inserts = &clip->inserts;
            volume = clip->gain;
            pan = clip->pan;
            const auto levels = m_controller->meterSnapshot(m_channelId.toStdString(), m_slotId.toStdString());
            peakLeft = levels.left;
            peakRight = levels.right;
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
    const QPointer<SamplerPanel> guard(this);
    if (!ui::prepareAudioImport(this, *m_controller, path) || !guard) return;
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
    if (const auto snapshot = sampler(); snapshot.available) {
        path = QString::fromStdString(snapshot.path);
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


bool SamplerWaveform::checkPeakUpdatesForTest() {
    SamplerWaveform waveform;
    waveform.resize(320, 116);
    const auto sample = [](float value, const char* path = "same.wav") {
        auto result = std::make_shared<sampler::SampleData>();
        auto audio = std::make_shared<daw::engine::SampleBuffer>(1, 4096, 48000);
        std::fill_n(audio->writableChannel(0), audio->frames(), value);
        result->audio = audio;
        result->baseFrames = audio->frames();
        result->path = path;
        return result;
    };
    const auto wait = [&] {
        QElapsedTimer time; time.start();
        while (waveform.m_peakBuildBusy && time.elapsed() < 3000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        return !waveform.m_peakBuildBusy;
    };
    const auto raster = [&] {
        QImage image(waveform.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        waveform.paintScene(painter, waveform.rect());
        return image;
    };
    auto cache = std::make_shared<ui::graphics::SceneRecordingCache>();
    const auto geometry = [&] {
        ui::graphics::SceneRecorder recorder(waveform.size(), 1, cache);
        { QPainter painter(&recorder); waveform.paintScene(painter, waveform.rect()); }
        QVector<ui::graphics::SceneVertex> vertices;
        for (const auto& mesh : cache->sections.at(1002))
            if (mesh.color == QColor(Qt::white)) vertices += mesh.vertices;
        return vertices;
    };
    waveform.setSample(sample(.1f));
    if (!wait() || waveform.m_minima.isEmpty()) return false;
    const auto before = raster();
    const auto beforeGeometry = geometry();
    waveform.setSample(sample(.8f));
    if (waveform.m_minima.isEmpty() || raster() != before || geometry() != beforeGeometry)
        return false; // No blank frame while a replacement is being prepared.
    if (!wait() || raster() == before || geometry() == beforeGeometry) return false;
    waveform.setSample(sample(.4f));
    waveform.setSample(sample(.2f));
    if (!wait() || waveform.m_maxima.isEmpty() || waveform.m_maxima.front() != .2f) return false;
    const auto beforeTail = raster();
    const auto beforeTailGeometry = geometry();
    const double markerBeforeTail = waveform.xForFraction(1.0);
    auto tail = sample(.6f);
    auto tailAudio = std::make_shared<daw::engine::SampleBuffer>(1, 8192, 48000);
    std::fill_n(tailAudio->writableChannel(0), tailAudio->frames(), .6f);
    tail->audio = tailAudio; // Reverb extends total frames, not marker base frames.
    waveform.setSample(tail);
    waveform.setSample(tail); // Ordinary panel polling during the peak rebuild.
    if (waveform.xForFraction(1.0) != markerBeforeTail || raster() != beforeTail ||
        geometry() != beforeTailGeometry) return false;
    if (!wait() || waveform.xForFraction(1.0) != markerBeforeTail * .5 ||
        raster() == beforeTail || geometry() == beforeTailGeometry) return false;
    waveform.setSample(sample(.5f, "different.wav"));
    if (!waveform.m_minima.isEmpty()) return false;
    waveform.setSample({});
    if (!wait() || !waveform.m_minima.isEmpty()) return false;
    std::fprintf(stderr, "PASS Sampler waveform: retained CPU/GPU envelope, atomic publication, stale result rejection, clear during rebuild\n");
    return true;
}

bool SamplerPanel::checkLayoutForTest() {
    if (!AudioEditPanel::checkForTest()) return false;
    if (!SamplerWaveform::checkPeakUpdatesForTest()) return false;
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto descriptor = controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.sampler");
    if (!descriptor) return false;
    const auto track = controller.addTrack(daw::TrackKind::Instrument, "Sampler UI check");
    controller.setTrackInstrumentPlugin(track, *descriptor);
    const auto slot = controller.project().findTrack(track)->instrument.id;
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "sampler UI: %s\n", message);
        ok &= condition;
    };
    const auto wheel = [](QWidget* widget) {
        const QPointF at(widget->rect().center());
        QWheelEvent event(at, widget->mapToGlobal(at.toPoint()), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(widget, &event);
    };
    for (const Context context : {Context::Instrument, Context::Clip}) {
        // The Clip shell also needs to fit while its file/target is missing.
        SamplerPanel panel(&controller, context, QString::fromStdString(track),
                           QString::fromStdString(slot));
        auto* primaryTabs = panel.findChild<QTabWidget*>("SamplerTabs");
        check(primaryTabs && primaryTabs->currentIndex() == (context == Context::Clip ? 1 : 0),
              "clip opens editor, instrument opens sampler");
        primaryTabs->setCurrentIndex(0);
        panel.show();
        auto* tabs = panel.findChild<QTabBar*>("SamplerToolsTabs");
        auto* scroll = panel.findChild<QScrollArea*>("SamplerBodyScroll");
        auto* pages = panel.findChild<QStackedWidget*>("SamplerToolsPages");
        auto* keyboard = panel.findChild<QToolButton*>("SamplerCollapse");
        auto* fxStrip = panel.findChild<QWidget*>("SamplerFxStrip");
        // Switch palettes while this editor remains open, at both supported
        // sizes. QSS lighting must not change layout or retain stale surfaces.
        for (const Theme& preset : ThemeManager::instance().presets()) {
            ThemeManager::instance().setThemeId(preset.id, false);
            for (const QSize size : {QSize(1040, 722), QSize(960, 562), QSize(860, 520)}) {
                panel.resize(size);
                const bool fullSize = size == QSize(1040, 722);
                keyboard->setChecked(fullSize);
                for (int index = 0; index < tabs->count(); ++index) {
                    tabs->setCurrentIndex(index);
                    QApplication::processEvents();
                    scroll->verticalScrollBar()->setValue(0);
                    if (context == Context::Instrument && tabs->tabText(index) == tr("Slide / Legato") &&
                        qEnvironmentVariableIsSet("DAW_SLIDE_CHECK_DIR"))
                        panel.grab().save(qEnvironmentVariable("DAW_SLIDE_CHECK_DIR") + "/sampler-" + preset.id + ".png");
                    check(scroll->horizontalScrollBar()->maximum() == 0,
                          "horizontal overflow at supported editor size");
                    if (fullSize) {
                        check(scroll->verticalScrollBar()->maximum() == 0,
                              "settings and expanded keyboard do not fit the default window");
                        check(panel.m_keyboardScroll->viewport()->height() >= panel.m_keyboard->height(),
                              "the keyboard scrollbar clips the bottom of the keys");
                    }
                    check(fxStrip->mapTo(&panel, QPoint()).y() ==
                              panel.m_waveform->parentWidget()->mapTo(&panel, QPoint()).y(),
                          "FX strip and waveform section have different top margins");
                    int controlsBottom = 0;
                    for (QWidget* widget : pages->currentWidget()->findChildren<QWidget*>()) {
                        if (!widget->isVisible() ||
                            (widget->objectName() != QLatin1String("SamplerSection") &&
                             !widget->objectName().startsWith("SamplerParameter.")))
                            continue;
                        controlsBottom = std::max(controlsBottom,
                            widget->mapTo(scroll->widget(), QPoint(0, widget->height())).y());
                    }
                    const int keyboardGap = keyboard->parentWidget()->mapTo(scroll->widget(), QPoint()).y() - controlsBottom;
                    check(keyboardGap >= 0 && keyboardGap <= 12,
                          "hidden settings pages leave a gap before the keyboard");
                    // A hidden scrollbar cannot disguise a clipped child.
                    for (QWidget* control : pages->currentWidget()->findChildren<QWidget*>()) {
                        if (!control->isVisible() || !control->objectName().startsWith("SamplerParameter."))
                            continue;
                        const QRect bounds(control->mapTo(scroll->widget(), QPoint()), control->size());
                        check(scroll->widget()->rect().contains(bounds), "parameter extends outside its page");
                        for (QWidget* parent = control->parentWidget(); parent && parent != pages;
                             parent = parent->parentWidget()) {
                            check(parent->rect().contains(QRect(control->mapTo(parent, QPoint()), control->size())),
                                  "parameter clipped by a control group");
                        }
                    }
                    check(panel.m_waveform->isVisible(), "waveform disappeared when changing settings tabs");
                }
            }
        }
        tabs->setCurrentIndex(0);
        const double before = panel.readParameter("stretch.pitch");
        ui::Knob* pitch = panel.m_knobs.value("stretch.pitch");
        wheel(pitch);
        check(panel.readParameter("stretch.pitch") == before && pitch->value() == before,
              "scrolling over a dial changes the parameter");
        auto* mode = panel.m_combos.value("stretch.mode");
        const int modeBefore = mode->currentIndex();
        wheel(mode);
        check(mode->currentIndex() == modeBefore, "scrolling changes playback mode");
        if (context == Context::Instrument) {
            panel.resize(1040, 722);
            keyboard->setChecked(true);
            QApplication::processEvents();
            auto* root = panel.m_combos.value(QStringLiteral("rootnote"));
            for (const int note : {1, 117, 127}) {
                root->setCurrentIndex(note);
                const QPoint key(panel.m_keyboard->xForPitch(note) -
                    panel.m_keyboardScroll->horizontalScrollBar()->value(), 20);
                check(panel.readParameter("rootnote") == note && panel.m_keyboard->root() == note &&
                      panel.m_keyboardScroll->viewport()->rect().contains(key),
                      "Root Note selection does not immediately reveal the matching keyboard key");
            }
            controller.undo(); panel.refresh();
            check(root->currentIndex() == 117 && panel.m_keyboard->root() == 117,
                  "Root Note undo does not synchronize the field and keyboard");
            controller.undo(); controller.undo(); panel.refresh();
            check(root->currentIndex() == 60 && panel.m_keyboard->root() == 60,
                  "Root Note edits do not restore the original key");
            const QPointF rootKey(panel.m_keyboard->xForPitch(61), 20);
            QMouseEvent rootPress(QEvent::MouseButtonPress, rootKey,
                panel.m_keyboard->mapToGlobal(rootKey.toPoint()), Qt::RightButton,
                Qt::RightButton, Qt::NoModifier);
            const auto rootDepth = controller.undoDepth();
            QApplication::sendEvent(panel.m_keyboard, &rootPress);
            check(panel.readParameter("rootnote") == 61 && root->currentIndex() == 61 &&
                  panel.m_keyboard->root() == 61 && controller.undoDepth() == rootDepth + 1,
                  "keyboard root assignment is delayed or creates multiple undo steps");
            controller.undo(); panel.refresh();
            check(!panel.m_detectPitch->isEnabled() && !panel.m_applyRoot->isEnabled() &&
                  !panel.m_correctTuning->isEnabled(), "missing sample enables detection/application");
            wheel(panel.m_fineTune);
            check(panel.readParameter("finepitch") == 0, "scrolling changes Fine Tune");
            panel.m_fineTune->setValue(12.3);
            QMetaObject::invokeMethod(panel.m_fineTune, "editingFinished", Qt::DirectConnection);
            check(panel.readParameter("finepitch") == 12.3, "numeric Fine Tune input is not applied");
            controller.undo(); panel.refresh();
            check(panel.readParameter("finepitch") == 0, "Fine Tune numeric edit does not undo once");

            const QPoint start(pitch->rect().center());
            const QPoint end = start - QPoint(0, 18);
            const auto mouse = [pitch](QEvent::Type type, QPoint point, Qt::MouseButton button,
                                       Qt::MouseButtons buttons) {
                QMouseEvent event(type, point, pitch->mapToGlobal(point), button, buttons, Qt::NoModifier);
                QApplication::sendEvent(pitch, &event);
            };
            mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
            const double changed = panel.readParameter("stretch.pitch");
            panel.refresh();
            check(changed > before && pitch->isEditing() && pitch->value() == changed,
                  "drag/refresh loses the live parameter value");
            mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
            controller.undo();
            panel.refresh();
            check(panel.readParameter("stretch.pitch") == before,
                  "one drag is not restored by one undo");

            auto tone = std::make_shared<daw::engine::SampleBuffer>(1, 48000, 48000);
            const double hz = 440 * std::exp2(18.2 / 1200);
            for (daw::engine::FrameCount i = 0; i < tone->frames(); ++i)
                tone->writableChannel(0)[i] = float(.4 * std::sin(6.283185307179586 * hz * i / 48000));
            auto* live = controller.samplerInstance(track, slot);
            live->adoptSample("synthetic-tuner.wav", tone);
            live->flushPendingPrecompute();
            panel.refresh();
            const auto waitForPitch = [&] {
                QElapsedTimer timer; timer.start();
                while (panel.m_pitchBusy && timer.elapsed() < 5000) {
                    QApplication::processEvents(); QThread::msleep(1);
                }
            };
            const auto depth = controller.undoDepth();
            panel.m_detectPitch->click();
            check(panel.m_pitchBusy && !panel.m_applyRoot->isEnabled(), "analysis does not expose a busy state");
            waitForPitch();
            check(panel.m_pitchEstimate.status == daw::analysis::SamplePitchStatus::Detected &&
                  panel.m_pitchEstimate.midiNote == 69 && panel.m_applyRoot->isEnabled() &&
                  panel.m_correctTuning->isEnabled() && controller.undoDepth() == depth,
                  "background detection changes the project or loses its result");
            panel.m_applyRoot->click();
            check(panel.readParameter("rootnote") == 69 && panel.m_keyboard->root() == 69 &&
                  root->currentIndex() == 69 && !panel.m_applyRoot->isEnabled(),
                  "root assignment fails or is not idempotent");
            controller.undo(); panel.refresh();
            check(panel.readParameter("rootnote") == 60, "root assignment does not undo once");
            controller.redo(); panel.refresh();
            panel.m_correctTuning->click();
            check(std::abs(panel.readParameter("finepitch") + 18.2) < .11 &&
                  !panel.m_correctTuning->isEnabled(), "tuning correction has the wrong sign or is not idempotent");
            controller.undo(); panel.refresh();
            check(panel.readParameter("finepitch") == 0, "tuning correction does not undo once");
            controller.redo(); panel.refresh();
            const auto correction = panel.readParameter("finepitch");
            if (qEnvironmentVariableIsSet("DAW_TUNING_CHECK_DIR")) {
                const auto previousTheme = th().id;
                for (const QString& theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                    ThemeManager::instance().setThemeId(theme, false);
                    QApplication::processEvents();
                    panel.grab().save(qEnvironmentVariable("DAW_TUNING_CHECK_DIR") +
                        "/tuning-" + theme + "-" + QLocale().name() + ".png");
                }
                ThemeManager::instance().setThemeId(previousTheme, false);
            }
            panel.m_detectPitch->click();
            panel.writeParameter("startoffset", .2);
            panel.refresh(); waitForPitch();
            check(!panel.m_pitchAnalyzed && !panel.m_applyRoot->isEnabled() &&
                  !panel.m_correctTuning->isEnabled() && panel.readParameter("finepitch") == correction,
                  "a changed region applies a stale result");
            panel.m_detectPitch->click();
            live->clearSample(); panel.refresh();
            QApplication::processEvents();
            check(!panel.m_pitchAnalyzed && !panel.m_detectPitch->isEnabled(),
                  "clearing a sample retains an applicable detection");
        }
        keyboard->setChecked(true);
        QApplication::processEvents();
        check(panel.m_keyboard->isVisible() && scroll->horizontalScrollBar()->maximum() == 0,
              "keyboard disclosure breaks the compact page");
    }
    if (ok) std::fprintf(stderr, "PASS Sampler: full settings and keyboard fit at 1040 px, compact 960/860 px layouts, immediate Root Note synchronization, note detection, tuning, wheel protection, drag and undo\n");
    return ok;
}
