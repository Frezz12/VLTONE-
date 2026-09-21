#include "WaveformPaint.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static daw::WaveformPeaks wave(int seed) {
    daw::WaveformPeaks a;
    a.geometryId=daw::allocateWaveformGeometryId();
    a.durationSeconds=300; a.bucketsPerSecond=1000; a.channels=2;
    a.minima.resize(300000); a.maxima.resize(300000);
    for(int i=0;i<300000;i++) {
        float v=.25f+.20f*std::sin(i*.017f+seed)+.15f*std::sin(i*.117f+seed);
        a.minima[i]=-v; a.maxima[i]=v;
    }
    for(int div=4;div<=65536;div*=4) {
        daw::WaveformPeaks::Level lv; lv.bucketsPerSecond=1000.0/div;
        for(int i=0;i<300000;i+=div) {
            float lo=0,hi=0;
            for(int j=i;j<std::min(i+div,300000);j++) {lo=std::min(lo,a.minima[j]);hi=std::max(hi,a.maxima[j]);}
            lv.minima.push_back(lo);lv.maxima.push_back(hi);
        }
        a.levels.push_back(std::move(lv));
    }
    return a;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QTemporaryDir settings;
    app.setOrganizationName("WaveformPaintTest");
    app.setApplicationName("WaveformPaintTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    int failures=0;
    const auto check=[&](bool ok,const char* name) {
        std::printf("%s  %s\n",ok?"PASS":"FAIL",name); if(!ok)++failures;
    };
    check(ui::waveformStyle() == ui::WaveformStyle::Crisp, "fresh preferences default to Crisp");
    ui::setWaveformStyle(ui::WaveformStyle::Smooth);
    check(ui::PeakPaint{}.style == ui::WaveformStyle::Smooth &&
          QSettings().value(ui::kWaveformStyleSetting).toInt() == int(ui::WaveformStyle::Smooth),
          "style preference is persisted and picked up by new paint requests");
    ui::setWaveformStyle(ui::WaveformStyle::Crisp);
    check(ui::checkWaveformBaselineForTest(),"cached/uncached pixels agree across gain, reversal and fractional pan");
    {
        daw::WaveformPeaks transient;
        transient.durationSeconds = 2.0;
        transient.bucketsPerSecond = 1000.0;
        transient.minima.assign(2000, 0.0f);
        transient.maxima.assign(2000, 0.0f);
        transient.minima[30] = -1.0f;
        transient.maxima[30] = 1.0f;
        daw::WaveformPeaks::Level coarse;
        coarse.bucketsPerSecond = 250.0;
        coarse.minima.assign(500, 0.0f);
        coarse.maxima.assign(500, 0.0f);
        coarse.minima[7] = -1.0f;
        coarse.maxima[7] = 1.0f;
        transient.levels.push_back(std::move(coarse));
        ui::PeakPaint how;
        how.secondsPerPixel = 1.0 / (80.0 * .42);
        how.clipRight = 4;
        how.color = Qt::white;
        QImage image(4, 20, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &transient, QRectF(0, 0, 4, 20), how); }
        check(qAlpha(image.pixel(0, 2)) == 0 && qAlpha(image.pixel(1, 2)) == 255,
              "zoomed-out transients stay in their own pixel instead of bleeding from a coarse bucket");
        transient.levels.clear();
        std::fill(transient.minima.begin(), transient.minima.end(), -.3f);
        std::fill(transient.maxima.begin(), transient.maxima.end(), .3f);
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &transient, QRectF(0, 0, 4, 20), how); }
        check(qAlpha(image.pixel(0, 7)) > 0 && qAlpha(image.pixel(0, 7)) < 255 &&
              qAlpha(image.pixel(0, 8)) == 255,
              "waveform outline has subpixel coverage while its body stays opaque");
    }
    for (int dpr : {1, 2}) {
        daw::WaveformPeaks steps;
        steps.geometryId = daw::allocateWaveformGeometryId();
        steps.durationSeconds = 2; steps.bucketsPerSecond = 1;
        steps.minima = {-.2f, -.8f}; steps.maxima = {.2f, .8f};
        ui::PeakPaint how;
        how.secondsPerPixel = .125; how.clipRight = 16; how.color = Qt::white;
        QImage image(16 * dpr, 100 * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &steps, QRectF(0, 0, 16, 100), how); }
        check(qAlpha(image.pixel(2 * dpr, 25 * dpr)) == 0 &&
              qAlpha(image.pixel(6 * dpr, 25 * dpr)) == 0 &&
              qAlpha(image.pixel(10 * dpr, 25 * dpr)) == 255,
              "envelope holds each source bucket with a sharp step, without a smoothed ramp");
        const auto crisp = image;
        how.style = ui::WaveformStyle::Smooth;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &steps, QRectF(0, 0, 16, 100), how); }
        check(image != crisp && qAlpha(image.pixel(6 * dpr, 25 * dpr)) == 255,
              "Smooth interpolates the envelope instead of reusing the cached Crisp steps");
        how.style = ui::WaveformStyle::Crisp;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &steps, QRectF(0, 0, 16, 100), how); }
        check(image == crisp, "switching back restores the exact Crisp image");

        daw::engine::SampleBuffer samples(1, 2, 2.0);
        samples.writableChannel(0)[0] = .5f;
        samples.writableChannel(0)[1] = -.5f;
        how.samples = &samples; how.secondsPerPixel = 1.0 / 16;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, nullptr, QRectF(0, 0, 16, 100), how); }
        const auto inkNear = [&](int x, int y) {
            for (int dy = -1; dy <= 1; ++dy)
                if (qAlpha(image.pixel(x * dpr, y * dpr + dy))) return true;
            return false;
        };
        check(inkNear(2, 26) && inkNear(6, 26) && inkNear(10, 75) &&
              !inkNear(4, 37), "individual samples form a thin staircase without diagonal interpolation");
        how.style = ui::WaveformStyle::Smooth;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, nullptr, QRectF(0, 0, 16, 100), how); }
        check(inkNear(2, 38) && !inkNear(2, 26), "Smooth connects individual samples with an antialiased line");
        how.style = ui::WaveformStyle::Crisp;
        how.reversed = true;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, nullptr, QRectF(0, 0, 16, 100), how); }
        check(inkNear(2, 75) && inkNear(10, 26), "sample staircase follows source reversal");

        daw::engine::SampleBuffer detail(1, 32, 32.0);
        for (std::size_t i = 0; i < 32; ++i) detail.writableChannel(0)[i] = i < 16 ? .2f : -.6f;
        steps.geometryId = daw::allocateWaveformGeometryId();
        steps.durationSeconds = 1; steps.bucketsPerSecond = 2;
        steps.minima = {-.8f, -.8f}; steps.maxima = {.8f, .8f};
        how.reversed = false; how.samples = nullptr;
        how.secondsPerPixel = .125; how.clipRight = 8;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &steps, QRectF(0, 0, 8, 100), how); }
        const bool coarse = qAlpha(image.pixel(2 * dpr, 30 * dpr)) != 0;
        how.samples = &detail;
        image.fill(Qt::transparent);
        { QPainter p(&image); ui::paintPeaks(p, &steps, QRectF(0, 0, 8, 100), how); }
        check(coarse && qAlpha(image.pixel(2 * dpr, 30 * dpr)) == 0 && inkNear(2, 40),
              "close zoom resolves decoded source detail and replaces cached coarse peaks");
    }
    for (auto style : {ui::WaveformStyle::Crisp, ui::WaveformStyle::Smooth}) for(int dpr:{1,2}) {
        daw::WaveformPeaks flat; flat.geometryId=daw::allocateWaveformGeometryId();
        flat.durationSeconds=100;flat.bucketsPerSecond=40;
        flat.minima.assign(4000,-.8f);flat.maxima.assign(4000,.8f);
        ui::PeakPaint how;how.secondsPerPixel=.0125;how.clipLeft=0;how.clipRight=1000;how.color=Qt::white;
        how.style = style;
        QImage result(1000*dpr,100*dpr,QImage::Format_ARGB32_Premultiplied);
        result.setDevicePixelRatio(dpr); result.fill(Qt::transparent);
        {QPainter p(&result);ui::paintPeaks(p,&flat,QRectF(-73.25,0,3000,100),how);}
        bool seamless=true;
        for(int x=2;x<result.width()-2;++x) {
            if (qAlpha(result.pixel(x,30*dpr)) != 255 && seamless)
                std::fprintf(stderr, "waveform seam: dpr=%d x=%d alpha=%d\n", dpr, x, qAlpha(result.pixel(x,30*dpr)));
            seamless &= qAlpha(result.pixel(x,30*dpr))==255;
        }
        check(seamless,"fractional tile boundaries have no transparent seams on a solid waveform");

        std::vector<float> envelope(600,.2f);
        const auto id=daw::allocateWaveformGeometryId();
        const auto render=[&](std::uint64_t identity) {
            QImage image(1000*dpr,100*dpr,QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);image.fill(Qt::transparent);
            QPainter p(&image);ui::paintRecordingPeaks(p,envelope,.025,identity,QRectF(-320,0,2000,100),how);
            return image;
        };
        ui::resetWaveformPaintCacheForTest();
        const auto old = render(id); const auto before=ui::waveformPaintStatsForTest();
        envelope.back()=.9f; envelope.resize(650,.4f);
        const auto updated = render(id);
        check(updated!=old && updated==render(0),"append and partial-bucket growth never reuse an unfinished recording tail");
        check(ui::waveformPaintStatsForTest().tileHits>before.tileHits,"recording append reuses sealed raster tiles");
        std::fill(envelope.begin(),envelope.end(),.7f);
        check(render(daw::allocateWaveformGeometryId())==render(0),"compaction/source replacement invalidates cached extrema");
    }
    std::vector<daw::WaveformPeaks> peaks;
    for(int i=0;i<8;++i)peaks.push_back(wave(i));
    {
        daw::engine::SampleBuffer source(1, 48000 * 24, 48000.0);
        for (std::size_t i = 0; i < source.frames(); ++i)
            source.writableChannel(0)[i] = float((.1 + .75 * std::pow(std::sin(i * .00019), 8)) *
                                               std::sin(i * .063));
        daw::WaveformPeaks detailed; daw::buildPeaks(source, detailed);
        detailed.geometryId = 0;
        auto reference = detailed; reference.levels.clear();
        bool exact = true;
        for (double dpr : {1., 1.25, 2.}) for (double zoom : {.16, .32, .42, .55, .78, 1.0})
        for (bool reversed : {false, true}) {
            const auto render = [&](const daw::WaveformPeaks& input) {
                QImage image(qCeil(360 * dpr), qCeil(80 * dpr), QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
                QPainter painter(&image);
                ui::PeakPaint how; how.style = ui::WaveformStyle::Crisp;
                how.sourceStartSeconds = .0077; how.secondsPerPixel = 1.0 / (80.0 * zoom);
                how.clipRight = 360; how.reversed = reversed;
                ui::paintPeaks(painter, &input, QRectF(-13.25, 0, 400, 80), how);
                return image;
            };
            const bool matched = render(detailed) == render(reference);
            if (!matched) std::printf("detail mismatch at zoom %.2f DPR %.2f reverse %d\n", zoom, dpr, reversed);
            exact &= matched;
        }
        check(exact, "overview levels retain the exact detailed contour around 0.42x zoom");
    }
    for(int dpr:{1,2})for(int pan:{0,1}) {
        ui::resetWaveformPaintCacheForTest();
        QImage image(1600*dpr,960*dpr,QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        std::vector<double> times;
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setClipRect(0,0,1600,960);
        for(int f=0;f<60;++f) {
            QElapsedTimer elapsed;elapsed.start();p.fillRect(0,0,1600,960,Qt::black);
            for(int k=0;k<8;++k) {
                ui::PeakPaint how;how.clipLeft=0;how.clipRight=1600;how.secondsPerPixel=.0125;how.color=Qt::white;
                ui::paintPeaks(p,&peaks[k],QRectF(-1000-pan*f*3,k*80+5,24000,68),how);
            }
            times.push_back(elapsed.nsecsElapsed()/1e6);
        }
        std::sort(times.begin(),times.end());
        const auto stats=ui::waveformPaintStatsForTest();
        std::printf("8 waves 1600x960 dpr=%d pan=%d median=%.3f p95=%.3f max=%.3f ms hits=%llu builds=%llu bytes=%zu\n",
            dpr,pan,times[30],times[57],times.back(),(unsigned long long)stats.tileHits,(unsigned long long)stats.tileBuilds,stats.bytes);
        check(stats.tileHits>stats.tileBuilds*10 && stats.bytes<=64*1024*1024,"horizontal pan reuses raster tiles inside the memory budget");
        // Visit much more audio than fits in cache. Returning to evicted tiles
        // must remain correct and memory must remain bounded.
        ui::PeakPaint how;how.clipLeft=0;how.clipRight=1600;how.secondsPerPixel=.0125;
        for(int step=0;step<40;++step) for(int k=0;k<8;++k)
            ui::paintPeaks(p,&peaks[k],QRectF(-step*512,k*80+5,24000,68),how);
        check(ui::waveformPaintStatsForTest().bytes<=64*1024*1024,"long scroll evicts old raster tiles");
    }
    if (const QString path = qEnvironmentVariable("DAW_WAVEFORM_SCREENSHOT"); !path.isEmpty()) {
        daw::engine::SampleBuffer audio(1, 96000, 48000.0);
        for (std::size_t i = 0; i < audio.frames(); ++i) {
            const double t = i / 48000.0;
            const double envelope = std::pow(std::max(0.0, std::sin(t * 8.0)), 2.0) * std::exp(-t * .3);
            audio.writableChannel(0)[i] = float(envelope * (.62 * std::sin(t * 9300) + .28 * std::sin(t * 25100)));
        }
        daw::WaveformPeaks envelope; daw::buildPeaks(audio, envelope);
        QImage preview(1200, 620, QImage::Format_ARGB32_Premultiplied);
        preview.fill(QColor(44, 46, 49));
        QPainter p(&preview);
        p.setFont(QFont(QStringLiteral("Arial"), 10));
        for (int column = 0; column < 2; ++column) for (int row = 0; row < 4; ++row) {
            const QRectF area(column * 600 + 16, row * 150 + 25, 568, 112);
            p.fillRect(area, QColor(55, 89, 128));
            ui::PeakPaint how; how.clipLeft = area.left(); how.clipRight = area.right();
            how.style = column == 0 ? ui::WaveformStyle::Crisp : ui::WaveformStyle::Smooth;
            how.color = QColor(187, 212, 238); how.samples = &audio;
            how.secondsPerPixel = row < 2 ? 2.0 / area.width() : row == 2 ? .0002 : .0000025;
            how.sourceStartSeconds = row < 2 ? 0.0 : .16;
            how.gain = row == 1 ? .15f : 1.0f;
            ui::paintPeaks(p, &envelope, area, how);
            p.setPen(Qt::white);
            p.drawText(column * 600 + 16, row * 150 + 17,
                (column == 0 ? QStringLiteral("Crisp — ") : QStringLiteral("Smooth — ")) +
                QStringList{"Overview", "Quiet audio", "Close envelope", "Individual samples"}[row]);
        }
        p.end();
        check(preview.save(path), "saved waveform visual preview");
    }
    return failures?1:0;
}
