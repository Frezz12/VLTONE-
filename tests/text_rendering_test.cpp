#include "graphics/SceneRecorder.hpp"
#include "graphics/SceneItem.hpp"
#include "graphics/RetainedScene.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QScopeGuard>
#include <rhi/qrhi.h>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace ui::graphics;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<SceneMesh> text(QFont font, qreal dpr, QPointF origin, bool scaled = false) {
    SceneRecorder recorder(QSize(480, 160), dpr);
    QPainter p(&recorder);
    p.setFont(font); p.setPen(Qt::white);
    p.translate(origin);
    if (scaled) p.scale(1.5, 1.5);
    p.drawText(QPointF(6.25, 22.5), QStringLiteral("Pan  Fixed  Pattern - add a sound"));
    p.end();
    require(recorder.supported(), "text recording supported");
    return recorder.takeMeshes();
}
void checkText(const std::vector<SceneMesh>& meshes, qreal dpr) {
    require(!meshes.empty(), "font must generate glyphs, not a skipped test");
    bool antialiasing = false;
    for (const auto& mesh : meshes) {
        require(!mesh.texture.isNull() && !mesh.smoothTexture, "native text must not be filtered twice");
        const auto& v = mesh.vertices;
        require(v.size() == 6, "unclipped glyph quad");
        require(std::abs((v[1].x - v[0].x) * dpr - mesh.texture.width()) < .002 &&
                std::abs((v[2].y - v[0].y) * dpr - mesh.texture.height()) < .002,
                "text texture must retain one texel per device pixel");
        for (int y = 0; y < mesh.texture.height(); ++y)
            for (int x = 0; x < mesh.texture.width(); ++x) {
                const int alpha = mesh.texture.pixelColor(x, y).alpha();
                antialiasing |= alpha > 0 && alpha < 255;
            }
    }
    require(antialiasing, "glyph edge antialiasing must remain enabled");
}
void checkRasterText(QFont font, qreal dpr) {
    const QSize size(480, 160);
    const QPointF baseline(20 / dpr, 40 / dpr);
    const QString label = QStringLiteral("Pan Fixed Pattern — Ноты Файл До#");
    SceneRecorder recorder(size, dpr);
    {
        QPainter p(&recorder); p.setFont(font); p.setPen(Qt::white);
        p.translate(baseline); p.drawText(QPointF(), label);
    }
    QImage reference(size * dpr, QImage::Format_ARGB32_Premultiplied);
    reference.setDevicePixelRatio(dpr); reference.fill(Qt::transparent);
    {
        QPainter p(&reference); p.setFont(font); p.setPen(Qt::white);
        p.translate(baseline); p.drawText(QPointF(), label);
    }
    QImage recorded(reference.size(), reference.format());
    recorded.setDevicePixelRatio(dpr); recorded.fill(Qt::transparent);
    {
        QPainter p(&recorded);
        for (const auto& mesh : recorder.takeMeshes())
            p.drawImage(QPointF(std::round(mesh.vertices[0].x * dpr) / dpr,
                               std::round(mesh.vertices[0].y * dpr) / dpr), mesh.texture);
    }
    if (recorded != reference) {
        std::cerr << "Raster mismatch at DPR " << dpr << '\n';
        const auto directory = qEnvironmentVariable("DAW_TEXT_CHECK_DIR");
        if (!directory.isEmpty()) {
            reference.save(directory + QStringLiteral("/reference-%1.png").arg(dpr));
            recorded.save(directory + QStringLiteral("/recorded-%1.png").arg(dpr));
        }
    }
    require(recorded == reference, "cached glyphs must match native raster text at a device-aligned baseline");
}
}
int main(int argc, char** argv) try {
    QApplication app(argc, argv);
    require(QFontDatabase::addApplicationFont(DAW_TEST_FONT_PATH) >= 0, "load bundled Inter fixture");
    QFont font(QStringLiteral("Inter")); font.setPixelSize(10);
    SceneRecorder fractional(QSize(480, 160), 1.3);
    require(fractional.devicePixelRatioF() == 1.3, "paint device must report the exact fractional DPR");
    RetainedScene movingBetweenScreens;
    for (qreal dpr : {1., 1.25, 1.5, 1.75, 2.}) {
        checkRasterText(font, dpr);
        for (QPointF origin : {QPointF(), QPointF(.25, .5), QPointF(17.75, 9.25)})
            checkText(text(font, dpr, origin), dpr);
        const auto scaled = text(font, dpr, {}, true);
        require(!scaled.empty() && scaled.front().smoothTexture, "scaled text retains resampling");
        SceneRecorder recorder(QSize(480, 160), dpr);
        QPainter p(&recorder);
        QImage art(8, 8, QImage::Format_ARGB32_Premultiplied); art.fill(Qt::red);
        p.drawImage(QRectF(0, 0, 15, 15), art);
        p.setClipRect(QRectF()); // Fully clipped text must not change the preceding image.
        p.drawText(QPointF(4, 20), QStringLiteral("Hidden"));
        p.end();
        const auto images = recorder.takeMeshes();
        require(images.size() == 1 && images.front().smoothTexture, "artwork filtering must remain enabled");
        SceneRecorder retained(QSize(480, 160), dpr);
        QPainter rp(&retained); rp.setFont(font);
        RetainedScene cache;
        const auto draw = [](QPainter& painter) {
            painter.setPen(Qt::white);
            painter.drawText(QPointF(6.25, 22.5), QStringLiteral("Pattern - add a sound"));
        };
        cache.paint(rp, 1, QSize(480, 160), QPointF(3.25, .5), draw);
        cache.paint(rp, 1, QSize(480, 160), QPointF(8.75, .25), draw);
        rp.end();
        const auto groups = retained.takeMeshes();
        require(groups.size() == 2 && groups[0].group == groups[1].group && cache.builds() == 1,
                "scrolling reuses the glyph group");
        checkText(groups[0].group->meshes, dpr);
        SceneRecorder moved(QSize(480, 160), dpr);
        QPainter mp(&moved); mp.setFont(font);
        movingBetweenScreens.paint(mp, 1, QSize(480, 160), {}, draw);
        mp.end();
        const auto movedGroups = moved.takeMeshes();
        checkText(movedGroups.front().group->meshes, dpr);
    }
    std::cout << "PASS native text filtering, antialiasing, clipping and retained groups at 100/125/150/175/200%\n";
    if (!app.arguments().contains("--hardware")) return 0;

    // Test-only GPU readback: identical glyph pixels, old vs new sampling.
    // This exercises shared atlas textures with two different material filters.
    QQuickRenderControl renderControl;
    renderControl.setSamples(4);
    QQuickWindow window(&renderControl);
    window.setColor(QColor(20, 23, 28)); window.resize(480, 220);
    auto* item = new SceneItem(window.contentItem()); item->setSize(window.size());
    std::atomic<bool> resourceError{false};
    QObject::connect(item, &SceneItem::resourceError, &app,
        [&] { resourceError.store(true); }, Qt::DirectConnection);
    auto snapshot = std::make_shared<SceneSnapshot>(); snapshot->viewport = window.size();
    auto layer = std::make_shared<SceneLayer>(); layer->id = layer->revision = 1;
    layer->clip = QRectF(QPointF(), window.size());
    const auto dpr = window.devicePixelRatio();
    const int noteX = qRound(300 * dpr), noteY = qRound(20 * dpr);
    for (int row = 0; row < 2; ++row) {
        const auto append = [&](QFont f, QPointF origin) {
            auto glyphs = text(f, dpr, origin);
            for (auto& mesh : glyphs) {
                mesh.smoothTexture = row == 0;
                layer->meshes.push_back(std::move(mesh));
            }
        };
        append(font, QPointF(14, 12 + row * 100));
        auto larger = font; larger.setPixelSize(12);
        append(larger, QPointF(14.25, 40 + row * 100));
    }
    snapshot->layers.push_back(layer);
    // Widget placement and retained scrolling each contribute a fractional
    // translation. Straight edges must still land on complete device pixels.
    SceneRecorder notes(QSize(480, 220), dpr);
    {
        QPainter p(&notes); p.setPen(Qt::NoPen); p.setBrush(Qt::white);
        p.fillRect(QRectF(noteX / dpr, noteY / dpr, 100 / dpr, 20 / dpr), Qt::white);
        p.setRenderHint(QPainter::Antialiasing);
        p.drawRoundedRect(QRectF(noteX / dpr, (noteY + 40) / dpr, 100 / dpr, 20 / dpr), 4 / dpr, 4 / dpr);
    }
    auto noteGroup = std::make_shared<SceneMeshGroup>(); noteGroup->meshes = notes.takeMeshes();
    SceneMesh movingNotes; movingNotes.group = noteGroup;
    movingNotes.transform.translate(.3 / dpr, .3 / dpr);
    auto noteLayer = std::make_shared<SceneLayer>(); noteLayer->id = 2; noteLayer->revision = 1;
    noteLayer->origin = QPointF(.3 / dpr, .3 / dpr);
    noteLayer->clip = layer->clip; noteLayer->meshes.push_back(movingNotes);
    snapshot->layers.push_back(noteLayer); item->setSnapshot(snapshot);
    // QQuickWindow::grabWindow() on a hidden window uses DPR 1 and disables
    // MSAA in Qt 6.8. Use an explicit render target to test real DPI and 4x AA.
    require(renderControl.initialize(), "initialize hardware render control");
    auto* rhi = renderControl.rhi();
    require(rhi && rhi->supportedSampleCounts().contains(4), "hardware supports 4x MSAA");
    const QSize pixels = window.size() * dpr;
    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA8, pixels, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    std::unique_ptr<QRhiRenderBuffer> color(rhi->newRenderBuffer(QRhiRenderBuffer::Color, pixels, 4));
    std::unique_ptr<QRhiRenderBuffer> depth(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixels, 4));
    require(texture->create() && color->create() && depth->create(), "create MSAA readback buffers");
    QRhiColorAttachment attachment(color.get()); attachment.setResolveTexture(texture.get());
    QRhiTextureRenderTargetDescription description;
    description.setColorAttachments({attachment}); description.setDepthStencilBuffer(depth.get());
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget(description));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    require(target->create(), "create MSAA render target");
    auto quickTarget = QQuickRenderTarget::fromRhiRenderTarget(target.get());
    quickTarget.setDevicePixelRatio(dpr); window.setRenderTarget(quickTarget);
    const auto cleanup = qScopeGuard([&] { window.setRenderTarget({}); renderControl.invalidate(); });
    renderControl.polishItems(); renderControl.beginFrame(); renderControl.sync(); renderControl.render();
    QRhiReadbackResult readback;
    auto* updates = rhi->nextResourceUpdateBatch();
    updates->readBackTexture(QRhiReadbackDescription(texture.get()), &readback);
    renderControl.commandBuffer()->resourceUpdate(updates);
    renderControl.endFrame(); rhi->finish();
    QImage frame(reinterpret_cast<const uchar*>(readback.data.constData()),
                 readback.pixelSize.width(), readback.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied);
    frame = rhi->isYUpInFramebuffer() ? frame.mirrored() : frame.copy();
    frame.setDevicePixelRatio(dpr);
    require(!frame.isNull(), "hardware frame available");
    const QString destination = app.arguments().value(app.arguments().indexOf("--hardware") + 1);
    if (!destination.isEmpty()) require(frame.save(destination), "save GPU comparison");
    const QColor background(20, 23, 28);
    for (int y : {noteY, noteY + 40}) {
        require(frame.pixelColor(noteX + 50, y - 1) == background &&
                frame.pixelColor(noteX + 50, y) == QColor(Qt::white) &&
                frame.pixelColor(noteX + 50, y + 19) == QColor(Qt::white) &&
                frame.pixelColor(noteX + 50, y + 20) == background,
                "flat and rounded note edges must stay sharp after fractional widget/tile translation");
    }
    bool roundedAntialiasing = false;
    for (int y = noteY + 40; y < noteY + 45; ++y)
        for (int x = noteX; x < noteX + 5; ++x) {
            const auto color = frame.pixelColor(x, y);
            roundedAntialiasing |= color != background && color != QColor(Qt::white);
        }
    require(roundedAntialiasing, "rounded note corners must retain antialiasing");
    require(frame.pixelColor(noteX, noteY + 40) == background,
            "the note corner must remain rounded");
    bool visibleText = false;
    for (int y = 0; y < int(180 * dpr); ++y)
        for (int x = 0; x < int(250 * dpr); ++x)
            visibleText |= frame.pixelColor(x, y) != background;
    require(visibleText, "GPU readback must contain rendered text, not a blank offscreen frame");
    require(!resourceError.load(), "hardware scene rendered");
    std::cout << "PASS GPU text and sharp note edges at DPR " << dpr
              << " with 4x MSAA"
              << " (top=linear sampling, bottom=native glyph texels)\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
}
