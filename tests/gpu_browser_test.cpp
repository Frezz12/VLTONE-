#include "graphics/BrowserSurface.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "Typography.hpp"
#include <QApplication>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTest>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <cstdio>

int main(int argc, char** argv) {
    qputenv("VLT_GPU_WORKSPACE", "1");
    ui::registerFontUrlScheme();
    QtWebEngineQuick::initialize();
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    // Use isolated paths for Chromium too; never touch the user's profile.
    app.setProperty("dawHeadlessDataRoot", settings.path());
    app.setOrganizationName("VltGpuBrowserTest"); app.setApplicationName("Browser");
    ui::graphics::BrowserProfile profile(&app);
    QWidget originalRoot, detachedRoot;
    originalRoot.resize(500, 320); detachedRoot.resize(500, 320);
    ui::graphics::BrowserSurface view(&profile, &originalRoot);
    view.resize(500, 320);
    view.page()->internalNavigationAllowed = [](const QUrl& url) { return url.scheme() == "data"; };
    bool loaded = false, failed = false;
    QObject::connect(&view, &ui::graphics::BrowserSurface::loadFinished, &app, [&](bool ok) { loaded = ok; });
    auto surface = std::make_unique<ui::graphics::WorkspaceSurface>(&originalRoot);
    QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::failed, &app, [&](const QString& why) { qWarning() << why; failed = true; });
    view.setHtml("<!doctype html><title>GPU browser fixture</title><body style='background:#ff0000'>"
                 "<input id='entry' aria-label='Test input'><script>window.answer=42;window.events=[];"
                 "for(const type of ['mousedown','mouseup','click','keydown'])addEventListener(type,e=>events.push([type,e.clientX,e.clientY,e.target.id,e.key]));</script></body>",
                 QUrl("https://vlt-fixture.invalid/"));
    view.show(); originalRoot.show();
    const auto settle = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    for (int i = 0; i < 50 && !loaded && !failed; ++i) settle(100);
    if (!loaded || failed || view.title() != "GPU browser fixture") return 1;
    const auto frame = view.page()->mainFrame();
    if (!frame || !frame->isValid()) { std::puts("Quick mainFrame was not exposed"); return 2; }
    int answer = 0;
    auto readable = *frame;
    readable.runJavaScript("window.answer", [&answer](const QVariant& value) { answer = value.toInt(); });
    for (int i = 0; i < 20 && !answer; ++i) settle(50);
    if (answer != 42) return 3;
    const auto click = [&](QPointF point) {
        auto* window = surface->quickWindow();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
    };
    surface->quickWindow()->requestActivate(); settle(300);
    QTest::mouseMove(surface->quickWindow(), QPoint(40, 18)); settle(100);
    click(QPointF(40, 18)); settle(100);
    QTest::keyClick(surface->quickWindow(), Qt::Key_A); settle(100);
    QString entered;
    readable.runJavaScript("document.getElementById('entry').value", [&entered](const QVariant& value) { entered = value.toString(); });
    for (int i = 0; i < 20 && entered.isEmpty(); ++i) settle(50);
    if (entered != "a") {
        qWarning() << "Quick keyboard input failed" << entered << QApplication::focusWidget()
                   << surface->quickWindow()->activeFocusItem() << QGuiApplication::focusWindow();
        const std::function<void(QQuickItem*, int)> dump = [&](QQuickItem* item, int depth) {
            qWarning() << depth << item << item->z() << item->isVisible() << item->isEnabled() << item->acceptedMouseButtons();
            for (auto* child : item->childItems()) dump(child, depth + 1);
        };
        dump(surface->quickWindow()->contentItem(), 0);
        readable.runJavaScript("JSON.stringify({active:document.activeElement.id,rect:document.getElementById('entry').getBoundingClientRect().toJSON(),focused:document.hasFocus(),events})",
                               [](const QVariant& value) { qWarning() << value; });
        settle(100);
        return 9;
    }
    QLineEdit overlay(&view); overlay.setGeometry(10, 40, 150, 30); overlay.show(); settle(100);
    click(QPointF(40, 55));
    QTest::keyClick(surface->quickWindow(), Qt::Key_A); settle(100);
    if (overlay.text() != "a" || view.ownsQuickFocus()) return 10;
    // Editor tabs restore focus without a mouse press. The window container
    // must not restore its stale line-edit proxy when Quick activates again.
    view.setFocus(Qt::OtherFocusReason); settle(200);
    if (!view.ownsQuickFocus()) return 11;
    QTest::keyClick(surface->quickWindow(), Qt::Key_B); settle(100);
    entered.clear();
    readable.runJavaScript("document.getElementById('entry').value", [&entered](const QVariant& value) { entered = value.toString(); });
    for (int i = 0; i < 20 && entered.isEmpty(); ++i) settle(50);
    if (entered != "ab" || overlay.text() != "a") return 12;
    overlay.hide();
    settle(300);
    const auto image = surface->quickWindow()->grabWindow();
    if (image.isNull() || image.pixelColor(image.width() / 2, image.height() / 2).red() < 220) return 4;
    // Hide/reopen retains the actual page and its Javascript state/history.
    view.hide(); settle(100); view.show(); settle(300);
    answer = 0;
    readable.runJavaScript("window.answer", [&answer](const QVariant& value) { answer = value.toInt(); });
    for (int i = 0; i < 20 && !answer; ++i) settle(50);
    if (answer != 42 || failed) return 5;
    // Force the new window to claim the persistent page before the old window
    // consumes its removal. Late teardown must not detach the new owner's page.
    view.setParent(&detachedRoot); view.show(); detachedRoot.show();
    auto detached = std::make_unique<ui::graphics::WorkspaceSurface>(&detachedRoot);
    auto* transferred = view.page()->createItem(nullptr, detached->quickWindow()->contentItem());
    surface.reset();
    if (!transferred || transferred->parentItem() != detached->quickWindow()->contentItem() || !transferred->isVisible()) return 13;
    surface = std::move(detached); settle(500);
    const auto detachedImage = surface->quickWindow()->grabWindow();
    if (view.page()->quickItem()->window() != surface->quickWindow() || detachedImage.isNull() ||
        detachedImage.pixelColor(detachedImage.width()/2, detachedImage.height()/2).red() < 220) return 14;
    surface.reset();
    view.update(); settle(500);
    if (!view.page()->quickItem()->window()) return 6;
    const auto fallbackFrame = view.page()->quickItem()->window()->grabWindow();
    if (fallbackFrame.isNull() || fallbackFrame.pixelColor(fallbackFrame.width() / 2, fallbackFrame.height() / 2).red() < 220)
        return 8;
    answer = 0;
    readable.runJavaScript("window.answer", [&answer](const QVariant& value) { answer = value.toInt(); });
    for (int i = 0; i < 20 && !answer; ++i) settle(50);
    if (answer != 42) return 7;
    std::puts("Quick browser: frame API, composition, focus, hidden-page lifetime, window transfer and compatibility fallback passed");
    return 0;
}
