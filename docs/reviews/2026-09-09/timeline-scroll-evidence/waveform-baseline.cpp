
#include "waveform-paint-snapshot.cpp"
#include <chrono>
#include <cstdio>
#include <QElapsedTimer>
int main() {
  constexpr int W=1600, H=960, N=12, F=240;
  std::vector<daw::WaveformPeaks> peaks(N);
  for(int k=0;k<N;++k){
    auto& a=peaks[k]; a.geometryId=k+1; a.durationSeconds=300; a.bucketsPerSecond=1000; a.channels=2;
    a.minima.resize(300000); a.maxima.resize(300000);
    for(int i=0;i<300000;i++){ float v=.25f+.20f*std::sin(i*.017f+k)+.15f*std::sin(i*.117f+k); a.minima[i]=-v; a.maxima[i]=v; }
    for(int div=4;div<=65536;div*=4){
      daw::WaveformPeaks::Level lv; lv.bucketsPerSecond=1000.0/div;
      for(int i=0;i<300000;i+=div){ float lo=0,hi=0; for(int j=i;j<std::min(i+div,300000);j++){lo=std::min(lo,a.minima[j]);hi=std::max(hi,a.maxima[j]);} lv.minima.push_back(lo);lv.maxima.push_back(hi); }
      a.levels.push_back(std::move(lv));
    }
  }
  for(int dpr: {1,2}) for(int mode: {0,1}) {
    QImage image(W*dpr,H*dpr,QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(dpr); image.fill(Qt::black);
    std::vector<double> ms; int hits=0;
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);p.setClipRect(0,0,W,H);
    for(int f=0;f<F;f++) {
      QElapsedTimer tm; tm.start();
      p.fillRect(0,0,W,H,Qt::black);
      for(int k=0;k<N;k++) {
        const double left = mode==0 ? -1000 : -1000-f*3;
        ui::PeakPaint how;how.clipLeft=0;how.clipRight=W;how.secondsPerPixel=.0125;how.color=Qt::white;
        ui::paintPeaks(p,&peaks[k],QRectF(left,k*80+5,24000,68),how);
      }
      ms.push_back(tm.nsecsElapsed()/1e6);
    }
    std::sort(ms.begin(),ms.end());
    printf("dpr=%d mode=%s median=%.3f p95=%.3f max=%.3f ms\n",dpr,mode==0?"stationary-cached":"horizontal-pan",ms[F/2],ms[int(F*.95)],ms.back());
  }
}
