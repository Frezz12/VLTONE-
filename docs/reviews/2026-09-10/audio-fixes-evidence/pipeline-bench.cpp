#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
int main() {
 for (unsigned frames : {32u,512u}) {
  daw::EngineController c;c.initialize(48000,frames,false);
  std::string id;
  for(int i=0;i<64;++i){auto t=c.addTrack(daw::TrackKind::Audio,"Bench");if(i==0)id=t;}
  c.setTrackInputEnabled(id,true);c.setTrackMonitor(id,true);
  for(int run=0;run<7;++run){
   auto graphs=c.graphRebuildCountForTest();auto cpu=std::clock();auto begin=std::chrono::steady_clock::now();
   for(unsigned i=0;i<1000;++i){
#ifdef ATOMIC_ROUTING
    c.setTrackInputRouting(id,i%2,1+i%2,true);
#else
    c.setTrackInputChannel(id,i%2);c.setTrackInputChannelCount(id,1+i%2);c.setTrackInputEnabled(id,true);
#endif
   }
   std::printf("ROUTING frames=%u run=%d actions=1000 cpu_ms=%.3f wall_ms=%.3f rebuilds=%llu\n",frames,run,1000.*(std::clock()-cpu)/CLOCKS_PER_SEC,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count(),(unsigned long long)(c.graphRebuildCountForTest()-graphs));
  }
  audio::AudioBuffer input(4,frames),out(2,frames);std::fill_n(input.getChannel(0),frames,.25f);
  for(int i=0;i<100;++i)c.processDeviceBlockForTest(input,out,frames);
  for(int run=0;run<5;++run){auto cpu=std::clock();auto begin=std::chrono::steady_clock::now();
   for(int i=0;i<10000;++i)c.processDeviceBlockForTest(input,out,frames);
   std::printf("CALLBACK frames=%u run=%d blocks=10000 cpu_ms=%.3f wall_ms=%.3f\n",frames,run,1000.*(std::clock()-cpu)/CLOCKS_PER_SEC,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
  }
 }
}
