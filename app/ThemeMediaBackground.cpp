#include "ThemeMediaBackground.hpp"

#include <QDateTime>
#include <QImage>
#include <QMediaPlayer>
#include <QMovie>
#include <QPainter>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

QImage composeFrame(const QImage& source, const QSize& target,
                    timelinebackgroundprefs::Placement placement,
                    int blurRadius) {
    if (source.isNull() || target.isEmpty()) return {};

    QImage composed;
    switch (placement) {
    case timelinebackgroundprefs::Placement::Fill: {
        const QImage cover = source.scaled(
            target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        composed = cover.copy(QRect((cover.width() - target.width()) / 2,
                                    (cover.height() - target.height()) / 2,
                                    target.width(), target.height()));
        break;
    }
    case timelinebackgroundprefs::Placement::Stretch:
        composed = source.scaled(target, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        break;
    case timelinebackgroundprefs::Placement::Tile: {
        composed = QImage(target, QImage::Format_ARGB32_Premultiplied);
        composed.fill(Qt::black);
        QPainter painter(&composed);
        for (int y = 0; y < target.height(); y += source.height())
            for (int x = 0; x < target.width(); x += source.width())
                painter.drawImage(x, y, source);
        break;
    }
    case timelinebackgroundprefs::Placement::Center: {
        composed = QImage(target, QImage::Format_ARGB32_Premultiplied);
        composed.fill(Qt::black);
        QPainter painter(&composed);
        painter.drawImage((target.width() - source.width()) / 2,
                          (target.height() - source.height()) / 2, source);
        break;
    }
    }

    if (blurRadius <= 0) return composed;
    // A cheap, bounded soft focus. Media decoding and composition stay on the
    // UI side; the real-time audio thread never sees either operation.
    const double reduction = 1.0 + double(blurRadius) / 4.0;
    const QSize softSize(
        std::max(1, int(std::lround(target.width() / reduction))),
        std::max(1, int(std::lround(target.height() / reduction))));
    return composed.scaled(softSize, Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation)
        .scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

} // namespace

ThemeMediaBackground::ThemeMediaBackground(QObject* parent) : QObject(parent) {}

ThemeMediaBackground::~ThemeMediaBackground() { clearDecoder(); }

void ThemeMediaBackground::clearDecoder() {
    if (m_movie) {
        m_movie->stop();
        delete m_movie;
        m_movie = nullptr;
    }
    if (m_video) {
        m_video->stop();
        m_video->setVideoSink(nullptr);
        delete m_video;
        m_video = nullptr;
    }
    delete m_videoSink;
    m_videoSink = nullptr;
}

void ThemeMediaBackground::setSource(const QString& path) {
    if (path == m_path) return;
    clearDecoder();
    m_path = path;
    m_sourceFrame = {};
    m_frame = {};
    m_lastVideoFrameMs = 0;
    emit frameChanged(false);

    using namespace timelinebackgroundprefs;
    switch (mediaKind(path)) {
    case MediaKind::Image:
        acceptSourceFrame(QImage(path), false);
        break;
    case MediaKind::AnimatedImage:
        m_movie = new QMovie(path, QByteArray(), this);
        // Retaining every high-resolution GIF frame is unnecessary for a
        // wallpaper. The local source remains seekable when it loops.
        m_movie->setCacheMode(QMovie::CacheNone);
        connect(m_movie, &QMovie::frameChanged, this, [this](int) {
            acceptSourceFrame(m_movie->currentImage(), true);
            syncPlayback();
        });
        connect(m_movie, &QMovie::finished, this, [this] {
            if (m_playRequested) m_movie->start();
        });
        // Decode one still frame even when motion is disabled.
        m_movie->start();
        break;
    case MediaKind::Video:
        m_videoSink = new QVideoSink(this);
        m_video = new QMediaPlayer(this);
        m_video->setVideoSink(m_videoSink);
        m_video->setLoops(QMediaPlayer::Infinite);
        connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame& videoFrame) {
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (!m_sourceFrame.isNull() &&
                        now - m_lastVideoFrameMs < 33)
                        return;
                    m_lastVideoFrameMs = now;
                    acceptSourceFrame(videoFrame.toImage(), true);
                    syncPlayback();
                });
        m_video->setSource(QUrl::fromLocalFile(path));
        // As with GIF, play only until the first frame when paused/reduced.
        m_video->play();
        break;
    case MediaKind::None:
        break;
    }
}

