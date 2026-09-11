#pragma once

#include "TimelineBackgroundPrefs.hpp"
#include "graphics/QuickVisual.hpp"

#include <QObject>
#include <QImage>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QElapsedTimer>

class QMediaPlayer;
class QMovie;
class QVideoSink;

namespace ui {
namespace graphics { class AnimatedImageDecoder; }

/// Presentation-only local media shared by the header and arrangement. It
/// decodes at most one current frame and prepares it at the consumer's device
/// resolution, so paint events remain a single cheap pixmap draw.
class ThemeMediaBackground final : public graphics::QuickVisual {
    Q_OBJECT
    Q_PROPERTY(QImage gpuImage READ gpuImage NOTIFY gpuFrameChanged)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY gpuFrameChanged)
    Q_PROPERTY(QUrl sourceUrl READ sourceUrl NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(bool videoSource READ videoSource NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(bool motionEnabled READ motionEnabled NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(int placementMode READ placementMode NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(int blurPixels READ blurPixels NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(int cornerRadius READ cornerRadius NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(double textureScale READ textureScale NOTIFY gpuConfigurationChanged)
    Q_PROPERTY(int frameLimit READ frameLimit NOTIFY gpuConfigurationChanged)
public:
    explicit ThemeMediaBackground(QObject* parent = nullptr);
    ~ThemeMediaBackground() override;

    void setSource(const QString& path);
    void setTargetSize(const QSize& logicalSize, qreal devicePixelRatio);
    void setPlacement(timelinebackgroundprefs::Placement placement);
    void setBlurRadius(int logicalPixels);
    void setPlaying(bool playing);

    const QPixmap& frame() const { return m_frame; }
    bool hasFrame() const { return m_gpu ? (videoSource() || !m_sourceFrame.isNull()) : !m_frame.isNull(); }
    void paint(QPainter&, const QRectF& bounds);
    QQuickItem* createItem(QQmlEngine*, QQuickItem*) override;
    void releaseItem(QQuickItem*) override;
    QImage gpuImage() const { return m_sourceFrame; }
    QSize imageSize() const { return m_sourceFrame.size(); }
    QUrl sourceUrl() const;
    bool videoSource() const;
    bool motionEnabled() const { return m_playRequested; }
    int placementMode() const { return int(m_placement); }
    int blurPixels() const { return m_blurRadius; }
    int cornerRadius() const { return m_cornerRadius; }
    void setCornerRadius(int radius);
    double textureScale() const;
    int frameLimit() const;

signals:
    /// `animatedFrame` lets the timeline replace only its cached wallpaper
    /// layer instead of repainting every clip for each video frame.
    void frameChanged(bool animatedFrame);
    void gpuFrameChanged();
    void gpuConfigurationChanged();

private:
    void clearDecoder();
    void requestImage();
    void setGpuPresentation(bool);
    void acceptSourceFrame(const QImage& frame, bool animatedFrame);
    void rebuild(bool animatedFrame = false);
    void syncPlayback();

    bool m_composing = false;
    bool m_imageLoading = false;
    bool m_imagePending = false;
    bool m_gpu = false;
    int m_cornerRadius = 0;
    graphics::AnimatedImageDecoder* m_animation = nullptr;
    bool m_composePending = false;
    bool m_pendingAnimated = false;
    quint64 m_generation = 0;
    quint64 m_sourceGeneration = 0;
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
    QElapsedTimer m_frameTime;
    QMovie* m_movie = nullptr;
    QMediaPlayer* m_video = nullptr;
    QVideoSink* m_videoSink = nullptr;
};

/// Headless branch check for all placement modes.
bool checkThemeMediaBackgroundForTest(QString* error = nullptr);

} // namespace ui
