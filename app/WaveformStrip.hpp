#pragma once

#include "WaveformCache.hpp"

#include <QString>
#include <QWidget>

/// One file's waveform across the full width, with a playhead.
///
/// The browser's preview bar, and anything else that needs to show a whole file
/// rather than a clip: it owns its envelope (rather than pointing into the
/// cache, which belongs to the document's files) and maps the width to the
/// file's length, so there is no zoom to keep in step. Clicking or dragging
/// asks for a seek — the widget draws, it does not play.
class WaveformStrip : public QWidget {
    Q_OBJECT
public:
    explicit WaveformStrip(QWidget* parent = nullptr);

    /// Show this envelope. Copied, because the worker that built it is about to
    /// drop it and the widget outlives the decode.
    void setPeaks(const daw::WaveformPeaks& peaks);
    /// Forget the waveform and show `message` instead ("no file", "decoding…",
    /// "cannot be read"). An empty message leaves the strip blank.
    void clear(const QString& message = {});

    /// Where the audition head is, in seconds into the file. Negative hides it.
    void setPlayheadSeconds(double seconds);
    double durationSeconds() const { return m_peaks.durationSeconds; }
    bool hasWaveform() const { return m_peaks.isValid(); }

signals:
    /// The user pointed at a position in the file, in seconds.
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;

private:
    /// Seconds under an x in widget coordinates, clamped to the file.
    double secondsAt(double x) const;

    daw::WaveformPeaks m_peaks;
    QString m_message;
    double m_playheadSeconds = -1.0;
};
