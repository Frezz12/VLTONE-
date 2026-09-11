#include "EngineController.hpp"
#include <chrono>
#include <thread>
#include <cstdio>
int main(){
 daw::EngineController c;audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;
 auto opened=c.initialize(config,true);
 if(!opened){std::printf("OPEN_FAILED %s\n",opened.message().c_str());return 1;}
 c.setMasterVolumeLive(0); // process the live input graph, output silence
 for(int i=0;i<64;++i){auto id=c.addTrack(daw::TrackKind::Audio,"Stress");if(i==0)c.setTrackMonitor(id,true);}
 std::printf("INPUT %s OUTPUT %s\n",c.currentInputDeviceUid().c_str(),c.currentOutputDeviceUid().c_str());
 for(unsigned frames:{32u,64u,128u,512u}){
  config=c.audioConfiguration();config.bufferSize=frames;
  auto applied=c.applyAudioConfiguration(config);if(!applied){std::printf("APPLY_FAILED %s\n",applied.message().c_str());return 2;}
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  daw::rt::TimingAccumulator discard;discard.drain(c.callbackMetrics());
  const auto deadlineBefore=c.callbackMetrics().counters().overruns;
  const auto& d=c.audioDeviceDiagnostics();auto count=d.diagCallbackCount();auto x=c.audioXruns();auto failures=c.failedAudioBlocks();
  std::this_thread::sleep_for(std::chrono::seconds(2));
  auto y=c.audioXruns();
  daw::rt::TimingAccumulator timing;timing.drain(c.callbackMetrics());const auto stats=timing.summary();
  std::printf("TIMING tracks=64 frames=%u mean_ms=%.4f p95_ms=%.4f p99_ms=%.4f max_ms=%.4f deadline_overruns=%llu\n",frames,stats.meanMs,stats.p95Ms,stats.p99Ms,stats.maximumMs,(unsigned long long)(c.callbackMetrics().counters().overruns-deadlineBefore));
  std::printf("HARDWARE frames=%u rate=%.0f callbacks=%llu running=%d recovery_needed=%d render_failures=%llu xruns=%llu,%llu,%llu,%llu input_peak=%.6f device_adc_time=%d\n",frames,c.sampleRate(),(unsigned long long)(d.diagCallbackCount()-count),c.isDeviceOpen(),c.audioDeviceNeedsRecovery(),(unsigned long long)(c.failedAudioBlocks()-failures),(unsigned long long)(y[0]-x[0]),(unsigned long long)(y[1]-x[1]),(unsigned long long)(y[2]-x[2]),(unsigned long long)(y[3]-x[3]),d.diagInputPeak(0),d.diagInputUsesDeviceTime());std::fflush(stdout);
 }
 c.shutdown();
}