void ThemeMediaBackground::setTargetSize(const QSize& logicalSize,
                                         qreal devicePixelRatio) {
    devicePixelRatio = std::max<qreal>(1.0, devicePixelRatio);
    if (m_logicalSize == logicalSize &&
        qFuzzyCompare(m_devicePixelRatio, devicePixelRatio))
        return;
    m_logicalSize = logicalSize;
    m_devicePixelRatio = devicePixelRatio;
    rebuild();
}

void ThemeMediaBackground::setPlacement(
    timelinebackgroundprefs::Placement placement) {
    if (m_placement == placement) return;
    m_placement = placement;
    rebuild();
}

void ThemeMediaBackground::setBlurRadius(int logicalPixels) {
    logicalPixels = std::clamp(logicalPixels, 0, 32);
    if (m_blurRadius == logicalPixels) return;
    m_blurRadius = logicalPixels;
    rebuild();
}

void ThemeMediaBackground::setPlaying(bool playing) {
    if (m_playRequested == playing) return;
    m_playRequested = playing;
    syncPlayback();
}

void ThemeMediaBackground::syncPlayback() {
    if (m_movie) {
        if (m_playRequested) {
            if (m_movie->state() == QMovie::NotRunning)
                m_movie->start();
            else
                m_movie->setPaused(false);
        } else if (!m_sourceFrame.isNull()) {
            m_movie->setPaused(true);
        }
    }
    if (m_video) {
        if (m_playRequested || m_sourceFrame.isNull())
            m_video->play();
        else
            m_video->pause();
    }
}

void ThemeMediaBackground::acceptSourceFrame(const QImage& source,
                                             bool animatedFrame) {
    if (source.isNull()) return;
    m_sourceFrame = source;
    rebuild(animatedFrame);
}

void ThemeMediaBackground::rebuild(bool animatedFrame) {
    if (m_sourceFrame.isNull() || m_logicalSize.isEmpty()) {
        m_frame = {};
    } else {
        const QSize pixels(
            std::max(1, int(std::ceil(m_logicalSize.width() *
                                     m_devicePixelRatio))),
            std::max(1, int(std::ceil(m_logicalSize.height() *
                                     m_devicePixelRatio))));
        m_frame = QPixmap::fromImage(composeFrame(
            m_sourceFrame, pixels, m_placement,
            int(std::lround(m_blurRadius * m_devicePixelRatio))));
        m_frame.setDevicePixelRatio(m_devicePixelRatio);
    }
    emit frameChanged(animatedFrame);
}

bool checkThemeMediaBackgroundForTest(QString* error) {
    QImage source(2, 2, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    source.setPixelColor(1, 1, Qt::green);
    const QSize target(6, 4);

    const QImage fill = composeFrame(
        source, target, timelinebackgroundprefs::Placement::Fill, 0);
    const QImage stretch = composeFrame(
        source, target, timelinebackgroundprefs::Placement::Stretch, 0);
    const QImage tile = composeFrame(
        source, target, timelinebackgroundprefs::Placement::Tile, 0);
    const QImage center = composeFrame(
        source, target, timelinebackgroundprefs::Placement::Center, 0);
    const bool ok = fill.size() == target && stretch.size() == target &&
                    tile.size() == target && center.size() == target &&
                    tile.pixelColor(1, 1) == tile.pixelColor(3, 3) &&
                    center.pixelColor(0, 0) == QColor(Qt::black) &&
                    center.pixelColor(2, 1) == QColor(Qt::red);
    if (!ok && error)
        *error = QStringLiteral("theme media placement check failed");
    return ok;
}

} // namespace ui
