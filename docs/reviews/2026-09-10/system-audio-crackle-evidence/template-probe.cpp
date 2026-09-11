#include "EngineController.hpp"
#include "Job/AudioWorkerRegistration.hpp"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <vector>
#include <algorithm>
int main(int argc,char** argv){
 if(argc<3)return 2;
 unsigned frames=std::atoi(argv[2]),limit=argc>3?std::atoi(argv[3]):0;
 daw::EngineController c;if(!c.initialize(48000,frames,false))return 3;
 auto loaded=c.openProjectTemplate(argv[1]);if(!loaded){std::printf("LOAD_FAILED %s\n",loaded.message().c_str());return 4;}
#ifdef AFTER_CAP
 c.configureAudioWorkersForTest(true,limit);
#else
 c.configureAudioWorkersForTest(true);
#endif
 std::string monitored;unsigned plugins=0;
 for(const auto& t:c.project().tracks){plugins+=t.inserts.size();if(t.name=="LEAD 2")monitored=t.id;}
 std::vector<std::string> ids;for(const auto& t:c.project().tracks)ids.push_back(t.id);
 for(const auto& id:ids)c.setTrackMonitor(id,false);
 audio::AudioBuffer input(2,frames),output(2,frames);input.clear(frames);
 auto render=[&]{c.processDeviceBlockForTest(input,output,frames);};
 std::printf("CONFIG frames=%u tracks=%zu plugins=%u workers=%u realtime_helpers=%u requested_limit=%u\n",frames,ids.size(),plugins,c.audioWorkerCount(),c.realtimeAudioWorkerCount(),limit);std::fflush(stdout);
 for(int monitoring:{0,1}){
  c.setTrackMonitor(monitored,monitoring);c.setTrackInputRouting(monitored,0,1,true);
  for(unsigned i=0;i<frames;++i)for(unsigned ch=0;ch<2;++ch)input.getChannel(ch)[i]=monitoring?.01f*std::sin(float(i)*.3f):0;
  for(int i=0;i<500;++i)render();
  const auto period=std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(double(frames)/48000));
  daw::engine::AudioWorkerRegistration registration;
  std::vector<double> ms;ms.reserve(5000);unsigned over=0,nonfinite=0;float peak=0;auto startCpu=std::clock();
  registration.configure({true,48000,frames,{}});
  for(int i=0;i<5000;++i){
   auto start=std::chrono::steady_clock::now();render();auto end=std::chrono::steady_clock::now();
   auto elapsed=std::chrono::duration<double,std::milli>(end-start).count();ms.push_back(elapsed);over+=elapsed>frames/48.;
   for(unsigned ch=0;ch<2;++ch)for(unsigned f=0;f<frames;++f){float value=output.getChannel(ch)[f];if(!std::isfinite(value))++nonfinite;else peak=std::max(peak,std::abs(value));}
   std::this_thread::sleep_until(start+period);
  }
  registration.configure({});
  double cpu=1000.*double(std::clock()-startCpu)/CLOCKS_PER_SEC;std::sort(ms.begin(),ms.end());
  std::printf("RESULT monitoring=%d frames=%u p50=%.4f p95=%.4f p99=%.4f max=%.4f over=%u/5000 cpu_ms=%.1f nonfinite=%u peak=%.5f\n",monitoring,frames,ms[2500],ms[4750],ms[4950],ms.back(),over,cpu,nonfinite,peak);std::fflush(stdout);
 }
 c.configureAudioWorkersForTest(false);c.shutdown();
}
