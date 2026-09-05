#pragma once

#include "TimelineBackgroundPrefs.hpp"

#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>

class QMediaPlayer;
class QMovie;
class QVideoSink;

namespace ui {

/// Presentation-only local media shared by the header and arrangement. It
/// decodes at most one current frame and prepares it at the consumer's device
/// resolution, so paint events remain a single cheap pixmap draw.
class ThemeMediaBackground final : public QObject {
    Q_OBJECT
public:
    explicit ThemeMediaBackground(QObject* parent = nullptr);
    ~ThemeMediaBackground() override;

    void setSource(const QString& path);
    void setTargetSize(const QSize& logicalSize, qreal devicePixelRatio);
    void setPlacement(timelinebackgroundprefs::Placement placement);
    void setBlurRadius(int logicalPixels);
    void setPlaying(bool playing);

    const QPixmap& frame() const { return m_frame; }
    bool hasFrame() const { return !m_frame.isNull(); }

signals:
    /// `animatedFrame` lets the timeline replace only its cached wallpaper
    /// layer instead of repainting every clip for each video frame.
    void frameChanged(bool animatedFrame);

private:
    void clearDecoder();
    void acceptSourceFrame(const QImage& frame, bool animatedFrame);
    void rebuild(bool animatedFrame = false);
    void syncPlayback();

    QPixmap m_frame;
    QImage m_sourceFrame;
    QString m_path;
    QSize m_logicalSize;
    qreal m_devicePixelRatio = 1.0;
    timelinebackgroundprefs::Placement m_placement =
        timelinebackgroundprefs::Placement::Fill;
    int m_blurRadius = 0;
    bool m_playRequested = false;
    qint64 m_lastVideoFrameMs = 0;
    QMovie* m_movie = nullptr;
    QMediaPlayer* m_video = nullptr;
    QVideoSink* m_videoSink = nullptr;
};

/// Headless branch check for all placement modes.
bool checkThemeMediaBackgroundForTest(QString* error = nullptr);

} // namespace ui
