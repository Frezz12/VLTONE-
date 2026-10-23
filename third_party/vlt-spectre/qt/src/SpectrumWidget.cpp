// SPDX-License-Identifier: MIT
#include <vlt/spectre/SpectrumWidget.hpp>
#include <vlt/spectre/DisplaySmoothing.hpp>
#include <QHideEvent>
#include <QShowEvent>

#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace vlt::spectre {
namespace {
QColor traceLine(const TraceStyle& style, const SpectrumTheme& theme) {
    return style.line.isValid() ? style.line : theme.background.lightness() > 128
        ? QColor("#536b79") : QColor("#d2dce0");
}
QColor traceFill(const TraceStyle& style, const SpectrumTheme& theme) {
    return style.fill.isValid() ? style.fill : theme.background.lightness() > 128
        ? QColor(77, 116, 139, 80) : QColor(175, 196, 207, 95);
}
QString frequencyText(double frequency) {
    if (frequency >= 1000.0)
        return QStringLiteral("%1 kHz").arg(frequency / 1000.0, 0, 'f', frequency < 10000.0 ? 2 : 1);
    return QStringLiteral("%1 Hz").arg(frequency, 0, 'f', frequency < 100.0 ? 1 : 0);
}
QString noteText(double frequency) {
    static const char* names[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int midi = int(std::lround(69.0 + 12.0 * std::log2(frequency / 440.0)));
    const int index = ((midi % 12) + 12) % 12;
    const int octave = int(std::floor(double(midi) / 12.0)) - 1;
    return QString::fromLatin1(names[index]) + QString::number(octave);
}
float levelAt(const SpectrumFrame& frame, double frequency) {
    if (frame.frequenciesHz.empty() || frame.levelsDb.size() != frame.frequenciesHz.size()) return -120.0f;
    const auto found = std::lower_bound(frame.frequenciesHz.begin(), frame.frequenciesHz.end(), float(frequency));
    if (found == frame.frequenciesHz.begin()) return frame.levelsDb.front();
    if (found == frame.frequenciesHz.end()) return frame.levelsDb.back();
    const auto high = std::size_t(found - frame.frequenciesHz.begin());
    const auto low = high - 1;
    const double t = std::log(frequency / frame.frequenciesHz[low]) /
                     std::log(frame.frequenciesHz[high] / frame.frequenciesHz[low]);
    return float(frame.levelsDb[low] + (frame.levelsDb[high] - frame.levelsDb[low]) * t);
}

// Monotonicity matters for a measurement graph. An unconstrained Catmull-Rom
// spline can draw peaks and dips that never existed. Limit each pair of tangents
// to the segment's slope (Fritsch-Carlson bound) before converting to Beziers.
QPainterPath monotonePath(const QPolygonF& points) {
    QPainterPath path;
    if (points.empty()) return path;
    path.moveTo(points.front());
    if (points.size() < 2) return path;
    std::vector<double> slopes(std::size_t(points.size() - 1));
    std::vector<double> tangents(std::size_t(points.size()));
    for (qsizetype i = 0; i + 1 < points.size(); ++i)
        slopes[std::size_t(i)] = (points[i + 1].y() - points[i].y()) / (points[i + 1].x() - points[i].x());
    tangents.front() = slopes.front(); tangents.back() = slopes.back();
    for (std::size_t i = 1; i + 1 < tangents.size(); ++i)
        tangents[i] = slopes[i - 1] * slopes[i] > 0 ? (slopes[i - 1] + slopes[i]) * 0.5 : 0;

    // Store ONE tangent per knot. Limiting each segment independently gives the
    // same knot two different slopes, which was the source of visible corners.
    for (std::size_t i = 0; i < slopes.size(); ++i) {
        if (std::abs(slopes[i]) < 1e-12) { tangents[i] = tangents[i + 1] = 0; continue; }
        const double magnitude = std::hypot(tangents[i] / slopes[i], tangents[i + 1] / slopes[i]);
        if (magnitude > 3.0) {
            tangents[i] *= 3.0 / magnitude;
            tangents[i + 1] *= 3.0 / magnitude;
        }
    }
    for (qsizetype i = 0; i + 1 < points.size(); ++i) {
        const double third = (points[i + 1].x() - points[i].x()) / 3.0;
        path.cubicTo(points[i] + QPointF(third, third * tangents[std::size_t(i)]),
                     points[i + 1] - QPointF(third, third * tangents[std::size_t(i + 1)]), points[i + 1]);
    }
    return path;
}

bool validView(const SpectrumView& view) {
    return std::isfinite(view.minimumFrequency) && std::isfinite(view.maximumFrequency) &&
           view.minimumFrequency >= 1.0 && view.maximumFrequency > view.minimumFrequency &&
           view.maximumFrequency <= 384000.0 && std::isfinite(view.minimumDb) &&
           std::isfinite(view.maximumDb) && view.minimumDb >= -200.0 && view.maximumDb <= 60.0 &&
           view.maximumDb > view.minimumDb && std::isfinite(view.tiltDbPerOctave) &&
           std::abs(view.tiltDbPerOctave) <= 12.0 &&
           std::isfinite(view.highFrequencyLiftDb) && std::abs(view.highFrequencyLiftDb) <= 36.0 &&
           std::isfinite(view.highFrequencyStartHz) && view.highFrequencyStartHz >= 1.0 &&
           std::isfinite(view.highFrequencyEndHz) && view.highFrequencyEndHz > view.highFrequencyStartHz &&
           view.highFrequencyEndHz <= 384000.0 && std::isfinite(view.smoothingOctaves) &&
           view.smoothingOctaves >= 0 && view.smoothingOctaves <= 1 &&
           std::isfinite(view.transitionMs) && view.transitionMs >= 0 && view.transitionMs <= 2000;
}
} // namespace

SpectrumTheme SpectrumTheme::dark() {
    return {QColor("#171b1e"), QColor("#252b30"), QColor("#384149"),
            QColor("#e6ecef"), QColor("#a0adb7"), QColor("#dfc589"), QColor("#242b31")};
}
SpectrumTheme SpectrumTheme::light() {
    return {QColor("#f6f7f7"), QColor("#e5e8e9"), QColor("#c9d0d4"),
            QColor("#263039"), QColor("#586772"), QColor("#8a641f"), QColor("#ffffff")};
}

double displayWeightingDb(double frequency, const SpectrumView& view) {
    if (!validView(view) || !std::isfinite(frequency) || frequency <= 0) return 0;
    const double fraction = std::clamp(std::log(frequency / view.highFrequencyStartHz) /
        std::log(view.highFrequencyEndHz / view.highFrequencyStartHz), 0.0, 1.0);
    const double shelf = fraction * fraction * (3.0 - 2.0 * fraction);
    return view.tiltDbPerOctave * std::log2(frequency / 1000.0) + view.highFrequencyLiftDb * shelf;
}

QPainterPath spectrumPath(const SpectrumFrame& frame, const QRectF& plot,
                          const SpectrumView& view, bool peaks) {
    const auto& measuredLevels = peaks ? frame.peaksDb : frame.levelsDb;
    const auto levels = smoothDisplaySpectrum(frame.frequenciesHz.data(), measuredLevels.data(),
        std::min(frame.frequenciesHz.size(), measuredLevels.size()), view.smoothingOctaves);
    if (!frame.ready || levels.size() < 2 || levels.size() != frame.frequenciesHz.size() ||
        !validView(view) || plot.width() <= 0 || plot.height() <= 0) return {};
    QPolygonF points;
    points.reserve(qsizetype(levels.size()));
    const double span = std::log(view.maximumFrequency / view.minimumFrequency);
    double previousFrequency = 0.0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const double frequency = frame.frequenciesHz[i];
        if (!std::isfinite(frequency) || frequency <= previousFrequency) return {};
        previousFrequency = frequency;
        const double measured = std::isfinite(levels[i]) ? levels[i] : view.minimumDb;
        const double tilted = measured + displayWeightingDb(frequency, view);
        const double x = plot.left() + std::log(frequency / view.minimumFrequency) / span * plot.width();
        const double y = plot.bottom() - (std::clamp(tilted, view.minimumDb, view.maximumDb) -
                         view.minimumDb) / (view.maximumDb - view.minimumDb) * plot.height();
        points.push_back({x, y});
    }
    return monotonePath(points);
}

