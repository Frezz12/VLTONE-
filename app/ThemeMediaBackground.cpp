#include "ThemeMediaBackground.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/MediaImageItem.hpp"
#include "graphics/AnimatedImageDecoder.hpp"
#include "graphics/SceneRecordingTag.hpp"
#include "UiFrameClock.hpp"
#include <QQmlComponent>
#include <QImageReader>
#include <QApplication>
#include <QPointer>
#include <QThreadPool>
#include <QWidget>
#include <QScreen>

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
QThreadPool& mediaPool() {
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(2);
        pool.setThreadPriority(QThread::LowPriority);
        pool.setExpiryTimeout(5000);
        return true;
    }();
    Q_UNUSED(configured);
    return pool;
}

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

void finishThemeMediaTasks() {
    mediaPool().clear();
    mediaPool().waitForDone();
}

ThemeMediaBackground::ThemeMediaBackground(QObject* parent) : graphics::QuickVisual(parent) {
    m_gpu = graphics::gpuWorkspaceEnabled();
    m_frameTime.start();
    connect(&FrameClock::instance(), &FrameClock::preferenceChanged, this, &ThemeMediaBackground::gpuConfigurationChanged);
    connect(&graphics::GraphicsPreferences::instance(), &graphics::GraphicsPreferences::effectiveQualityChanged,
            this, [this] { rebuild(); });
}

ThemeMediaBackground::~ThemeMediaBackground() { clearDecoder(); }

