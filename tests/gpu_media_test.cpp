#include "ThemeMediaBackground.hpp"
#include "graphics/ScenePaintSource.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include "graphics/VideoFrameGate.hpp"
#include "UiFrameClock.hpp"
#include <QApplication>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QSettings>
#include <QEventLoop>
#include <QTimer>
#include <QVideoFrameFormat>
#include <cstdio>

class MediaCanvas : public ui::FrameWidget, public ui::graphics::ScenePaintSource {
public:
    ui::ThemeMediaBackground media{this};
    MediaCanvas() {
        connect(&media, &ui::ThemeMediaBackground::frameChanged, this, [this] { update(); });
    }
    void paintScene(QPainter& p, const QRegion&) override {
        p.fillRect(rect(), Qt::blue);
        if (media.hasFrame()) media.paint(p, rect());
        p.fillRect(QRect(80, 40, 80, 80), Qt::green);
    }
    void paintEvent(QPaintEvent*) override { QPainter p(this); paintScene(p, QRegion(rect())); }
};
int main(int argc, char** argv) {
    qputenv("VLT_GPU_WORKSPACE", "1");
    QApplication app(argc, argv);
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    app.setOrganizationName("VltTest"); app.setApplicationName("GpuMedia");
    const bool hardware = app.arguments().contains("--hardware");
    const auto settle = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    {
        ui::graphics::VideoFrameGate gate;
        QVideoSink output;
        gate.setTarget(&output); gate.setFrameLimit(10);
        int frames = 0;
        QObject::connect(&output, &QVideoSink::videoFrameChanged, &app, [&](const QVideoFrame&) { ++frames; });
        const auto burst = [&](int first) {
            for (int i = first; i < first + 128; ++i) {
                QVideoFrame frame(QVideoFrameFormat(QSize(4, 4), QVideoFrameFormat::Format_RGBA8888));
                frame.setStartTime(i);
                gate.videoSink()->setVideoFrame(frame);
            }
        };
        burst(0); settle(10);
        if (frames != 1 || output.videoFrame().startTime() != 127) return 10;
        burst(128); settle(10);
        if (frames != 1) return 11;
        settle(120);
        if (frames != 2 || output.videoFrame().startTime() != 255) return 12;
        gate.videoSink()->setVideoFrame({}); settle(120);
        if (output.videoFrame().isValid()) return 13;
    }
    QImage image(64, 64, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::red);
    const QString path = dir.filePath("source.png");
    if (!image.save(path)) return 1;
    QImage replacement(64, 64, QImage::Format_RGB32); replacement.fill(Qt::yellow);
    const QString replacementPath = dir.filePath("replacement.jpg");
    if (!replacement.save(replacementPath, "JPG", 95)) return 1;
    ui::timelinebackgroundprefs::setEnabled(false);
    ui::headerbackgroundprefs::setEnabled(false);
    if (!ui::timelinebackgroundprefs::setPath(path) ||
        !ui::timelinebackgroundprefs::enabled() ||
        !ui::headerbackgroundprefs::setPath(replacementPath) ||
        !ui::headerbackgroundprefs::enabled()) {
        qWarning() << "Choosing background media did not activate it";
        return 1;
    }
    MediaCanvas canvas; canvas.resize(320, 180);
    canvas.media.setTargetSize(canvas.size(), 1);
    canvas.media.setSource(path);
    settle(200);
    QQmlEngine engine;
    QQuickItem parent;
    auto* component = canvas.media.createItem(&engine, &parent);
    if (!component) return 2;
    delete component;
    if (!hardware) return 0;
    bool failed = false;
    ui::graphics::WorkspaceSurface surface(&canvas);
    QObject::connect(&surface, &ui::graphics::WorkspaceSurface::failed, &app, [&](const QString& error) {
        qWarning() << error; failed = true;
    });
    canvas.show(); settle(800);
    for (const auto quality : {ui::graphics::Quality::Maximum, ui::graphics::Quality::Medium, ui::graphics::Quality::Low}) {
        ui::graphics::GraphicsPreferences::instance().setQuality(quality);
        settle(150);
        const auto frame = surface.quickWindow()->grabWindow();
        if (failed || frame.isNull()) return 3;
        const qreal dpr = surface.quickWindow()->devicePixelRatio();
        if (frame.pixelColor(int(30 * dpr), int(60 * dpr)).red() < 230 ||
            frame.pixelColor(int(100 * dpr), int(60 * dpr)).green() < 230) {
            frame.save(dir.filePath("failed.png"));
            dir.setAutoRemove(false);
            qWarning() << "Media compositing/order failed" << dir.path(); return 4;
        }
    }
    canvas.media.setBlurRadius(6);
    canvas.media.setCornerRadius(10);
    settle(200);
    const auto effectedFrame = surface.quickWindow()->grabWindow();
    const qreal effectDpr = surface.quickWindow()->devicePixelRatio();
    if (effectedFrame.isNull() ||
        effectedFrame.pixelColor(int(30 * effectDpr), int(60 * effectDpr)).red() < 180) {
        qWarning() << "Background shader path produced no image";
        return 4;
    }
    canvas.media.setBlurRadius(0);
    canvas.media.setCornerRadius(0);
    canvas.media.setSource(QStringLiteral(DAW_TEST_VIDEO));
    canvas.media.setPlaying(true);
    settle(1000);
    if (failed) return 5;
    auto* video = surface.quickWindow()->contentItem()->findChild<QObject*>("GpuMediaBackground");
    if (!video || !video->property("firstVideoFrame").toBool()) {
        for (auto* child : surface.quickWindow()->contentItem()->findChildren<QObject*>()) {
            if (child->objectName() == "GpuMediaPlayer")
                qWarning() << child->property("source") << child->property("playbackState")
                           << child->property("mediaStatus") << child->property("errorString");
        }
        qWarning() << "VideoOutput received no decoded frame"; return 6;
    }
    const auto videoFrame = surface.quickWindow()->grabWindow();
    if (videoFrame.isNull()) return 7;
    canvas.media.setSource(replacementPath);
    settle(500);
    const auto replacementFrame = surface.quickWindow()->grabWindow();
    const qreal dpr = surface.quickWindow()->devicePixelRatio();
    const auto replacementPixel = replacementFrame.pixelColor(int(30 * dpr), int(60 * dpr));
    if (replacementFrame.isNull() || replacementPixel.red() < 200 || replacementPixel.green() < 200) {
        replacementFrame.save(dir.filePath("replacement-failed.png"));
        dir.setAutoRemove(false);
        qWarning() << "Live video-to-image source replacement failed" << dir.path();
        return 8;
    }
    std::puts("GPU image, live source replacement, profiles, compositing order and VideoOutput passed");
    return 0;
}