SpectrumWidget::SpectrumWidget(QWidget* parent) : QWidget(parent) {
    m_transitionTimer.setTimerType(Qt::PreciseTimer);
    m_transitionTimer.setInterval(16);
    connect(&m_transitionTimer, &QTimer::timeout, this, &SpectrumWidget::advanceTransition);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(tr("Frequency spectrum"));
    setAccessibleDescription(tr("Frequency and amplitude graph. Space freezes the display; "
        "arrow keys inspect frequencies; plus and minus zoom; Home resets the zoom."));
    setToolTip(tr("Wheel: zoom around cursor · Middle drag: pan · Double-click: reset\n"
                  "Space: freeze · Arrow keys: inspect · Right-click: display options"));
}

bool SpectrumWidget::setTrace(const QString& id, const SpectrumFrame& frame, const TraceStyle& style) {
    if (m_frozen) return false;
    if (frame.ready && frame.frequenciesHz.size() < 2) return false;
    if (frame.levelsDb.size() != frame.frequenciesHz.size() ||
        (!frame.peaksDb.empty() && frame.peaksDb.size() != frame.frequenciesHz.size())) return false;
    float previous = 0;
    for (const auto frequency : frame.frequenciesHz) {
        if (!std::isfinite(frequency) || frequency <= previous) return false;
        previous = frequency;
    }
    for (const auto value : frame.levelsDb) if (!std::isfinite(value)) return false;
    for (const auto value : frame.peaksDb) if (!std::isfinite(value)) return false;
    auto found = std::find_if(m_traces.begin(), m_traces.end(), [&](const Trace& t) { return t.id == id; });
    if (found == m_traces.end()) {
        Trace trace;
        trace.id = id; trace.frame = frame; trace.style = style;
        m_traces.push_back(std::move(trace));
        found = m_traces.end() - 1;
    } else { found->frame = frame; found->style = style; }
    updatePresentation(*found);
    startTransition();
    update();
    return true;
}

