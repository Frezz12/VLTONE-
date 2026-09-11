#include <CoreAudio/CoreAudio.h>
#include <os/workgroup.h>
#include <cstdio>
#include <vector>
int main(){
 AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
 UInt32 size=0; if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,nullptr,&size))return 1;
 std::vector<AudioDeviceID> ds(size/sizeof(AudioDeviceID));AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,&size,ds.data());
 for(auto d:ds){
  CFStringRef name=nullptr;size=sizeof(name);a.mSelector=kAudioObjectPropertyName;AudioObjectGetPropertyData(d,&a,0,nullptr,&size,&name);
  char n[256]{};if(name){CFStringGetCString(name,n,sizeof(n),kCFStringEncodingUTF8);CFRelease(name);}
  UInt32 frames=0;size=sizeof(frames);a.mSelector=kAudioDevicePropertyBufferFrameSize;AudioObjectGetPropertyData(d,&a,0,nullptr,&size,&frames);
  Float64 rate=0;size=sizeof(rate);a.mSelector=kAudioDevicePropertyNominalSampleRate;AudioObjectGetPropertyData(d,&a,0,nullptr,&size,&rate);
  os_workgroup_t wg=nullptr;size=sizeof(wg);a.mSelector=kAudioDevicePropertyIOThreadOSWorkgroup;
  auto status=AudioObjectGetPropertyData(d,&a,0,nullptr,&size,&wg);
  unsigned recommended=wg?os_workgroup_max_parallel_threads(wg,nullptr):0;
  std::printf("device=%u name=%s rate=%.0f hardware_frames=%u workgroup_status=%d recommended_parallel=%u\n",d,n,rate,frames,int(status),recommended);
  if(wg)os_release(wg);
 }
}
