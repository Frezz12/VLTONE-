// SPDX-License-Identifier: MIT
#include <vlt/spectre/SpectrumWidget.hpp>
#include "../examples/WavFile.hpp"
#include "../examples/DemoEq.hpp"
#include <QApplication>
#include <QImage>
#include <QElapsedTimer>
#include <QThread>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <cmath>
#include <iostream>

using namespace vlt::spectre;
namespace {
int failures = 0, checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void sendKey(QWidget& widget, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void append16(QByteArray& data, quint16 value) {
    const auto little = qToLittleEndian(value); data.append(reinterpret_cast<const char*>(&little), 2);
}
void append32(QByteArray& data, quint32 value) {
    const auto little = qToLittleEndian(value); data.append(reinterpret_cast<const char*>(&little), 4);
}
void wavChecks() {
    QTemporaryDir temp;
    QByteArray bytes("RIFF"); append32(bytes, 36 + 8); bytes.append("WAVEfmt ");
    append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, 48000); append32(bytes, 48000 * 4); append16(bytes, 4); append16(bytes, 16);
    bytes.append("data"); append32(bytes, 8);
    append16(bytes, 16384); append16(bytes, quint16(-16384));
    append16(bytes, 0); append16(bytes, 32767);
    const QString path = temp.filePath(QStringLiteral("stereo.wav"));
    QFile file(path); file.open(QIODevice::WriteOnly); file.write(bytes); file.close();
    QString error;
    const auto audio = WavFile::load(path, error);
    check(audio && audio->left.size() == 2 && audio->sampleRate == 48000 &&
          audio->left[0] == .5f && audio->right[0] == -.5f, "WAV signed stereo decoding");
    file.open(QIODevice::WriteOnly); file.write(bytes.left(46)); file.close();
    check(!WavFile::load(path, error) && !error.isEmpty(), "truncated WAV is rejected");
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Analyzer analyzer;
    analyzer.prepare(48000);
    for (int i = 0; i < 8192; ++i) {
        const float sample = float(.5 * std::sin(6.283185307179586 * 1000 * i / 48000.0));
        analyzer.pushSample(sample, -sample);
    }
    analyzer.read();
    SpectrumWidget widget;
    widget.resize(900, 420);
    check(widget.setTrace(QStringLiteral("main"), analyzer.frame()), "accept measured spectrum");
    widget.show(); application.processEvents();
    const auto image = widget.grab().toImage();
    check(!image.isNull() && image.width() == 900, "widget renders real audio");

    // Shape test deliberately alternates large jumps. The path's bounding box must
    // remain inside the data range, even where an ordinary cubic spline overshoots.
    SpectrumFrame jagged;
    jagged.ready = true;
    jagged.frequenciesHz = {20, 100, 500, 2500, 12500, 20000};
    jagged.levelsDb = {-80, -10, -12, -82, -20, -70};
    SpectrumView plain; plain.tiltDbPerOctave = 0; plain.highFrequencyLiftDb = 0; plain.minimumDb = -100; plain.maximumDb = 0;
    const auto path = spectrumPath(jagged, QRectF(0, 0, 900, 400), plain);
    check(path.boundingRect().top() >= 40 - 0.001 && path.boundingRect().bottom() <= 328 + 0.001,
          "curve cannot invent a peak or dip outside measured values");
    bool continuous = true;
    for (int end = 3; end + 1 < path.elementCount(); end += 3) {
        const auto before = path.elementAt(end - 1), knot = path.elementAt(end), after = path.elementAt(end + 1);
        const double incoming = (knot.y - before.y) / (knot.x - before.x);
        const double outgoing = (after.y - knot.y) / (after.x - knot.x);
        continuous &= std::abs(incoming - outgoing) < 1e-7;
    }
    check(continuous, "every curve join has one continuous tangent");
    SpectrumView weighted;
    check(displayWeightingDb(500, weighted) == 0 && displayWeightingDb(1000, weighted) == 0 &&
          displayWeightingDb(2000, weighted) == 0, "treble lift leaves 500–2000 Hz unchanged");
    check(std::abs(displayWeightingDb(16000, weighted) - 12) < 1e-9 &&
          displayWeightingDb(8000, weighted) > displayWeightingDb(4000, weighted),
          "treble-only lift rises progressively and stops at the configured shelf");
    auto invalid = jagged; invalid.frequenciesHz[2] = 0;
    check(!widget.setTrace(QStringLiteral("bad"), invalid), "custom malformed frames are rejected");
    auto invalidView = plain; invalidView.minimumFrequency = 0;
    check(!widget.setView(invalidView), "invalid visible frequency range rejected");

    // Target changes must start from the visible curve, not jump to the new frame.
    auto quiet = analyzer.frame();
    for (auto& level : quiet.levelsDb) level -= 24;
    const auto before = widget.grab().toImage();
    widget.setTrace(QStringLiteral("main"), quiet);
    const auto immediate = widget.grab().toImage();
    check(before == immediate, "new spectrum targets do not jump on arrival");
    QElapsedTimer animation; animation.start();
    while (animation.elapsed() < 180) { application.processEvents(); QThread::msleep(4); }
    check(widget.grab().toImage() != immediate, "presentation advances smoothly on elapsed time");
    auto direct = widget.view(); direct.transitionMs = 0;
    widget.setView(direct);
    widget.setTrace(QStringLiteral("main"), analyzer.frame());
    check(widget.grab().toImage() == before, "host can disable transitions for reduced motion");

    const auto plot = widget.plotRect();
    const QPointF anchor(plot.left() + plot.width() * .33, plot.center().y());
    const double anchoredFrequency = widget.frequencyAt(anchor.x());
    QWheelEvent wheel(anchor, widget.mapToGlobal(anchor.toPoint()), QPoint(), QPoint(0,120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheel);
    check(std::abs(widget.frequencyAt(anchor.x()) - anchoredFrequency) < 1e-6,
          "wheel zoom keeps the cursor frequency anchored");
    check(widget.visibleMaximumFrequency() / widget.visibleMinimumFrequency() < 1000,
          "wheel changes the visible span");
    sendKey(widget, Qt::Key_Home);
    check(std::abs(widget.visibleMinimumFrequency() - 20) < .001 &&
          std::abs(widget.visibleMaximumFrequency() - 20000) < .001, "Home restores full range");
    bool frozenCallback = false;
    widget.frozenChanged = [&](bool value) { frozenCallback = value; };
    sendKey(widget, Qt::Key_Space);
    check(widget.frozen() && frozenCallback && !widget.setTrace(QStringLiteral("main"), analyzer.frame()),
          "Space freezes the display and notifies its host");
    sendKey(widget, Qt::Key_Space);
    sendKey(widget, Qt::Key_Right);
    check(widget.accessibleDescription().contains(QStringLiteral("dBFS")), "keyboard inspection exposes numeric levels");
    widget.setTheme(SpectrumTheme::light());
    widget.resize(220, 140); application.processEvents();
    check(widget.plotRect().width() > 100 && widget.plotRect().height() > 40 && !widget.grab().isNull(),
          "compact light view remains renderable");
    auto mini = widget.view(); mini.interactive = false; mini.legend = false;
    mini.frequencyLabels = mini.decibelLabels = false;
    check(widget.setView(mini), "host can strip the widget down to a noninteractive miniature");
    const double oldSpan = widget.visibleMaximumFrequency();
    sendKey(widget, Qt::Key_Plus);
    check(widget.visibleMaximumFrequency() == oldSpan, "disabled interactions do not change miniature view");
    // Real before/after analysis: the output layer must be driven by filtered audio,
    // not a drawing offset. Test both the boost and cut at the filter centre.
    for (const double gain : {-6.0, 0.0, 6.0}) {
        DemoEq eq; eq.setParameters(1500, gain, 1); eq.reset(48000);
        Analyzer pre, post; pre.prepare(48000); post.prepare(48000);
        for (int i = 0; i < 48000; ++i) {
            const float value = float(.1 * std::sin(6.283185307179586 * 1500 * i / 48000.0));
            const auto output = eq.process(value, -value);
            pre.pushSample(value, -value); post.pushSample(output[0], output[1]);
        }
        pre.read(); post.read();
        check(std::abs(post.frame().strongestLevelDb - pre.frame().strongestLevelDb - gain) < .03,
              "post-EQ layer measures actual filter gain, including cuts and bypass");
    }

    SpectrumWidget overlay;
    overlay.resize(600, 300);
    auto layerView = plain; layerView.transitionMs = 0; layerView.legend = false;
    layerView.frequencyLabels = layerView.decibelLabels = false;
    overlay.setView(layerView);
    SpectrumFrame base = jagged, above = jagged;
    for (auto& level : base.levelsDb) level = -60;
    for (auto& level : above.levelsDb) level = -30;
    TraceStyle back; back.filled = false; back.zOrder = 0;
    TraceStyle front; front.fillReference = QStringLiteral("before"); front.zOrder = 10;
    front.line = QColor("#80cbaa"); front.fill = QColor(128, 203, 170, 90);
    overlay.setTrace(QStringLiteral("before"), base, back);
    const auto baseImage = overlay.grab().toImage();
    overlay.setTrace(QStringLiteral("after"), above, front);
    const auto composite = overlay.grab().toImage();
    const auto area = overlay.plotRect();
    const int x = int(area.center().x());
    const auto yAt = [&](double db) { return int(area.bottom() - (db + 100) / 100 * area.height()); };
    check(baseImage.pixel(x, yAt(-45)) != composite.pixel(x, yAt(-45)) &&
          baseImage.pixel(x, yAt(-75)) == composite.pixel(x, yAt(-75)) &&
          baseImage.pixel(x, yAt(-15)) == composite.pixel(x, yAt(-15)),
          "transparent difference layer fills only the space between the two spectra");
    overlay.clearTraces();
    overlay.setTrace(QStringLiteral("after"), above, front);
    overlay.setTrace(QStringLiteral("before"), base, back);
    check(overlay.grab().toImage() == composite, "explicit layer order is independent of insertion order");
    auto below = base;
    for (auto& level : below.levelsDb) level = -80;
    overlay.setTrace(QStringLiteral("after"), below, front);
    const auto cutImage = overlay.grab().toImage();
    check(cutImage.pixel(x, yAt(-70)) != baseImage.pixel(x, yAt(-70)) &&
          cutImage.pixel(x, yAt(-45)) == baseImage.pixel(x, yAt(-45)),
          "difference layers also show frequency cuts below the original spectrum");
    wavChecks();
    std::cout << checks << " widget/WAV checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}