void SpectrumWidget::updatePresentation(Trace& trace) {
    trace.target = trace.frame;
    trace.target.levelsDb = smoothDisplaySpectrum(trace.frame.frequenciesHz.data(), trace.frame.levelsDb.data(),
        trace.frame.levelsDb.size(), m_view.smoothingOctaves);
    trace.target.peaksDb = smoothDisplaySpectrum(trace.frame.frequenciesHz.data(), trace.frame.peaksDb.data(),
        trace.frame.peaksDb.size(), m_view.smoothingOctaves);
    if (trace.displayed.frequenciesHz != trace.target.frequenciesHz ||
        trace.displayed.levelsDb.size() != trace.target.levelsDb.size() ||
        trace.displayed.peaksDb.size() != trace.target.peaksDb.size()) {
        trace.displayed = trace.target;
        trace.followingLevels = trace.target.levelsDb;
        trace.followingPeaks = trace.target.peaksDb;
    }
    trace.displayed.ready = trace.displayed.ready || trace.target.ready;
}

void SpectrumWidget::removeTrace(const QString& id) {
    m_traces.erase(std::remove_if(m_traces.begin(), m_traces.end(),
        [&](const Trace& trace) { return trace.id == id; }), m_traces.end());
    update();
}
void SpectrumWidget::clearTraces() { m_traces.clear(); update(); }
void SpectrumWidget::setTraceStyle(const QString& id, const TraceStyle& style) {
    for (auto& trace : m_traces) if (trace.id == id) trace.style = style;
    update();
}
bool SpectrumWidget::setView(const SpectrumView& view) {
    if (!validView(view)) return false;
    const bool rangeChanged = view.minimumFrequency != m_view.minimumFrequency ||
                              view.maximumFrequency != m_view.maximumFrequency;
    const bool smoothingChanged = view.smoothingOctaves != m_view.smoothingOctaves;
    m_view = view;
    if (smoothingChanged) for (auto& trace : m_traces) updatePresentation(trace);
    if (rangeChanged) resetZoom();
    startTransition();
    update();
    return true;
}
void SpectrumWidget::setTheme(const SpectrumTheme& theme) { m_theme = theme; update(); }
void SpectrumWidget::setFrozen(bool frozen) {
    if (m_frozen == frozen) return;
    m_frozen = frozen;
    if (frozen) {
        // Hold the currently visible curve, rather than jumping to a pending target.
        for (auto& trace : m_traces) {
            trace.target = trace.displayed;
            trace.followingLevels = trace.displayed.levelsDb;
            trace.followingPeaks = trace.displayed.peaksDb;
        }
    }
    update();
    if (frozenChanged) frozenChanged(frozen);
}
void SpectrumWidget::resetZoom() {
    m_visibleMinimum = m_view.minimumFrequency;
    m_visibleMaximum = m_view.maximumFrequency;
    update();
}
QRectF SpectrumWidget::plotRect() const {
    const QFontMetricsF metrics(font());
    const double right = m_view.decibelLabels ? metrics.horizontalAdvance(QStringLiteral("-120")) + 18 : 16;
    const double top = m_view.legend ? metrics.height() + 25 : 16;
    const double bottom = m_view.frequencyLabels ? metrics.height() + 18 : 16;
    return QRectF(18, top, std::max(1.0, width() - 18.0 - right),
                   std::max(1.0, height() - top - bottom));
}
double SpectrumWidget::frequencyAt(double x) const {
    const auto plot = plotRect();
    const double fraction = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
    return m_visibleMinimum * std::pow(m_visibleMaximum / m_visibleMinimum, fraction);
}
double SpectrumWidget::xForFrequency(double frequency) const {
    if (!std::isfinite(frequency) || frequency <= 0.0) return plotRect().left();
    return plotRect().left() + std::log(frequency / m_visibleMinimum) /
        std::log(m_visibleMaximum / m_visibleMinimum) * plotRect().width();
}

