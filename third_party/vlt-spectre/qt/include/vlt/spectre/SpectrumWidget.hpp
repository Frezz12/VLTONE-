// SPDX-License-Identifier: MIT
#pragma once

#include <vlt/spectre/Analyzer.hpp>
#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

namespace vlt::spectre {

/// No application-global theme: every widget can be styled independently.
struct SpectrumTheme {
    QColor background, grid, strongGrid, text, secondaryText, cursor, readoutBackground;
    static SpectrumTheme dark();
    static SpectrumTheme light();
};

struct SpectrumView {
    double minimumFrequency = 20.0;
    double maximumFrequency = 20000.0;
    double minimumDb = -96.0;
    double maximumDb = 6.0;
    double tiltDbPerOctave = 0.0;  ///< Presentation only, pivoting around 1 kHz.
    double highFrequencyLiftDb = 12.0; ///< Smooth visual shelf; measured levels stay unchanged.
    double highFrequencyStartHz = 2000.0; ///< No additional lift at/below this frequency.
    double highFrequencyEndHz = 16000.0;  ///< Full shelf lift at/above this frequency.
    double smoothingOctaves = 1.0 / 6.0;  ///< Rounded envelope width; 0 disables it.
    double transitionMs = 180.0;  ///< Approx. 95% settling time; 0 for reduced motion.
    bool frequencyLabels = true;
    bool decibelLabels = true;
    bool legend = true;
    bool peakHold = false;
    bool interactive = true;      ///< Disable for embedded miniature graphs.
};

struct TraceStyle {
    QString label = QStringLiteral("Input");
    QColor line;                 ///< Invalid color selects a theme-aware default.
    QColor fill;                 ///< Invalid color selects a theme-aware default.
    qreal width = 1.4;
    Qt::PenStyle penStyle = Qt::SolidLine;
    bool filled = true;
    bool visible = true;
    int zOrder = 0;              ///< Back to front. Equal values keep insertion order.
    QString fillReference;      ///< Optional trace ID: fill only the gap to that curve.
};

/// Presentation weighting in dB. The high shelf is zero below its start frequency;
/// it never adds a bell-shaped boost to the midrange. Numeric readouts bypass it.
double displayWeightingDb(double frequencyHz, const SpectrumView& view);

/// Reusable drawing helper for hosts with their own QWidget/QPainter composition.
/// The bounded Hermite curve goes through the data without inventing overshoot.
/// Set view.minimum/maximumFrequency to the visible range. The caller clips to plot.
QPainterPath spectrumPath(const SpectrumFrame& frame, const QRectF& plot,
                          const SpectrumView& view, bool peaks = false);

/// Optional Qt view. Owns no audio device, analyzer, worker or global settings.
/// Its lightweight presentation timer runs only while a transition is active.
/// Feed it frames from your application's single analyzer consumer, on the UI thread.
/// Cursor/zoom interaction is immediate. Amplitude and display changes use a
/// critically damped presentation filter; transitionMs = 0 disables that filter.
class SpectrumWidget final : public QWidget {
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);

    /// Stable IDs let you overlay arbitrary input/output/sidechain/reference traces.
    /// Copies data on the UI thread; do not call this from an audio callback.
    /// Empty/unready frames are allowed. Malformed custom frames are rejected.
    bool setTrace(const QString& id, const SpectrumFrame& frame, const TraceStyle& style = {});
    void removeTrace(const QString& id);
    void clearTraces();
    void setTraceStyle(const QString& id, const TraceStyle& style);
    bool setView(const SpectrumView& view);
    const SpectrumView& view() const noexcept { return m_view; }
    void setTheme(const SpectrumTheme& theme);
    const SpectrumTheme& theme() const noexcept { return m_theme; }
    void setFrozen(bool frozen);
    bool frozen() const noexcept { return m_frozen; }
    void resetZoom();

    QRectF plotRect() const;
    double frequencyAt(double x) const;
    double xForFrequency(double frequency) const;
    double visibleMinimumFrequency() const noexcept { return m_visibleMinimum; }
    double visibleMaximumFrequency() const noexcept { return m_visibleMaximum; }
    QSize minimumSizeHint() const override { return {180, 130}; }
    QSize sizeHint() const override { return {900, 440}; }

    /// Optional host hooks; no moc or subclass is needed to embed the widget.
    std::function<void(double frequencyHz)> frequencySelected;
    std::function<void(bool frozen)> frozenChanged;

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    struct Trace {
        QString id;
        SpectrumFrame frame;      // Latest target from the host.
        SpectrumFrame target;     // Rounded presentation target; never used for measurements.
        SpectrumFrame displayed;  // Visible presentation; retarget from here.
        std::vector<float> followingLevels, followingPeaks; // First of two damping stages.
        TraceStyle style;
    };
    void updatePresentation(Trace& trace);
    void startTransition();
    void advanceTransition();
    void zoom(double factor, double anchorX);
    void setVisibleRange(double low, double high);
    void updateCursor(double x);
    QString cursorText() const;

    SpectrumView m_view;
    // Two cascaded exponential stages give continuous velocity when a target changes.
    double m_displayMinimumDb = -96.0, m_displayMaximumDb = 6.0, m_displayTilt = 0.0;
    double m_displayHighLift = 12.0;
    double m_followingMinimumDb = -96.0, m_followingMaximumDb = 6.0, m_followingTilt = 0.0;
    double m_followingHighLift = 12.0;
    QTimer m_transitionTimer;
    QElapsedTimer m_transitionClock;
    SpectrumTheme m_theme = SpectrumTheme::dark();
    std::vector<Trace> m_traces;
    double m_visibleMinimum = 20.0, m_visibleMaximum = 20000.0;
    double m_cursorX = -1.0;
    bool m_frozen = false, m_panning = false;
    QPointF m_panStart;
    double m_panLow = 20.0, m_panHigh = 20000.0;
};
} // namespace vlt::spectre