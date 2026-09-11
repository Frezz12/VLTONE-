#include "WaveformPaint.hpp"
#include "graphics/SceneRecorder.hpp"
#include <QApplication>
#include <QPainter>
#include <cmath>
#include <iostream>

using Meshes = std::vector<ui::graphics::SceneMesh>;
static bool same(const Meshes& a, const Meshes& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].vertices != b[i].vertices || a[i].transform != b[i].transform ||
            a[i].color != b[i].color || a[i].opacity != b[i].opacity ||
            bool(a[i].clip) != bool(b[i].clip)) return false;
        if (a[i].clip && (a[i].clip->bounds != b[i].clip->bounds ||
            a[i].clip->triangles != b[i].clip->triangles)) return false;
    }
    return true;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool pass, const char* name) {
        std::cout << (pass ? "PASS  " : "FAIL  ") << name << '\n';
        failures += !pass;
    };
    daw::WaveformPeaks peaks;
    peaks.geometryId = daw::allocateWaveformGeometryId();
    peaks.durationSeconds = 100; peaks.bucketsPerSecond = 100;
    for (int i = 0; i < 10000; ++i) {
        peaks.minima.push_back(-.2f - .1f * std::sin(i * .07));
        peaks.maxima.push_back(.4f + .3f * std::sin(i * .03));
    }
    for (int dpr : {1, 2}) for (bool reversed : {false, true}) {
        ui::resetWaveformPaintCacheForTest();
        ui::PeakPaint how;
        how.secondsPerPixel = .0125; how.clipLeft = 0; how.clipRight = 1000;
        how.color = Qt::white; how.reversed = reversed;
        auto record = [&](double left, std::uint64_t identity) {
            ui::graphics::SceneRecorder recorder(QSize(1000, 100), dpr);
            QPainter p(&recorder);
            p.setClipRect(QRectF(0, 5, 1000, 90));
            const auto saved = peaks.geometryId; peaks.geometryId = identity;
            ui::paintPeaks(p, &peaks, QRectF(left, 0, 5000, 100), how);
            peaks.geometryId = saved;
            p.end();
            return recorder.takeMeshes();
        };
        const auto first = record(-73.25, peaks.geometryId);
        const auto before = ui::waveformGeometryStatsForTest();
        const auto moved = record(-76.5, peaks.geometryId);
        const auto after = ui::waveformGeometryStatsForTest();
        check(after.tileHits > before.tileHits && after.tileBuilds == before.tileBuilds,
              "fractional pan reuses visible geometry tiles");
        bool shared = false;
        for (const auto& a : first) for (const auto& b : moved)
            shared |= a.vertices.constData() == b.vertices.constData() && a.transform != b.transform;
        check(shared, "pan changes GPU transforms while retaining vertex storage");
        check(same(moved, record(-76.5, 0)), "cached geometry matches a fresh build, including reversal and clipping");
        how.gain = 1.7f;
        check(same(record(-76.5, peaks.geometryId), record(-76.5, 0)), "gain changes invalidate geometry");
        for (int step = 0; step < 300; ++step) record(-step * 256., peaks.geometryId);
        check(ui::waveformGeometryStatsForTest().bytes <= 64 * 1024 * 1024, "geometry cache stays inside its memory budget");
    }
    std::vector<float> envelope(600, .2f);
    const auto identity = daw::allocateWaveformGeometryId();
    auto recording = [&](std::uint64_t id) {
        ui::graphics::SceneRecorder recorder(QSize(1000, 100), 2);
        QPainter p(&recorder); p.setClipRect(QRectF(0, 0, 1000, 100));
        ui::PeakPaint how; how.secondsPerPixel = .0125; how.clipLeft = 0; how.clipRight = 1000;
        ui::paintRecordingPeaks(p, envelope, .025, id, QRectF(-320, 0, 2000, 100), how);
        p.end(); return recorder.takeMeshes();
    };
    ui::resetWaveformPaintCacheForTest();
    const auto old = recording(identity);
    envelope.back() = .9f; envelope.resize(650, .4f);
    const auto updated = recording(identity);
    check(!same(old, updated) && same(updated, recording(0)), "recording refreshes unfinished tail without stale geometry");
    check(ui::waveformGeometryStatsForTest().tileHits > 0, "recording retains sealed geometry tiles");
    return failures ? 1 : 0;
}
