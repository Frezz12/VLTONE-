#pragma once

#include <QColor>
#include <QWidget>

#include <array>
#include <cstddef>

/// Compact master-bus spectrum between the playhead and tempo readouts.
///
/// The twelve bars are real log-spaced frequency bands, low at the left and
/// high at the right. The engine supplies linear RMS values; this widget only
/// performs display smoothing and the dB-to-height mapping.
class SpectrumMeter final : public QWidget {
    Q_OBJECT
public:
    static constexpr std::size_t kBandCount = 12;
    using Levels = std::array<float, kBandCount>;

    explicit SpectrumMeter(QWidget* parent = nullptr);

    /// Update the displayed band energy. `live` is false only when the audio
    /// device itself is unavailable; a stopped transport remains live so
    /// previews, played instruments and effect tails are still visible.
    void push(const Levels& levels, bool live);

    /// The transport uses its record colour while a take is rolling.
    void setAccent(const QColor& accent);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    static double displayHeight(float linear);

    Levels m_levels{};
    Levels m_peakHold{};
    QColor m_accent;
    bool m_awake = false;
};
