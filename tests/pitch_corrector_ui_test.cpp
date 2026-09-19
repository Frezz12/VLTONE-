#include "EngineController.hpp"
#include "Internal/PitchCorrectorInstance.hpp"
#include "PitchCorrectorPanel.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "graphics/BrowserSurface.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <QWebEngineView>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>

namespace {
int failures = 0;
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }
void events(int ms = 90) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
QVariant js(ui::graphics::BrowserPage* page, const QString& source) {
    struct Result { bool done = false; QVariant value; };
    auto result = std::make_shared<Result>();
    page->runJavaScript(source, [result](const QVariant& value) { result->done = true; result->value = value; });
    for (int i = 0; i < 100 && !result->done; ++i) events(10);
    if (!result->done) check(false, "JavaScript callback completes");
    events(); return result->value;
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& text) { std::fprintf(stderr,"Qt: %s\n",text.toUtf8().constData()); });
    ui::registerFontUrlScheme(); QtWebEngineQuick::initialize();
    QApplication app(argc, argv); ui::initializeApplicationFonts();
    QTemporaryDir temporary;
    app.setProperty("dawHeadlessDataRoot", temporary.path());
    app.setOrganizationName("VLTONE-Pitch-Test"); app.setApplicationName("PitchWeb");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    daw::EngineController controller;
    check(bool(controller.initialize(48000, 257, false)), "headless controller initializes");
    const auto track = controller.addTrack(daw::TrackKind::Audio, "Vocal");
    const auto insert = controller.addInsert(track, daw::plugins::pitch::PitchCorrectorInstance::staticDescriptor());
    check(!insert.empty(), "insert uses the standard host"); if (insert.empty()) return 1;
    const auto value = [&](const char* id) { return controller.insertParameter(track, insert, id); };
    controller.setInsertParameter(track, insert, "a4_hz", 442);
    PitchCorrectorPanel panel(&controller, QString::fromStdString(track), QString::fromStdString(insert));
    panel.resize(640, 410);
    std::unique_ptr<ui::graphics::WorkspaceSurface> workspace;
    if (qEnvironmentVariableIntValue("VLT_PITCH_TEST_WORKSPACE") == 1) {
        workspace = std::make_unique<ui::graphics::WorkspaceSurface>(&panel);
        QObject::connect(workspace.get(),&ui::graphics::WorkspaceSurface::failed,[](const QString& reason) {
            std::fprintf(stderr,"Workspace failure: %s\n",reason.toUtf8().constData());
            check(false,"GPU workspace remains available");
        });
    }
    panel.show();
    auto* view = panel.findChild<ui::graphics::BrowserSurface*>("PitchWebView");
    check(view != nullptr, "local HTML editor uses the shared browser surface"); if (!view) return 1;
    auto* page = view->page(); bool connected = false;
    std::printf("HTML backend: %s, resource: %d, URL: %s\n",page->isQuick()?"Quick":"Widgets",int(QFile::exists(":/vlt/pitch/index.html")),page->url().toString().toUtf8().constData());
    QObject::connect(page,&ui::graphics::BrowserPage::loadFinished,[](bool ok){ std::printf("HTML load finished: %d\n",int(ok)); });
    QObject::connect(page,&ui::graphics::BrowserPage::renderProcessTerminated,[](auto status,int code){ std::printf("HTML renderer terminated: %d / %d\n",int(status),code); });
    for (int i = 0; i < 50 && !connected; ++i) connected = js(page, "document.documentElement.dataset.connected === 'true'").toBool();
    check(connected, "embedded resources and WebChannel initialize without a server"); if (!connected) return 1;
    {
        auto& themes = ThemeManager::instance(); const Theme original = themes.theme();
        const auto accentMatches = [&] {
            return js(page,"getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()").toString() == th().accent.name();
        };
        check(accentMatches(), "HTML receives the current application accent on first load");
        const auto beforeColor = js(page,"getComputedStyle(document.querySelector('.pointer'),'::after').backgroundColor");
        Theme custom = original; custom.accent = QColor("#cf71dc"); themes.applyCustomTheme(custom,false); events();
        check(accentMatches() && beforeColor != js(page,"getComputedStyle(document.querySelector('.pointer'),'::after').backgroundColor"),
              "changing the application accent repaints the HTML controls without reopening");
        panel.hide(); themes.setThemeId("gruvbox",false); panel.show(); events();
        check(accentMatches(), "an editor reopened after a theme change uses the latest accent");
        themes.setThemeId(original.id,false); events();
    }
    check(js(page,"document.getElementById('retune-value').value === '20.0' && document.getElementById('humanize-value').value === '0.0' && document.getElementById('vibrato-value').value === '0.0'").toBool(), "new editor starts at 20 milliseconds with Humanize and Vibrato zero");
    check(js(page, "document.querySelectorAll('[role=slider]').length === 3 && document.querySelectorAll('.key').length === 12").toBool(), "three unique dials and all twelve piano notes");
    check(!page->navigationPolicy(QUrl("https://example.com"),true) && !page->navigationPolicy(QUrl("file:///C:/"),true), "editor rejects external and arbitrary local navigation");
    const double before = value("tune");
    js(page,"document.getElementById('preset').value='factory:3';document.getElementById('preset').dispatchEvent(new Event('change'))");
    check(value("tune") == 100 && value("humanize") == 0 && value("vibrato") == 0 && value("a4_hz") == 442, "HTML Hard preset preserves A4 and applies the three musical settings");
    check(js(page,"document.getElementById('retune-value').value === '0.0' && document.getElementById('retune').getAttribute('aria-valuetext') === '0.0 milliseconds'").toBool(), "hardest tuning displays zero milliseconds, including accessibility");
    controller.undo(); events(); check(value("tune") == before && value("humanize") == 0, "factory style is one undo group");
    controller.redo(); events();
    js(page,"document.getElementById('retune').dispatchEvent(new MouseEvent('dblclick',{bubbles:true}))");
    check(std::abs(daw::plugins::pitch::retuneMilliseconds(value("tune"))-20) < 1e-9, "double-click resets retune to the 20 ms default");
    controller.undo(); events();
    js(page,"document.getElementById('a4').value='432.1';document.getElementById('a4').dispatchEvent(new Event('change'))");
    check(std::abs(value("a4_hz")-432.1) < 1e-9, "A4 numeric entry preserves tenths of a Hertz");
    controller.undo(); events(); check(value("a4_hz") == 442, "numeric entry is undoable");
    js(page,"document.getElementById('retune-value').value='18';document.getElementById('retune-value').dispatchEvent(new Event('change'))");
    check(std::abs(value("tune")-70) < 1e-9, "18 ms maps to existing tune automation without reinterpretation");
    controller.undo(); events();
    js(page,"(()=>{const d=document.getElementById('retune');d.focus();for(let i=0;i<3;++i)d.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true}));d.dispatchEvent(new KeyboardEvent('keyup',{key:'ArrowUp',bubbles:true}));})()");
    check(std::abs(daw::plugins::pitch::retuneMilliseconds(value("tune"))-3) < 1e-8, "keyboard repeat changes the displayed millisecond value");
    controller.undo(); events(); check(value("tune") == 100, "a held keyboard gesture creates one undo action");
    js(page,"(()=>{const d=document.getElementById('humanize');d.focus();d.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',shiftKey:true}));d.dispatchEvent(new KeyboardEvent('keyup',{key:'ArrowUp'}));})()");
    check(std::abs(value("humanize")-.1) < 1e-9, "Shift keyboard adjustment retains fine precision");
    controller.undo(); events();
    // Exercise actual Chromium pointer capture, not just the bridge slots.
    {
        const auto rect = js(page,"(()=>{const r=document.getElementById('humanize').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").toList();
        const QPoint center(qRound(rect[0].toDouble()),qRound(rect[1].toDouble()));
        const auto drag = [&](auto* receiver) {
            QTest::mousePress(receiver,Qt::LeftButton,Qt::NoModifier,center); events();
            QTest::mouseMove(receiver,center-QPoint(0,20),30); QTest::mouseMove(receiver,center-QPoint(0,40),30);
            QTest::mouseRelease(receiver,Qt::LeftButton,Qt::NoModifier,center-QPoint(0,40)); events();
            check(value("humanize") >= 10 && value("humanize") <= 14, "native mouse dragging uses a controlled 320-pixel range");
            controller.undo(); events(); check(value("humanize") == 0, "complete pointer drag undoes in one step");

            // Repeated partial Chromium updates must neither erase the glass
            // keyboard nor rewind the value under a continuous pointer grab.
            double last = 0; bool monotonic = true, visible = true;
            controller.play(); events();
            QTest::mousePress(receiver,Qt::LeftButton,Qt::NoModifier,center); events();
            for (int step = 1; step <= 64; ++step) {
                QTest::mouseMove(receiver,center-QPoint(0,step*3),1); events(16);
                const double current = value("humanize");
                monotonic &= current+.01 >= last; last = current;
                if (step%8 == 0) {
                    const auto image = page->isQuick() ? page->quickItem()->window()->grabWindow() : panel.grab().toImage();
                    visible &= !image.isNull() && image.pixelColor(image.width()/2,image.height()*82/100).lightness() > 120;
                }
            }
            QTest::mouseRelease(receiver,Qt::LeftButton,Qt::NoModifier,center-QPoint(0,192)); events();
            check(monotonic && std::abs(value("humanize")-60) < .2,
                  "drag during playback stays monotonic outside the panel and commits its last position");
            check(visible, "partial repaints keep the keyboard visible throughout dragging");
            check(js(page,"document.getElementById('humanize-value').value === '60.0'").toBool(),
                  "readout settles to the exact final value without a stale echo");
            controller.stop(); events();
            controller.undo(); events(); check(value("humanize") == 0, "a sustained drag still has one undo step");
        };
        if (auto* native = view->findChild<QWebEngineView*>()) {
            auto* receiver = native->focusProxy(); if (!receiver) receiver = native; drag(receiver);
        } else if (page->isQuick() && page->quickItem()->window()) drag(page->quickItem()->window());
        else check(false,"browser exposes an input surface");
    }
    QString routed;
    QObject::connect(&panel,&PitchCorrectorPanel::automationRequested,[&](QString id) { routed = id; });
    js(page,"document.getElementById('retune').dispatchEvent(new MouseEvent('contextmenu',{bubbles:true,clientX:80,clientY:160}));document.getElementById('automate').click()");
    check(routed == "tune", "HTML context menu routes the stable automation ID");
    js(page,"document.getElementById('active').click()");
    check(controller.insertModel(track,insert)->bypassed, "power button uses host bypass");
    js(page,"document.getElementById('active').click()");
    controller.play(); events();
    check(js(page,"document.getElementById('quality').disabled").toBool(), "quality control is disabled during playback");
    auto* bridge = panel.findChild<PitchWebBridge*>(); bridge->edit(11,1,true); events();
    check(value("quality") == 0, "C++ also rejects live structural edits");
    controller.stop(); events();
    js(page,"document.getElementById('quality').value='1';document.getElementById('quality').dispatchEvent(new Event('change'))");
    check(value("quality") == 1, "idle HD selection uses host reconfiguration");
    const double oldScale = value("scale"), oldMask = value("note_mask");
    js(page,"document.getElementById('note-0').click()");
    check(value("scale") == 7 && int(value("note_mask")) == 4094 && js(page,"document.getElementById('note-0').getAttribute('aria-pressed') === 'false'").toBool(), "piano changes Custom mask and accessible state");
    controller.undo(); events(); check(value("scale") == oldScale && value("note_mask") == oldMask, "piano mask and scale undo together");
    controller.setInsertParameter(track,insert,"scale",7); controller.setInsertParameter(track,insert,"note_mask",1); events();
    js(page,"document.getElementById('note-0').click()"); check(value("note_mask") == 1, "cannot exclude the last allowed note");
    js(page,"document.getElementById('settings').click();document.getElementById('output').value='-13.5';document.getElementById('output').dispatchEvent(new Event('change'))");
    check(value("output_db") == -13.5, "HTML settings edit output in decibels");
    js(page,"document.getElementById('preset-name').value='My vocal';document.getElementById('save-preset').click()");
    check(QSettings().value("pitchCorrector/userPresets.v1").toByteArray().contains("My vocal"), "user preset persists in the existing library format");
    js(page,"document.getElementById('save-preset').click()");
    check(js(page,"!document.getElementById('replace-preset').hidden").toBool(), "replacing a named preset requires an explicit action");
    js(page,"document.getElementById('settings-dialog').close();document.getElementById('preset').value='factory:0';document.getElementById('preset').dispatchEvent(new Event('change'))");
    const double beforePreset = value("tune");
    js(page,"document.getElementById('preset').value='user:My vocal';document.getElementById('preset').dispatchEvent(new Event('change'))");
    check(value("tune") == 100, "saved web preset restores musical settings");
    controller.undo(); events(); check(value("tune") == beforePreset, "user preset loading has one undo step");
    auto* timer = panel.findChild<QTimer*>("PitchTelemetryTimer");
    panel.hide(); events(); check(timer && !timer->isActive(), "hidden panel stops telemetry updates");
    panel.show(); events(); check(timer && timer->isActive(), "reopened panel resumes telemetry");

    {
        const auto otherTrack = controller.addTrack(daw::TrackKind::Audio, "Double");
        const auto other = controller.addInsert(otherTrack, daw::plugins::pitch::PitchCorrectorInstance::staticDescriptor());
        controller.setInsertParameter(track,insert,"humanize",34);
        controller.setInsertParameter(track,insert,"key",7);
        controller.setInsertParameter(track,insert,"scale",2);
        events();
        const auto depth = controller.undoDepth();
        check(js(page,"document.getElementById('send-to-all').getAttribute('aria-label').includes('VLT Pitch')").toBool(),
              "send-to-all icon has a descriptive accessible label");
        js(page,"document.getElementById('send-to-all').click()");
        check(controller.insertParameter(otherTrack,other,"humanize") == 34 &&
                  controller.insertParameter(otherTrack,other,"key") == 7 &&
                  controller.insertParameter(otherTrack,other,"scale") == 2 && controller.undoDepth() == depth+1,
              "header send icon copies the current settings to another VLT Pitch");
        controller.undo(); events();
        check(controller.insertParameter(otherTrack,other,"key") == 0 && value("key") == 7,
              "send icon copies undo together without changing the source");
        check(bool(controller.applyKeyToPitchCorrectors(2,"major")), "analysis can update an already open pitch editor");
        events();
        check(js(page,"document.getElementById('key').value === '2' && document.getElementById('scale').value === '1'").toBool(),
              "open HTML editor refreshes after external key import");
    }

    for (const auto& p : daw::plugins::pitch::parameterTable()) controller.setInsertParameter(track,insert,p.id,p.defaultValue);
    for (int i = 0; i < 64; ++i) (void)controller.pumpPluginEvents();
    auto* processor = dynamic_cast<daw::plugins::pitch::PitchCorrectorInstance*>(controller.insertInstance(track,insert));
    if (processor) {
        std::array<float,257> in{},left{},right{}; const float* inputs[]{in.data(),in.data()}; float* outputs[]{left.data(),right.data()};
        daw::plugins::PluginProcessContext context; context.inputs=inputs;context.outputs=outputs;context.inputChannels=context.outputChannels=2;context.frames=257;
        processor->startProcessing();
        for (int block = 0; block < 256; ++block) {
            for (int i = 0; i < 257; ++i) in[i] = float(.2*std::sin(2*std::numbers::pi*215.597*(block*257+i)/48000));
            processor->process(context);
        }
        events(); check(js(page,"document.getElementById('note-9').classList.contains('current')").toBool(), "real DSP telemetry lights the target key");
    }
    QString screenshots;
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--screenshots") { screenshots=QString::fromLocal8Bit(argv[2]); QDir().mkpath(screenshots); }
    for (QSize size : {QSize(640,410),QSize(560,360)}) {
        panel.resize(size); events(300);
        check(js(page,"document.documentElement.scrollWidth <= innerWidth && document.documentElement.scrollHeight <= innerHeight && [...document.querySelectorAll('.dial,.key,.configuration,footer,header button,header select')].every(e=>{const r=e.getBoundingClientRect();return r.left>=0 && r.top>=0 && r.right<=innerWidth && r.bottom<=innerHeight})").toBool(), "controls fit default and minimum sizes without scrolling");
        const auto shot = page->isQuick() && page->quickItem()->window()
            ? page->quickItem()->window()->grabWindow() : panel.grab().toImage();
        const auto keyPoint = js(page,"(()=>{const r=document.getElementById('note-0').getBoundingClientRect();return [r.left+r.width*.3,r.top+r.height*.7]})()").toList();
        const double pixelScale = shot.width()/double(size.width());
        check(!shot.isNull() && shot.pixelColor(shot.width()/2,shot.height()*7/100).lightness() < 120 &&
              shot.pixelColor(qRound(keyPoint[0].toDouble()*pixelScale),qRound(keyPoint[1].toDouble()*pixelScale)).lightness() > 120,
              "browser renders the dark panel and dimensional light keys");
        if (!screenshots.isEmpty()) check(shot.save(screenshots+QString("/pitch-%1x%2.png").arg(size.width()).arg(size.height())), "HTML editor screenshot saves");
    }
    if (!screenshots.isEmpty()) {
        panel.resize(640,410);
        for (const auto& id : {QStringLiteral("gruvbox"),QStringLiteral("light")}) {
            ThemeManager::instance().setThemeId(id,false); events(300);
            const auto shot = page->isQuick() && page->quickItem()->window()
                ? page->quickItem()->window()->grabWindow() : panel.grab().toImage();
            check(shot.save(screenshots+"/pitch-theme-"+id+".png"), "theme preview saves");
        }
    }
    return failures ? 1 : 0;
}