void ThemeMediaBackground::clearDecoder() {
    if (m_animation) { delete m_animation; m_animation = nullptr; }
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

void ThemeMediaBackground::requestImage() {
    if (m_imageLoading) { m_imagePending = true; return; }
    if (timelinebackgroundprefs::mediaKind(m_path) != timelinebackgroundprefs::MediaKind::Image) return;
    m_imageLoading = true; m_imagePending = false;
    const QPointer<ThemeMediaBackground> guard(this);
    const auto generation = m_sourceGeneration;
    const auto path = m_path;
    mediaPool().start([guard, generation, path] {
        QImageReader reader(path);
        const QSize original = reader.size();
        if (qint64(original.width()) * original.height() > 16 * 1024 * 1024)
            reader.setScaledSize(original.scaled(4096, 4096, Qt::KeepAspectRatio));
        QImage image = reader.read();
        QMetaObject::invokeMethod(qApp, [guard, generation, path, image] {
            if (!guard) return;
            guard->m_imageLoading = false;
            if (guard->m_sourceGeneration == generation && guard->m_path == path)
                guard->acceptSourceFrame(image, false);
            if (guard && guard->m_imagePending) {
                guard->m_imagePending = false;
                guard->requestImage();
            }
        }, Qt::QueuedConnection);
    });
}

void ThemeMediaBackground::setSource(const QString& path) {
    if (path == m_path) return;
    clearDecoder();
    ++m_sourceGeneration;
    ++m_generation;
    m_path = path;
    m_sourceFrame = {};
    m_frame = {};
    m_lastVideoFrameMs = 0;
    emit frameChanged(false);

    using namespace timelinebackgroundprefs;
    switch (mediaKind(path)) {
    case MediaKind::Image:
        requestImage();
        break;
    case MediaKind::AnimatedImage:
        m_animation = new graphics::AnimatedImageDecoder(this);
        connect(m_animation, &graphics::AnimatedImageDecoder::frameReady, this,
                [this](const QImage& image) { acceptSourceFrame(image, true); });
        m_animation->setSource(path);
        m_animation->setPlaying(m_playRequested);
        break;
    case MediaKind::Video:
        if (m_gpu) break;
        m_videoSink = new QVideoSink(this);
        m_video = new QMediaPlayer(this);
        m_video->setVideoSink(m_videoSink);
        m_video->setLoops(QMediaPlayer::Infinite);
        connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame& videoFrame) {
                    const qint64 now = m_frameTime.elapsed();
                    const int preferredFps = graphics::GraphicsPreferences::instance().backgroundFps();
                    // This compatibility path still converts frames on CPU.
                    // Preserve its previous ceiling until VideoOutput owns it.
                    const int fps = preferredFps > 0 ? std::min(30, preferredFps) : 30;
                    if (!m_sourceFrame.isNull() &&
                        now - m_lastVideoFrameMs < 1000. / fps)
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
    emit gpuConfigurationChanged();
    if (m_gpu && videoSource()) emit frameChanged(false);
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
    emit gpuConfigurationChanged();
    if (m_animation) m_animation->setPlaying(m_playRequested);
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
    if (m_gpu && animatedFrame && !m_sourceFrame.isNull()) {
        const int fps = frameLimit();
        const qint64 now = m_frameTime.elapsed();
        if (fps > 0 && now - m_lastVideoFrameMs < 1000. / fps) return;
        m_lastVideoFrameMs = now;
    }
    const bool first = m_sourceFrame.isNull();
    m_sourceFrame = source;
    emit gpuFrameChanged();
    if (m_gpu) {
        if (first) emit frameChanged(false);
    } else rebuild(animatedFrame);
}

void ThemeMediaBackground::rebuild(bool animatedFrame) {
    ++m_generation;
    emit gpuConfigurationChanged();
    if (m_gpu) return;
    m_pendingAnimated = animatedFrame;
    if (m_sourceFrame.isNull() || m_logicalSize.isEmpty()) {
        m_frame = {}; m_composePending = false;
        emit frameChanged(animatedFrame);
        return;
    }
    if (m_composing) { m_composePending = true; return; }
    m_composing = true; m_composePending = false;
    const auto path = m_path;
    const auto sourceGeneration = m_sourceGeneration;
    const auto logical = m_logicalSize;
    const auto source = m_sourceFrame;
    const auto placement = m_placement;
    const auto scale = graphics::GraphicsPreferences::instance().backgroundScale();
    const auto dpr = m_devicePixelRatio * scale;
    const int blur = int(std::lround(m_blurRadius * dpr));
    const QSize pixels(std::max(1, int(std::ceil(m_logicalSize.width() * dpr))),
                       std::max(1, int(std::ceil(m_logicalSize.height() * dpr))));
    const QPointer<ThemeMediaBackground> guard(this);
    mediaPool().start([guard, path, sourceGeneration, logical, source, pixels, placement, blur, dpr, scale, animatedFrame] {
        auto decoration = source;
        if (scale != 1. && (placement == timelinebackgroundprefs::Placement::Center ||
                            placement == timelinebackgroundprefs::Placement::Tile)) {
            // Quality changes texture density, not the apparent image size
            // or repeat period. Fill/stretch already scale to the target.
            decoration = source.scaled(QSize(std::max(1, int(std::lround(source.width() * scale))),
                                              std::max(1, int(std::lround(source.height() * scale)))),
                                       Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        auto image = composeFrame(decoration, pixels, placement, blur);
        QMetaObject::invokeMethod(qApp, [guard, path, sourceGeneration, logical, placement, blur, image = std::move(image), dpr, scale, animatedFrame] {
            if (!guard) return;
            guard->m_composing = false;
            if (!guard->m_gpu && guard->m_sourceGeneration == sourceGeneration &&
                guard->m_path == path && guard->m_logicalSize == logical &&
                guard->m_placement == placement && guard->m_devicePixelRatio * scale == dpr &&
                graphics::GraphicsPreferences::instance().backgroundScale() == scale &&
                int(std::lround(guard->m_blurRadius * dpr)) == blur) {
                guard->m_frame = QPixmap::fromImage(image);
                guard->m_frame.setDevicePixelRatio(dpr);
                emit guard->frameChanged(animatedFrame);
            }
            if (guard->m_composePending) guard->rebuild(guard->m_pendingAnimated);
        }, Qt::QueuedConnection);
    });
}

QUrl ThemeMediaBackground::sourceUrl() const { return QUrl::fromLocalFile(m_path); }
bool ThemeMediaBackground::videoSource() const {
    return timelinebackgroundprefs::mediaKind(m_path) == timelinebackgroundprefs::MediaKind::Video;
}
double ThemeMediaBackground::textureScale() const { return graphics::GraphicsPreferences::instance().backgroundScale(); }
int ThemeMediaBackground::frameLimit() const {
    const int quality = graphics::GraphicsPreferences::instance().backgroundFps();
    const auto& clock = FrameClock::instance();
    const auto* widget = qobject_cast<QWidget*>(parent());
    const auto* screen = widget ? widget->screen() : QGuiApplication::primaryScreen();
    const double hz = screen ? screen->refreshRate() : 60.;
    const int displayCap = std::isfinite(hz) && hz > 0 ? int(std::ceil(hz)) : 60;
    const int cap = clock.mode() == FrameMode::Fixed ? clock.limit() :
                    clock.mode() == FrameMode::Display ? displayCap : 0;
    return quality && cap ? std::min(quality, cap) : std::max(quality, cap);
}
void ThemeMediaBackground::setCornerRadius(int radius) {
    if (m_cornerRadius == radius) return;
    m_cornerRadius = radius; emit gpuConfigurationChanged();
}
void ThemeMediaBackground::setGpuPresentation(bool enabled) {
    if (m_gpu == enabled) return;
    m_gpu = enabled;
    if (videoSource()) {
        const auto path = m_path;
        m_path.clear();
        setSource(path);
    } else rebuild();
}
void ThemeMediaBackground::paint(QPainter& painter, const QRectF& bounds) {
    setGpuPresentation(graphics::isSceneRecording(painter));
    if (m_gpu) paintVisual(painter, bounds);
    else painter.drawPixmap(bounds.topLeft(), m_frame);
}
QQuickItem* ThemeMediaBackground::createItem(QQmlEngine* engine, QQuickItem* parent) {
    graphics::registerMediaImageItem();
    setGpuPresentation(true);
    QQmlComponent component(engine, QUrl(QStringLiteral("qrc:/vlt/graphics/MediaBackground.qml")));
    auto* item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(
        {{QStringLiteral("media"), QVariant::fromValue(static_cast<QObject*>(this))}}));
    if (!item) { qWarning() << component.errors(); return nullptr; }
    QQmlEngine::setObjectOwnership(item, QQmlEngine::CppOwnership);
    item->setParent(parent); item->setParentItem(parent);
    return item;
}
void ThemeMediaBackground::releaseItem(QQuickItem*) {
    // Retain the raw decoder/image while hidden. Compatibility painting calls
    // setGpuPresentation(false) if the window falls back, without losing prefs.
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