void SpectrumWidget::startTransition() {
    if (!isVisible() || m_view.transitionMs <= 0.0) {
        for (auto& trace : m_traces) {
            trace.displayed = trace.target;
            trace.followingLevels = trace.target.levelsDb;
            trace.followingPeaks = trace.target.peaksDb;
        }
        m_followingMinimumDb = m_displayMinimumDb = m_view.minimumDb;
        m_followingMaximumDb = m_displayMaximumDb = m_view.maximumDb;
        m_followingTilt = m_displayTilt = m_view.tiltDbPerOctave;
        m_followingHighLift = m_displayHighLift = m_view.highFrequencyLiftDb;
        m_transitionTimer.stop();
    } else if (!m_transitionTimer.isActive()) {
        m_transitionClock.start();
        m_transitionTimer.start();
    }
}
void SpectrumWidget::advanceTransition() {
    const double elapsedMs = double(m_transitionClock.nsecsElapsed()) * 1e-6;
    m_transitionClock.restart();
    // Exact solution of two cascaded one-pole filters. Unlike a single easing
    // stage, the visible velocity stays continuous when the target is replaced.
    // (1 + 4.7439) * exp(-4.7439) = 0.05: 95% settling in transitionMs.
    const double step = 4.7438645 * elapsedMs / std::max(0.01, m_view.transitionMs);
    const double decay = std::exp(-step);
    bool moving = false;
    const auto approach = [&](double current, double& following, double target) {
        if (std::abs(current - target) < 0.01 && std::abs(following - target) < 0.01) {
            following = target; return target;
        }
        moving = true;
        const double oldFollowing = following;
        following = target + (following - target) * decay;
        return target + (current - target + (oldFollowing - target) * step) * decay;
    };
    const auto advanceLevels = [&](std::vector<float>& current, std::vector<float>& following,
                                   const std::vector<float>& target) {
        for (std::size_t i = 0; i < current.size(); ++i) {
            double firstStage = following[i];
            current[i] = float(approach(current[i], firstStage, target[i]));
            following[i] = float(firstStage);
        }
    };
    for (auto& trace : m_traces) {
        advanceLevels(trace.displayed.levelsDb, trace.followingLevels, trace.target.levelsDb);
        advanceLevels(trace.displayed.peaksDb, trace.followingPeaks, trace.target.peaksDb);
    }
    m_displayMinimumDb = approach(m_displayMinimumDb, m_followingMinimumDb, m_view.minimumDb);
    m_displayMaximumDb = approach(m_displayMaximumDb, m_followingMaximumDb, m_view.maximumDb);
    m_displayTilt = approach(m_displayTilt, m_followingTilt, m_view.tiltDbPerOctave);
    m_displayHighLift = approach(m_displayHighLift, m_followingHighLift, m_view.highFrequencyLiftDb);
    if (!moving) {
        for (auto& trace : m_traces) trace.displayed.ready = trace.target.ready;
        m_transitionTimer.stop();
    }
    update();
}
void SpectrumWidget::showEvent(QShowEvent* event) { QWidget::showEvent(event); startTransition(); }
void SpectrumWidget::hideEvent(QHideEvent* event) { m_transitionTimer.stop(); QWidget::hideEvent(event); }

void SpectrumWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), m_theme.background);
    const auto plot = plotRect();
    const QFontMetricsF metrics(font());
    const auto yForDb = [&](double db) {
        return plot.bottom() - (db - m_displayMinimumDb) / (m_displayMaximumDb - m_displayMinimumDb) * plot.height();
    };
    const double dbStep = plot.height() < 190 ? 24.0 : 12.0;
    for (double db = std::ceil(m_displayMinimumDb / dbStep) * dbStep; db <= m_displayMaximumDb; db += dbStep) {
        const double y = yForDb(db);
        painter.setPen(QPen(db == 0 ? m_theme.strongGrid : m_theme.grid, 1));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (m_view.decibelLabels) {
            painter.setPen(m_theme.secondaryText);
            painter.drawText(QRectF(plot.right() + 9, y - metrics.height() / 2,
                width() - plot.right() - 10, metrics.height()), Qt::AlignVCenter | Qt::AlignLeft,
                QString::number(int(db)));
        }
    }

    // A logarithmic 1-2-5 grid adapts to zoom and width. Skip colliding labels,
    // never shrink their text to squeeze a desktop-sized scale into a mini graph.
    double lastLabelRight = -1000;
    for (int decade = 0; decade <= 5; ++decade) {
        for (double multiple : {1.0, 2.0, 5.0}) {
            const double hz = multiple * std::pow(10.0, decade);
            if (hz < m_visibleMinimum * 0.999 || hz > m_visibleMaximum * 1.001) continue;
            const double x = xForFrequency(hz);
            painter.setPen(QPen(hz == 1000 ? m_theme.strongGrid : m_theme.grid, 1));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            if (!m_view.frequencyLabels) continue;
            const QString text = hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000, 0, 'g', 3)
                                             : QString::number(int(hz));
            const double w = metrics.horizontalAdvance(text) + 4;
            const double left = std::clamp(x - w / 2, 2.0, std::max(2.0, width() - w - 2));
            if (left < lastLabelRight + 14) continue;
            painter.setPen(m_theme.secondaryText);
            painter.drawText(QRectF(left, plot.bottom() + 8, w, metrics.height()), Qt::AlignCenter, text);
            lastLabelRight = left + w;
        }
    }

    SpectrumView visible = m_view;
    visible.minimumDb = m_displayMinimumDb;
    visible.maximumDb = m_displayMaximumDb;
    visible.tiltDbPerOctave = m_displayTilt;
    visible.highFrequencyLiftDb = m_displayHighLift;
    visible.smoothingOctaves = 0; // The presentation targets are already rounded.
    visible.minimumFrequency = m_visibleMinimum;
    visible.maximumFrequency = m_visibleMaximum;
    painter.save();
    painter.setClipRect(plot);
    std::vector<const Trace*> layers;
    for (const auto& trace : m_traces) layers.push_back(&trace);
    std::stable_sort(layers.begin(), layers.end(), [](const Trace* a, const Trace* b) {
        return a->style.zOrder < b->style.zOrder;
    });
    for (const auto* layer : layers) {
        const auto& trace = *layer;
        if (!trace.style.visible || !trace.displayed.ready) continue;
        const auto line = spectrumPath(trace.displayed, plot, visible);
        if (line.isEmpty()) continue;
        if (trace.style.filled) {
            auto area = line;
            if (trace.style.fillReference.isEmpty()) {
                area.lineTo(QPointF(line.currentPosition().x(), plot.bottom()));
                area.lineTo(QPointF(line.elementAt(0).x, plot.bottom()));
            } else {
                const auto reference = std::find_if(m_traces.begin(), m_traces.end(),
                    [&](const Trace& other) { return other.id == trace.style.fillReference; });
                const auto referencePath = reference == m_traces.end() ? QPainterPath{} :
                    spectrumPath(reference->displayed, plot, visible);
                // An unavailable reference means no difference fill, not a large
                // misleading area down to the floor. The foreground line remains.
                if (referencePath.isEmpty()) area = {};
                else area.connectPath(referencePath.toReversed());
            }
            area.closeSubpath();
            QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
            auto transparent = traceFill(trace.style, m_theme);
            transparent.setAlpha(3);
            gradient.setColorAt(0, traceFill(trace.style, m_theme));
            gradient.setColorAt(1, transparent);
            if (trace.style.fillReference.isEmpty()) painter.fillPath(area, gradient);
            else painter.fillPath(area, traceFill(trace.style, m_theme));
        }
        if (m_view.peakHold && trace.frame.peaksDb.size() == trace.frame.frequenciesHz.size()) {
            auto peakInk = traceLine(trace.style, m_theme);
            peakInk.setAlpha(90);
            painter.setPen(QPen(peakInk, 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(spectrumPath(trace.displayed, plot, visible, true));
        }
        painter.setPen(QPen(traceLine(trace.style, m_theme), trace.style.width, trace.style.penStyle, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(line);
    }
    painter.restore();

    if (m_view.legend) {
        double x = plot.left();
        const double y = 18;
        for (const auto& trace : m_traces) {
            if (!trace.style.visible) continue;
            const double labelWidth = metrics.horizontalAdvance(trace.style.label);
            if (x + labelWidth + 36 > plot.right() - 100) break;
            painter.setPen(QPen(traceLine(trace.style, m_theme), 2, trace.style.penStyle));
            painter.drawLine(QPointF(x, y), QPointF(x + 16, y));
            painter.setPen(m_theme.text);
            painter.drawText(QRectF(x + 24, y - metrics.height() / 2, labelWidth + 2, metrics.height()),
                             Qt::AlignVCenter, trace.style.label);
            x += labelWidth + 48;
        }
        painter.setPen(m_frozen ? m_theme.cursor : m_theme.secondaryText);
        const auto unit = m_frozen ? tr("Frozen") : m_view.tiltDbPerOctave == 0 && m_view.highFrequencyLiftDb == 0
            ? QStringLiteral("dBFS") : tr("Display dB");
        painter.drawText(QRectF(plot.right() - 100, y - metrics.height() / 2, 100, metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, unit);
    }

    const bool ready = std::any_of(m_traces.begin(), m_traces.end(),
        [](const Trace& trace) { return trace.style.visible && trace.frame.ready; });
    if (!ready) {
        painter.setPen(m_theme.secondaryText);
        painter.drawText(plot, Qt::AlignCenter, tr("Waiting for audio"));
    }
    if (m_view.interactive && m_cursorX >= plot.left() && m_cursorX <= plot.right()) {
        painter.save();
        painter.setClipRect(plot);
        painter.setPen(QPen(m_theme.cursor, 1, Qt::DashLine));
        painter.drawLine(QPointF(m_cursorX, plot.top()), QPointF(m_cursorX, plot.bottom()));
        const double frequency = frequencyAt(m_cursorX);
        for (const auto& trace : m_traces) {
            if (!trace.style.visible || !trace.displayed.ready) continue;
            const double db = levelAt(trace.displayed, frequency) + displayWeightingDb(frequency, visible);
            painter.setPen(QPen(m_theme.background, 2));
            painter.setBrush(traceLine(trace.style, m_theme));
            painter.drawEllipse(QPointF(m_cursorX, yForDb(db)), 3.5, 3.5);
        }
        const auto text = cursorText();
        const auto bounds = metrics.boundingRect(QRectF(0, 0, 1000, 1000), Qt::AlignLeft, text);
        const double w = std::min(plot.width() - 16, bounds.width() + 24);
        const double h = bounds.height() + 18;
        const double x = std::clamp(m_cursorX + 14, plot.left() + 8, std::max(plot.left() + 8, plot.right() - w - 8));
        const QRectF box(x, plot.top() + 10, w, h);
        painter.setPen(QPen(m_theme.strongGrid, 1));
        painter.setBrush(m_theme.readoutBackground);
        painter.drawRoundedRect(box, 6, 6);
        painter.setPen(m_theme.text);
        painter.drawText(box.adjusted(12, 8, -10, -8), Qt::AlignLeft | Qt::AlignTop, text);
        painter.restore();
    }
    if (hasFocus()) {
        painter.setPen(QPen(m_theme.cursor, 1, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 5, 5);
    }
}

QString SpectrumWidget::cursorText() const {
    const double frequency = frequencyAt(m_cursorX);
    QString text = frequencyText(frequency) + QStringLiteral("  ·  ") + noteText(frequency);
    for (const auto& trace : m_traces) {
        if (!trace.style.visible || !trace.displayed.ready) continue;
        text += QStringLiteral("\n%1  %2 dBFS").arg(trace.style.label).arg(levelAt(trace.frame, frequency), 0, 'f', 1);
    }
    return text;
}
void SpectrumWidget::updateCursor(double x) {
    m_cursorX = std::clamp(x, plotRect().left(), plotRect().right());
    // A numeric alternative to interpreting the graph, also available by keyboard.
    setAccessibleDescription(cursorText());
    update();
}
void SpectrumWidget::setVisibleRange(double low, double high) {
    const double fullLow = std::log(m_view.minimumFrequency);
    const double fullHigh = std::log(m_view.maximumFrequency);
    const double span = std::min(std::log(high / low), fullHigh - fullLow);
    const double start = std::clamp(std::log(low), fullLow, fullHigh - span);
    m_visibleMinimum = std::exp(start);
    m_visibleMaximum = std::exp(start + span);
    update();
}
void SpectrumWidget::zoom(double factor, double anchorX) {
    const double fraction = std::clamp((anchorX - plotRect().left()) / plotRect().width(), 0.0, 1.0);
    const double anchor = std::log(frequencyAt(anchorX));
    const double maximumSpan = std::log(m_view.maximumFrequency / m_view.minimumFrequency);
    const double minimumSpan = std::min(std::log(2.0) * 0.5, maximumSpan);
    const double span = std::clamp(std::log(m_visibleMaximum / m_visibleMinimum) / factor, minimumSpan, maximumSpan);
    setVisibleRange(std::exp(anchor - fraction * span), std::exp(anchor + (1.0 - fraction) * span));
}
void SpectrumWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_view.interactive) return QWidget::mouseMoveEvent(event);
    if (m_panning) {
        const double shift = (m_panStart.x() - event->position().x()) / plotRect().width() * std::log(m_panHigh / m_panLow);
        setVisibleRange(m_panLow * std::exp(shift), m_panHigh * std::exp(shift));
    }
    if (plotRect().contains(event->position())) updateCursor(event->position().x());
    else { m_cursorX = -1; update(); }
}
void SpectrumWidget::mousePressEvent(QMouseEvent* event) {
    if (!m_view.interactive || !plotRect().contains(event->position())) return QWidget::mousePressEvent(event);
    setFocus(Qt::MouseFocusReason);
    updateCursor(event->position().x());
    if (event->button() == Qt::MiddleButton) {
        m_panning = true; m_panStart = event->position();
        m_panLow = m_visibleMinimum; m_panHigh = m_visibleMaximum;
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::LeftButton && frequencySelected) frequencySelected(frequencyAt(m_cursorX));
}
void SpectrumWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) { m_panning = false; unsetCursor(); }
    QWidget::mouseReleaseEvent(event);
}
void SpectrumWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_view.interactive && event->button() == Qt::LeftButton) resetZoom();
}
void SpectrumWidget::leaveEvent(QEvent* event) {
    if (!hasFocus()) m_cursorX = -1;
    update(); QWidget::leaveEvent(event);
}
void SpectrumWidget::wheelEvent(QWheelEvent* event) {
    if (!m_view.interactive || !plotRect().contains(event->position())) return QWidget::wheelEvent(event);
    const double delta = event->pixelDelta().isNull() ? event->angleDelta().y() / 120.0 : event->pixelDelta().y() / 60.0;
    zoom(std::pow(1.25, delta), event->position().x());
    updateCursor(event->position().x());
    event->accept();
}
void SpectrumWidget::keyPressEvent(QKeyEvent* event) {
    if (!m_view.interactive) return QWidget::keyPressEvent(event);
    switch (event->key()) {
    case Qt::Key_Space: setFrozen(!m_frozen); break;
    case Qt::Key_Home: resetZoom(); break;
    case Qt::Key_Plus: case Qt::Key_Equal: zoom(1.25, plotRect().center().x()); break;
    case Qt::Key_Minus: zoom(0.8, plotRect().center().x()); break;
    case Qt::Key_Left: case Qt::Key_Right:
        updateCursor((m_cursorX < 0 ? plotRect().center().x() : m_cursorX) +
                     (event->key() == Qt::Key_Left ? -1 : 1) * (event->modifiers() & Qt::ShiftModifier ? 12 : 2)); break;
    case Qt::Key_Escape: m_cursorX = -1; clearFocus(); update(); break;
    default: return QWidget::keyPressEvent(event);
    }
    event->accept();
}
void SpectrumWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!m_view.interactive) return;
    QMenu menu(this);
    auto* freeze = menu.addAction(m_frozen ? tr("Resume spectrum") : tr("Freeze spectrum"));
    freeze->setShortcut(QKeySequence(Qt::Key_Space));
    auto* peaks = menu.addAction(tr("Peak hold"));
    peaks->setCheckable(true); peaks->setChecked(m_view.peakHold);
    auto* reset = menu.addAction(tr("Reset zoom"));
    const auto* chosen = menu.exec(event->globalPos());
    if (chosen == freeze) setFrozen(!m_frozen);
    if (chosen == peaks) { m_view.peakHold = !m_view.peakHold; update(); }
    if (chosen == reset) resetZoom();
}
} // namespace vlt::spectre