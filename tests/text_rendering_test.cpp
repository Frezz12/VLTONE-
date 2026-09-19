#include "graphics/SceneRecorder.hpp"
#include "graphics/SceneItem.hpp"
#include "graphics/RetainedScene.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QQuickWindow>
#include <QTimer>
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
}
int main(int argc, char** argv) try {
    QApplication app(argc, argv);
    require(QFontDatabase::addApplicationFont(DAW_TEST_FONT_PATH) >= 0, "load bundled Inter fixture");
    QFont font(QStringLiteral("Inter")); font.setPixelSize(10);
    for (qreal dpr : {1., 1.25, 1.5, 1.75, 2.}) {
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
    }
    std::cout << "PASS native text filtering, antialiasing, clipping and retained groups at 100/125/150/175/200%\n";
    if (!app.arguments().contains("--hardware")) return 0;

    // Test-only GPU readback: identical glyph pixels, old vs new sampling.
    // This exercises shared atlas textures with two different material filters.
    QQuickWindow window;
    window.setColor(QColor(20, 23, 28)); window.resize(480, 220);
    auto* item = new SceneItem(window.contentItem()); item->setSize(window.size());
    std::atomic<bool> resourceError{false};
    QObject::connect(item, &SceneItem::resourceError, &app,
        [&] { resourceError.store(true); }, Qt::DirectConnection);
    auto snapshot = std::make_shared<SceneSnapshot>(); snapshot->viewport = window.size();
    auto layer = std::make_shared<SceneLayer>(); layer->id = layer->revision = 1;
    layer->clip = QRectF(QPointF(), window.size());
    const auto dpr = window.devicePixelRatio();
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
    snapshot->layers.push_back(layer); item->setSnapshot(snapshot);
    window.show();
    QTimer::singleShot(500, &app, &QCoreApplication::quit); app.exec();
    const QImage frame = window.grabWindow();
    require(!frame.isNull(), "hardware frame available");
    const QString destination = app.arguments().value(app.arguments().indexOf("--hardware") + 1);
    if (!destination.isEmpty()) require(frame.save(destination), "save GPU comparison");
    require(!resourceError.load(), "hardware scene rendered");
    std::cout << "PASS GPU comparison captured: top=previous linear, bottom=native glyph texels\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n'; return 1;
}
