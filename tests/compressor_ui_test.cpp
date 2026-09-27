#include "CompressorPanel.hpp"
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
namespace comp = daw::plugins::compressor;
int failures = 0;
void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; }
void events(int ms = 65) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
QVariant js(ui::graphics::BrowserPage* page, const QString& script) {
    struct Result { bool done = false; QVariant value; };
    auto result = std::make_shared<Result>();
    page->runJavaScript(script, [result](const QVariant& v) { result->done = true; result->value = v; });
    for (int i = 0; i < 100 && !result->done; ++i) events(10);
    check(result->done, "JavaScript callback completes"); events(); return result->value;
}
}
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ui::registerFontUrlScheme(); QtWebEngineQuick::initialize(); QApplication app(argc, argv); ui::initializeApplicationFonts();
    QTemporaryDir temporary; app.setProperty("dawHeadlessDataRoot", temporary.path());
    app.setOrganizationName("VLTONE-Compressor-Test"); app.setApplicationName("CompressorWeb");
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    daw::EngineController controller; check(bool(controller.initialize(48000, 257, false)), "headless engine initializes");
    const auto descriptor = comp::CompressorInstance::staticDescriptor();
    audio::AudioBuffer liveSource(2, 192000);
    for (unsigned i = 0; i < 192000; ++i)
        liveSource.getChannel(0)[i] = liveSource.getChannel(1)[i] = float(.6 * std::sin(2 * std::numbers::pi * 431 * i / 48000));
    const auto livePath = temporary.path().toStdString() + "/gesture-source.wav";
    audio::AudioRecorder liveWriter; liveWriter.initialize(48000, 2); liveWriter.writeWAVFile(livePath, liveSource, 48000);
    const auto track = controller.importAudioToNewTrack(livePath, 0); const auto insert = controller.addInsert(track, descriptor);
    check(!insert.empty(), "compressor inserts through the normal plugin host"); if (insert.empty()) return 1;
    const auto value = [&](const char* id) { return controller.insertParameter(track, insert, id); };
    CompressorPanel panel(&controller, QString::fromStdString(track), QString::fromStdString(insert)); panel.resize(920, 360);
    std::unique_ptr<ui::graphics::WorkspaceSurface> workspace;
    if (qEnvironmentVariableIntValue("VLT_COMPRESSOR_TEST_WORKSPACE") == 1) {
        workspace = std::make_unique<ui::graphics::WorkspaceSurface>(&panel);
        QObject::connect(workspace.get(), &ui::graphics::WorkspaceSurface::failed, [](const QString& reason) {
            std::fprintf(stderr, "GPU failure: %s\n", reason.toUtf8().constData()); check(false, "GPU workspace stays available");
        });
    }
    panel.show(); auto* view = panel.findChild<ui::graphics::BrowserSurface*>("CompressorWebView");
    check(view != nullptr, "panel uses BrowserSurface"); if (!view) return 1; auto* page = view->page();
    bool connected = false;
    for (int i = 0; i < 50 && !connected; ++i) connected = js(page, "document.documentElement.dataset.connected === 'true'").toBool();
    check(connected, "local resources and QWebChannel connect"); if (!connected) return 1;
    std::printf("Backend %s, scale %.2f\n", page->isQuick() ? "GPU Quick" : "Widgets", panel.devicePixelRatioF());
    check(js(page, "document.querySelectorAll('.dial').length===7 && document.querySelectorAll('[role=slider]').length===9 && document.getElementById('ratio-value').value==='3.0'").toBool(), "seven dials, two graph handles and default values are accessible");
    check(!page->navigationPolicy(QUrl("https://example.com"), true), "panel rejects external navigation");
    const auto background = js(page, "getComputedStyle(document.getElementById('panel')).backgroundImage");
    ThemeManager::instance().setThemeId("light", false); events();
    ThemeManager::instance().setThemeId("gruvbox", false); events();
    check(js(page, "getComputedStyle(document.getElementById('panel')).backgroundImage") == background, "light compressor finish is independent of the DAW theme");

    js(page, "(()=>{const x=document.getElementById('threshold-value');x.focus();x.value='-27.4';})()"); events(180);
    check(js(page, "document.getElementById('threshold-value').value==='-27.4'").toBool(), "telemetry leaves active numeric input intact");
    js(page, "document.getElementById('threshold-value').dispatchEvent(new Event('change'));document.getElementById('threshold-value').blur()");
    check(value("threshold") == -27.4, "numeric input reaches the DSP in plain units");
    controller.undo(); events(); check(value("threshold") == -18, "numeric input undoes once"); controller.redo(); events(); check(value("threshold") == -27.4, "numeric edit redoes");
    js(page, "document.getElementById('threshold').dispatchEvent(new MouseEvent('dblclick',{bubbles:true}))"); check(value("threshold") == -18, "double click resets to default");
    const auto keyboardDepth = controller.undoDepth();
    js(page, "(()=>{const x=document.getElementById('ratio');x.focus();for(let i=0;i<5;++i)x.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',shiftKey:true}));x.dispatchEvent(new KeyboardEvent('keyup',{key:'ArrowUp'}));})()");
    check(std::abs(value("ratio") - 3.05) < 1e-8 && controller.undoDepth() == keyboardDepth + 1, "Shift and key repeat form one precise undo gesture");
    controller.undo(); events();
    js(page, "(()=>{const x=document.getElementById('mix');for(let i=0;i<3;++i)x.dispatchEvent(new WheelEvent('wheel',{deltaY:100,cancelable:true}));})()"); events(250);
    check(std::abs(value("mix") - 97) < 1e-8, "hover wheel adjusts Mix"); controller.undo(); events(); check(value("mix") == 100, "wheel burst undoes once");

    auto nativeDrag = [&](auto* receiver, const QString& id, QPoint delta, const char* param, bool live) {
        const auto point = js(page, QString("(()=>{const r=document.getElementById('%1').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()").arg(id)).toList();
        const QPoint start(qRound(point[0].toDouble()), qRound(point[1].toDouble()));
        const double before = value(param); const auto depth = controller.undoDepth();
        audio::AudioBuffer input(2, 257), output(2, 257); input.clear();
        QTimer audioPump; bool rendered = true; double energy = 0;
        QObject::connect(&audioPump, &QTimer::timeout, [&] {
            rendered &= controller.processDeviceBlockForTest(input, output, 257);
            for (unsigned i = 0; i < 257; ++i) energy += output.getChannel(0)[i] * output.getChannel(0)[i];
        });
        if (live) { controller.seekSeconds(0); controller.play(); audioPump.start(5); }
        QTest::mousePress(receiver, Qt::LeftButton, Qt::NoModifier, start); events();
        bool monotonic = true; double previous = before;
        for (int i = 1; i <= 16; ++i) {
            QTest::mouseMove(receiver, start + delta * i / 16, 5); events(34);
            const double current = value(param); monotonic &= current + 1e-8 >= previous; previous = current;
        }
        QTest::mouseRelease(receiver, Qt::LeftButton, Qt::NoModifier, start + delta); events();
        check(monotonic && value(param) > before && controller.undoDepth() == depth + 1,
              "real pointer gesture remains anchored during telemetry and creates one undo");
        if (live) { audioPump.stop(); controller.stop(); check(rendered && energy > 1, "real audio blocks keep rendering throughout the pointer gesture"); }
        controller.undo(); events(); check(std::abs(value(param) - before) < 1e-8, "pointer gesture restores its original value with one undo");
    };
    auto drags = [&](auto* receiver) {
        nativeDrag(receiver, "threshold", {0, -65}, "threshold", true);
        nativeDrag(receiver, "graph-threshold", {35, 0}, "threshold", true);
        nativeDrag(receiver, "graph-ratio", {0, -25}, "ratio", false);
    };
    if (auto* native = view->findChild<QWebEngineView*>()) drags(native->focusProxy() ? native->focusProxy() : native);
    else if (page->isQuick() && page->quickItem()->window()) drags(page->quickItem()->window());
    else check(false, "browser has a native input surface");
    QString automated;
    QObject::connect(&panel, &CompressorPanel::automationRequested, [&](QString id) { automated = id; });
    js(page, "document.getElementById('ratio').dispatchEvent(new MouseEvent('contextmenu',{clientX:350,clientY:180,bubbles:true}));document.getElementById('automate').click()");
    check(automated == "ratio", "context menu requests automation using the stable ID");
    js(page, "document.getElementById('mode').click();document.getElementById('autoGain').click()");
    check(value("mode") == 1 && value("autoGain") == 1 && value("ratio") == 3, "mode and Auto Gain are editable without moving other knobs");

    controller.setInsertParameter(track, insert, "threshold", -33); controller.setInsertParameter(track, insert, "mix", 47);
    const auto copy = controller.copyChannelStrip(track, false); const auto other = controller.addTrack(daw::TrackKind::Audio, "Copied");
    check(controller.pasteChannelInserts(other, copy), "effect chain copies with opaque compressor state");
    const auto copiedId = controller.project().findTrack(other)->inserts.front().id;
    check(controller.insertParameter(other, copiedId, "threshold") == -33 && controller.insertParameter(other, copiedId, "mix") == 47, "copied compressor retains parameter values");
    check(controller.insertSupportsSidechain(track, insert), "standard plugin window offers a sidechain source");
    check(bool(controller.setInsertSidechainSource(track, insert, other)), "standard sidechain routing accepts another track");
    controller.setInsertBypassed(track, insert, true);
    const auto path = temporary.path().toStdString() + "/compressor.vlt";
    check(bool(controller.saveProject(path)), "project saves compressor and routing");
    {
        daw::EngineController reopened; reopened.initialize(48000, 257, false);
        check(bool(reopened.openProject(path)) && reopened.insertParameter(track, insert, "threshold") == -33 && reopened.insertParameter(track, insert, "mix") == 47 &&
            reopened.insertModel(track, insert)->bypassed && reopened.insertModel(track, insert)->sidechainTrackIds == std::vector<std::string>{other},
            "project reopens parameters, bypass and sidechain source");
    }
    controller.setInsertBypassed(track, insert, false); controller.setInsertSidechainSource(track, insert, "");
    for (const auto& p : comp::parameterTable()) controller.setInsertParameter(track, insert, p.id, p.defaultValue);
    controller.pumpPluginEvents();
    auto* processor = dynamic_cast<comp::CompressorInstance*>(controller.insertInstance(track, insert));
    if (processor) {
        std::array<float, 257> input{}, left{}, right{}; input.fill(.8f); const float* inputs[]{input.data(), input.data()}; float* outputs[]{left.data(), right.data()};
        daw::plugins::PluginProcessContext context; context.inputs = inputs; context.outputs = outputs; context.inputChannels = context.outputChannels = 2; context.frames = 257;
        for (int i = 0; i < 100; ++i) processor->process(context);
        events(40);
        check(js(page, "Number(document.querySelector('#meter-gr [role=meter]').getAttribute('aria-valuenow')) > 9 && Number(document.querySelector('#meter-in [role=meter]').getAttribute('aria-valuenow')) > -4").toBool(), "real DSP telemetry drives GR and input meters");
    }
    auto* timer = panel.findChild<QTimer*>("CompressorTelemetryTimer"); panel.hide(); events(); check(timer && !timer->isActive(), "hidden panel stops telemetry"); panel.show(); events();

    const auto screenshots = qEnvironmentVariable("VLT_COMPRESSOR_SCREENSHOTS"); if (!screenshots.isEmpty()) QDir().mkpath(screenshots);
    for (QSize size : {QSize(920, 360), QSize(800, 340), QSize(1200, 470)}) {
        panel.resize(size); events(220);
        check(js(page, "(()=>{const all=[...document.querySelectorAll('.dial,input,button:not(#reset):not(#automate),.meter')];return all.every(e=>{const r=e.getBoundingClientRect();return r.left>=0 && r.top>=0 && r.right<=innerWidth+1 && r.bottom<=innerHeight+1}) && document.documentElement.scrollWidth<=innerWidth})()").toBool(), "controls fit default, minimum and enlarged panel without clipping");
        const auto shot = page->isQuick() && page->quickItem()->window() ? page->quickItem()->window()->grabWindow() : panel.grab().toImage();
        check(!shot.isNull() && shot.pixelColor(shot.width()/2, 5).lightness() > 160, "rendered panel retains its light housing");
        if (!screenshots.isEmpty()) check(shot.save(screenshots + QString("/compressor-%1x%2.png").arg(size.width()).arg(size.height())), "panel screenshot saves");
    }
    // Exercise the actual graph and file exporter, including PDC and bypass.
    {
        constexpr unsigned frames = 12000; audio::AudioBuffer source(2, frames);
        for (unsigned i = 0; i < frames; ++i) source.getChannel(0)[i] = source.getChannel(1)[i] = float(.5 * std::sin(2 * std::numbers::pi * 431 * i / 48000));
        const auto sourcePath = temporary.path().toStdString() + "/source.wav";
        audio::AudioRecorder writer; writer.initialize(48000, 2); writer.writeWAVFile(sourcePath, source, 48000);
        daw::EngineController render; render.initialize(48000, 128, false);
        const auto audio = render.importAudioToNewTrack(sourcePath, 0); const auto fx = render.addInsert(audio, descriptor);
        daw::rendering::Spec spec; spec.outputDir = temporary.path().toStdString(); spec.range = daw::rendering::Range::Custom;
        spec.customEndSeconds = double(frames)/48000; spec.file.container = audio::platform::Container::Wav; spec.file.encoding = audio::platform::Encoding::Float32;
        spec.stemChannelIds = {audio};
        for (int mode = 0; mode < 3; ++mode) {
            spec.baseName = "compressor-render-" + std::to_string(mode);
            render.setInsertBypassed(audio, fx, mode == 1); render.setInsertParameter(audio, fx, "mix", mode == 0 ? 0 : 100);
            daw::rendering::Report report; bool valid = bool(render.renderProject(spec, {}, report)) && report.files.size() == 2;
            for (const auto& file : report.files) {
                audio::platform::DecodedAudio decoded; valid &= bool(audio::platform::decodeAudioFile(file, decoded)) && decoded.frames == frames;
                if (decoded.frames != frames) continue;
                double error = 0, energy = 0; for (unsigned i = 0; i < frames; ++i) {
                    error = std::max(error, double(std::abs(decoded.interleaved[2*i] - source.getChannel(0)[i]))); energy += decoded.interleaved[2*i] * decoded.interleaved[2*i];
                }
                valid &= mode < 2 ? error < 2e-5 : energy < frames * .06 && energy > 1;
            }
            check(valid, mode == 0 ? "offline master/stem Mix 0 exports compensate 16 samples exactly" : mode == 1 ? "host bypass preserves aligned audio during offline export" : "offline master/stem apply compression with no external sidechain");
        }
    }
    return failures ? 1 : 0;
}
