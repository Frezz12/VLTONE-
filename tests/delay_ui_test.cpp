#include "DelayPanel.hpp"
#include "EngineController.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "graphics/BrowserSurface.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
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
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }
void events(int ms = 65) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
QVariant js(ui::graphics::BrowserPage* page, const QString& script) {
    struct Result { bool done = false; QVariant value; }; auto r = std::make_shared<Result>();
    page->runJavaScript(script, [r](const QVariant& v) { r->done = true; r->value = v; });
    for (int i = 0; i < 100 && !r->done; ++i) events(10);
    check(r->done, "JavaScript callback completes"); events(); return r->value;
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ui::registerFontUrlScheme(); QtWebEngineQuick::initialize(); QApplication app(argc, argv); ui::initializeApplicationFonts();
    QTemporaryDir temporary; app.setProperty("dawHeadlessDataRoot", temporary.path());
    app.setOrganizationName("VLTONE-Delay-Test"); app.setApplicationName("DelayWeb");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    daw::EngineController controller; check(bool(controller.initialize(48000, 257, false)), "headless engine initializes");
    const auto descriptor = daw::plugins::delay::DelayInstance::staticDescriptor();
    audio::AudioBuffer liveSource(2, 192000);
    for (unsigned i = 0; i < 192000; ++i) liveSource.getChannel(0)[i] = liveSource.getChannel(1)[i] = float(.4 * std::sin(2 * std::numbers::pi * 431 * i / 48000));
    const auto livePath = temporary.path().toStdString() + "/gesture-source.wav";
    audio::AudioRecorder writer; writer.initialize(48000, 2); writer.writeWAVFile(livePath, liveSource, 48000);
    const auto track = controller.importAudioToNewTrack(livePath, 0); const auto insert = controller.addInsert(track, descriptor);
    check(!insert.empty(), "Delay inserts through normal host"); if (insert.empty()) return 1;
    const auto master = std::string(daw::EngineController::kMasterChannelId);
    const auto masterInsert = controller.addInsert(master, descriptor);
    check(!masterInsert.empty() && controller.insertParameter(master, masterInsert, "feedback") == 35, "Delay inserts on the master bus");
    controller.removeInsert(master, masterInsert);
    const auto value = [&](const char* id) { return controller.insertParameter(track, insert, id); };
    DelayPanel panel(&controller, QString::fromStdString(track), QString::fromStdString(insert)); panel.resize(1100, 460);
    std::unique_ptr<ui::graphics::WorkspaceSurface> workspace;
    if (qEnvironmentVariableIntValue("VLT_DELAY_TEST_WORKSPACE") == 1) {
        workspace = std::make_unique<ui::graphics::WorkspaceSurface>(&panel);
        QObject::connect(workspace.get(), &ui::graphics::WorkspaceSurface::failed, [](const QString& reason) { std::fprintf(stderr, "%s\n", reason.toUtf8().constData()); check(false, "GPU workspace stays available"); });
    }
    panel.show(); auto* view = panel.findChild<ui::graphics::BrowserSurface*>("DelayWebView");
    check(view != nullptr, "panel uses BrowserSurface"); if (!view) return 1; auto* page = view->page();
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) connected = js(page, "document.documentElement.dataset.connected === 'true'").toBool();
    check(connected, "resources and WebChannel connect"); if (!connected) return 1;
    std::printf("Backend %s, scale %.2f\n", page->isQuick() ? "GPU Quick" : "Widgets", panel.devicePixelRatioF());
    check(js(page, "document.querySelectorAll('.dial').length===9 && document.querySelectorAll('#character option').length===7 && document.querySelectorAll('#division option').length===23 && document.getElementById('time-unit').textContent==='375.0 ms'").toBool(), "nine accessible knobs, seven characters and musical timing defaults");
    check(!page->navigationPolicy(QUrl("https://example.com"), true), "external navigation is rejected");
    const auto background = js(page, "getComputedStyle(document.getElementById('panel')).backgroundImage");
    ThemeManager::instance().setThemeId("light", false); events(); ThemeManager::instance().setThemeId("gruvbox", false); events();
    check(js(page, "getComputedStyle(document.getElementById('panel')).backgroundImage") == background, "yellow finish survives DAW theme changes");
    controller.setTempo(90); events();
    check(js(page, "document.getElementById('time-unit').textContent==='500.0 ms' && document.getElementById('bpm-value').value==='90.0'").toBool(), "host tempo reaches centre display");
    js(page, "document.getElementById('local').click()");
    check(value("timeMode") == 1 && !js(page, "document.getElementById('tap').disabled").toBool(), "local tempo enables Tap");
    js(page, "document.getElementById('tap').click()"); events(420); js(page, "document.getElementById('tap').click()");
    check(value("bpm") > 70 && value("bpm") < 160 && controller.tempo() == 90, "Tap edits local tempo without changing project tempo");
    js(page, "(()=>{document.getElementById('milliseconds').click();const x=document.getElementById('timeMs-value');x.value='100';x.dispatchEvent(new Event('change'));x.blur();})()");
    check(value("timeMode") == 2 && value("timeMs") == 100, "millisecond mode sends time to DSP");
    js(page, "(()=>{const x=document.getElementById('feedback-value');x.focus();x.value='42.7';})()"); events(160);
    check(js(page, "document.getElementById('feedback-value').value==='42.7'").toBool(), "telemetry preserves active numeric text");
    js(page, "document.getElementById('feedback-value').dispatchEvent(new Event('change'));document.getElementById('feedback-value').blur()");
    check(value("feedback") == 42.7, "numeric edit reaches host"); controller.undo(); events(); check(value("feedback") == 35, "numeric edit undoes once"); controller.redo(); events(); check(value("feedback") == 42.7, "numeric edit redoes");
    js(page, "document.getElementById('feedback').dispatchEvent(new MouseEvent('dblclick',{bubbles:true}))"); check(value("feedback") == 35, "double click resets default");
    const auto keyboardDepth = controller.undoDepth();
    js(page, "(()=>{const x=document.getElementById('feedback');x.focus();for(let i=0;i<5;++i)x.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',shiftKey:true}));x.dispatchEvent(new KeyboardEvent('keyup',{key:'ArrowUp'}));})()");
    check(std::abs(value("feedback") - 35.5) < 1.e-8 && controller.undoDepth() == keyboardDepth+1, "fine keyboard repeat creates one undo"); controller.undo(); events();
    js(page, "(()=>{const x=document.getElementById('mix');for(let i=0;i<3;++i)x.dispatchEvent(new WheelEvent('wheel',{deltaY:100,cancelable:true}));})()"); events(230);
    check(std::abs(value("mix") - 22) < 1.e-8, "wheel adjusts Mix"); controller.undo(); events(); check(value("mix") == 25, "wheel burst undoes once");
    auto drag = [&](auto* receiver) {
        const auto point = js(page, "(()=>{const r=document.getElementById('feedback').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").toList();
        const QPoint start(qRound(point[0].toDouble()), qRound(point[1].toDouble())); const double before = value("feedback"); const auto depth = controller.undoDepth();
        audio::AudioBuffer input(2,257), output(2,257); input.clear(); QTimer pump; bool rendered = true; double energy = 0;
        QObject::connect(&pump, &QTimer::timeout, [&] { rendered &= controller.processDeviceBlockForTest(input, output, 257); for (unsigned i = 0; i < 257; ++i) energy += output.getChannel(0)[i] * output.getChannel(0)[i]; });
        controller.seekSeconds(0); controller.play(); pump.start(5);
        QTest::mousePress(receiver, Qt::LeftButton, Qt::NoModifier, start); events();
        bool monotonic = true; double previous = before;
        for (int i = 1; i <= 12; ++i) { QTest::mouseMove(receiver, start + QPoint(0,-5*i),5); events(34); const double current = value("feedback"); monotonic &= current >= previous; previous = current; }
        QTest::mouseRelease(receiver, Qt::LeftButton, Qt::NoModifier, start + QPoint(0,-60)); events();
        pump.stop(); controller.stop(); check(rendered && energy > 1, "audio keeps rendering during native pointer input");
        check(monotonic && value("feedback") > before && controller.undoDepth() == depth+1, "native drag keeps anchor across snapshots and commits one undo");
        controller.undo(); events(); check(value("feedback") == before, "native drag undo restores original value");
        QTest::keyClick(receiver, Qt::Key_Up); events(); check(value("feedback") == before+1, "native keyboard input edits focused knob"); controller.undo(); events();
    };
    if (auto* native = view->findChild<QWebEngineView*>()) drag(native->focusProxy() ? native->focusProxy() : native);
    else if (page->isQuick() && page->quickItem()->window()) drag(page->quickItem()->window()); else check(false, "native browser input surface exists");
    QString automated; QObject::connect(&panel, &DelayPanel::automationRequested, [&](QString id) { automated = id; });
    js(page, "document.getElementById('feedback').dispatchEvent(new MouseEvent('contextmenu',{clientX:750,clientY:190,bubbles:true}));document.getElementById('automate').click()");
    check(automated == "feedback", "automation uses stable parameter ID");
    js(page, "document.getElementById('character').value='3';document.getElementById('character').dispatchEvent(new Event('change'))");
    check(value("character") == 3 && value("timeMs") == 100 && value("feedback") == 35 && value("mix") == 25, "character changes preserve timing, feedback and mix");
    const auto copy = controller.copyChannelStrip(track, false); const auto other = controller.addTrack(daw::TrackKind::Audio, "Copied");
    check(controller.pasteChannelInserts(other, copy), "Delay chain copies"); const auto copied = controller.project().findTrack(other)->inserts.front().id;
    check(controller.insertParameter(other, copied, "character") == 3 && controller.insertParameter(other, copied, "timeMs") == 100, "copied state retains character and time");
    check(!controller.insertSupportsSidechain(track, insert), "Delay has no unnecessary sidechain selector");
    controller.setInsertBypassed(track, insert, true);
    const auto path = temporary.path().toStdString() + "/delay.vlt"; check(bool(controller.saveProject(path)), "project saves Delay");
    { daw::EngineController reopened; reopened.initialize(48000,257,false); check(bool(reopened.openProject(path)) && reopened.insertParameter(track, insert, "character") == 3 && reopened.insertParameter(track, insert, "timeMs") == 100 && reopened.insertModel(track, insert)->bypassed, "project reopens all settings and bypass"); }
    controller.setInsertBypassed(track, insert, false);
    auto* timer = panel.findChild<QTimer*>("DelayTelemetryTimer"); panel.hide(); events(); check(timer && !timer->isActive(), "hidden panel stops telemetry"); panel.show(); events(); check(timer->isActive(), "shown panel resumes telemetry");
    for (const auto& p : daw::plugins::delay::parameterTable()) controller.setInsertParameter(track, insert, p.id, p.defaultValue);
    controller.setTempo(120); events();
    const auto screenshots = qEnvironmentVariable("VLT_DELAY_SCREENSHOTS"); if (!screenshots.isEmpty()) QDir().mkpath(screenshots);
    for (QSize size : {QSize(1100,460),QSize(960,420),QSize(1400,580)}) {
        panel.resize(size); events(200);
        check(js(page, "(()=>{const all=[...document.querySelectorAll('.dial,input,select,button:not(#reset):not(#automate)')].filter(e=>e.getClientRects().length);return all.every(e=>{const r=e.getBoundingClientRect();return r.left>=0&&r.top>=0&&r.right<=innerWidth+1&&r.bottom<=innerHeight+1})&&document.documentElement.scrollWidth<=innerWidth})()").toBool(), "controls fit minimum, default and enlarged panel");
        check(js(page, "[...document.querySelectorAll('.pair')].every(pair=>{const d=[...pair.querySelectorAll('.dial')].map(e=>e.getBoundingClientRect());return d[1].left-d[0].right>=25.5}) && !document.querySelector('.signature') && document.querySelector('h1').textContent==='Flowers Delay' && !document.querySelector('.brand span')").toBool(), "paired knob scales have breathing room and Flowers Delay replaces header branding");
        const auto shot = page->isQuick() && page->quickItem()->window() ? page->quickItem()->window()->grabWindow() : panel.grab().toImage();
        check(!shot.isNull(), "rendered screenshot exists"); if (!screenshots.isEmpty()) check(shot.save(screenshots+QString("/delay-%1x%2.png").arg(size.width()).arg(size.height())), "screenshot saves");
    }
    {
        audio::AudioBuffer impulse(2,4800); impulse.clear(); impulse.getChannel(0)[0] = impulse.getChannel(1)[0] = .8f;
        const auto source = temporary.path().toStdString()+"/impulse.wav"; writer.writeWAVFile(source,impulse,48000);
        daw::EngineController render; render.initialize(48000,128,false); const auto audio = render.importAudioToNewTrack(source,0); const auto fx = render.addInsert(audio,descriptor);
        render.setInsertParameter(audio,fx,"timeMode",2); render.setInsertParameter(audio,fx,"timeMs",200); render.setInsertParameter(audio,fx,"feedback",50); render.setInsertParameter(audio,fx,"mix",100);
        daw::rendering::Spec spec; spec.outputDir = temporary.path().toStdString(); spec.range = daw::rendering::Range::Custom; spec.customEndSeconds = .1;
        spec.tail = daw::rendering::Tail::Fixed; spec.tailSeconds = 1; spec.file.container = audio::platform::Container::Wav; spec.file.encoding = audio::platform::Encoding::Float32; spec.stemChannelIds = {audio};
        for (int mode = 0; mode < 3; ++mode) {
            render.setInsertParameter(audio,fx,"mix",mode == 1 ? 0 : 100); render.setInsertBypassed(audio,fx,mode == 2); spec.baseName = "delay-render-"+std::to_string(mode);
            daw::rendering::Report report; bool valid = bool(render.renderProject(spec,{},report)) && report.files.size() == 2;
            for (const auto& file : report.files) { audio::platform::DecodedAudio decoded; valid &= bool(audio::platform::decodeAudioFile(file,decoded)) && decoded.frames > 19200;
                if (decoded.frames <= 19200) continue;
                valid &= mode == 0 ? std::abs(decoded.interleaved[19200]-.8f)<1.e-5 && std::abs(decoded.interleaved[38400]-.4f)<1.e-5 : std::abs(decoded.interleaved[0]-.8f)<1.e-5 && std::abs(decoded.interleaved[19200])<1.e-6;
            }
            check(valid, mode == 0 ? "master/stem export preserves delayed impulses beyond source end" : mode == 1 ? "Mix 0 exports exact dry signal" : "host bypass exports exact dry signal");
        }
    }
    return failures ? 1 : 0;
}
