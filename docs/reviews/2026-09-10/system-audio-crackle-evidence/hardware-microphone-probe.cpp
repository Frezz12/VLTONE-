#include "EngineController.hpp"
#include <CoreAudio/CoreAudio.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <vector>
int main(int argc,char** argv){
 if(argc!=2)return 2;
 // Read-only discovery; the test opens the built-in microphone for metering/DSP only; it never records a file. Restore the
 // speaker's prior hardware buffer on every normal/error exit after shutdown.
 AudioObjectPropertyAddress p{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
 UInt32 bytes=0;AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&p,0,nullptr,&bytes);
 std::vector<AudioDeviceID> devices(bytes/sizeof(AudioDeviceID));AudioObjectGetPropertyData(kAudioObjectSystemObject,&p,0,nullptr,&bytes,devices.data());
 AudioDeviceID device=0,microphone=0;
 for(auto d:devices){CFStringRef uid=nullptr;bytes=sizeof(uid);p.mSelector=kAudioDevicePropertyDeviceUID;
  if(!AudioObjectGetPropertyData(d,&p,0,nullptr,&bytes,&uid)&&uid){if(CFStringCompare(uid,CFSTR("BuiltInSpeakerDevice"),0)==kCFCompareEqualTo)device=d;if(CFStringCompare(uid,CFSTR("BuiltInMicrophoneDevice"),0)==kCFCompareEqualTo)microphone=d;CFRelease(uid);}}
 if(!device||!microphone)return 3;
 p.mSelector=kAudioDevicePropertyBufferFrameSize;UInt32 original=0;bytes=sizeof(original);
 if(AudioObjectGetPropertyData(device,&p,0,nullptr,&bytes,&original))return 4;
 struct Restore{std::vector<std::pair<AudioDeviceID,UInt32>> states;~Restore(){for(auto [device,frames]:states){AudioObjectPropertyAddress p{kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
  auto status=AudioObjectSetPropertyData(device,&p,0,nullptr,sizeof(frames),&frames);std::printf("RESTORE device=%u hardware_frames=%u status=%d\n",device,frames,int(status));}}} restore;
 for(auto d:{device,microphone}){UInt32 frames=0;bytes=sizeof(frames);if(AudioObjectGetPropertyData(d,&p,0,nullptr,&bytes,&frames))return 4;restore.states.emplace_back(d,frames);}
 daw::EngineController c;audio::AudioDeviceConfig config;
 config.sampleRate=48000;config.bufferSize=512;config.inputEnabled=true;config.inputDeviceUid="coreaudio:BuiltInMicrophoneDevice";config.outputDeviceUid="coreaudio:BuiltInSpeakerDevice";
 auto started=c.initialize(config,true);if(!started){std::printf("OPEN_FAILED %s\n",started.message().c_str());return 5;}
 auto loaded=c.openProjectTemplate(argv[1]);if(!loaded){std::printf("LOAD_FAILED %s\n",loaded.message().c_str());return 6;}
 c.setMasterVolumeLive(0);
 std::string monitored;std::vector<std::string> ids;for(const auto& t:c.project().tracks){ids.push_back(t.id);if(t.name=="LEAD 2")monitored=t.id;}
 for(const auto& id:ids)c.setTrackMonitor(id,false);
 if(monitored.empty())return 9;
 c.setTrackInputRouting(monitored,0,1,true);
 int failed=0;
 for(unsigned frames:{32u,16u,128u})for(int monitoring:{0,1}){
  c.setTrackMonitor(monitored,monitoring);
  config=c.audioConfiguration();config.bufferSize=frames;
  if(!c.applyAudioConfiguration(config)){failed=7;break;}
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  daw::rt::TimingAccumulator discard;discard.drain(c.callbackMetrics());
  auto overruns=c.callbackMetrics().counters().overruns;auto x=c.audioXruns();const auto& d=c.audioDeviceDiagnostics();auto count=d.diagCallbackCount();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  daw::rt::TimingAccumulator timing;timing.drain(c.callbackMetrics());auto s=timing.summary();auto y=c.audioXruns();
  UInt32 actual=0;bytes=sizeof(actual);AudioObjectGetPropertyData(device,&p,0,nullptr,&bytes,&actual);
  bool okay=c.audioWorkerCount()==4&&c.realtimeAudioWorkerCount()==3&&c.workgroupAudioWorkerCount()==3;
  if(!okay)failed=8;
  std::printf("HARDWARE monitoring=%d frames=%u hardware_frames=%u workers=%u realtime_helpers=%u workgroup_helpers=%u callbacks=%llu p95_ms=%.4f p99_ms=%.4f max_ms=%.4f over=%llu xruns=%llu,%llu,%llu,%llu input_enabled=%d input_peak=%.6f device_adc_time=%d\n",monitoring,frames,actual,c.audioWorkerCount(),c.realtimeAudioWorkerCount(),c.workgroupAudioWorkerCount(),(unsigned long long)(d.diagCallbackCount()-count),s.p95Ms,s.p99Ms,s.maximumMs,(unsigned long long)(c.callbackMetrics().counters().overruns-overruns),(unsigned long long)(y[0]-x[0]),(unsigned long long)(y[1]-x[1]),(unsigned long long)(y[2]-x[2]),(unsigned long long)(y[3]-x[3]),c.audioConfiguration().inputEnabled,d.diagInputPeak(0),d.diagInputUsesDeviceTime());std::fflush(stdout);
 }
 c.shutdown();return failed;
}
