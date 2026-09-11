#include "WaveformPaint.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
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
    int failures=0;
    const auto check=[&](bool ok,const char* name) {
        std::printf("%s  %s\n",ok?"PASS":"FAIL",name); if(!ok)++failures;
    };
    check(ui::checkWaveformBaselineForTest(),"cached/uncached pixels agree across gain, reversal and fractional pan");
    for(int dpr:{1,2}) {
        daw::WaveformPeaks flat; flat.geometryId=daw::allocateWaveformGeometryId();
        flat.durationSeconds=100;flat.bucketsPerSecond=40;
        flat.minima.assign(4000,-.8f);flat.maxima.assign(4000,.8f);
        ui::PeakPaint how;how.secondsPerPixel=.0125;how.clipLeft=0;how.clipRight=1000;how.color=Qt::white;
        QImage result(1000*dpr,100*dpr,QImage::Format_ARGB32_Premultiplied);
        result.setDevicePixelRatio(dpr); result.fill(Qt::transparent);
        {QPainter p(&result);ui::paintPeaks(p,&flat,QRectF(-73.25,0,3000,100),how);}
        bool seamless=true;
        for(int x=2;x<result.width()-2;++x) seamless &= qAlpha(result.pixel(x,30*dpr))==255;
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
    return failures?1:0;
}
