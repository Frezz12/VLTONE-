#include "EngineController.hpp"
#include "Device/AudioDeviceManager.hpp"
#include <portaudio.h>
#include <algorithm>
#include <cstdio>
#include <vector>

namespace fake {
struct Stream { bool active=false; PaStreamInfo info{1,0.01,0.02,48000}; PaStreamCallback* callback=nullptr;void* data=nullptr;int in=0,out=2; };
PaHostApiInfo host{1,paInDevelopment,"Review",3,0,0};
PaDeviceInfo devices[]={{2,"Original",0,2,2,.01,.02,.1,.1,48000},{2,"Candidate",0,2,2,.01,.02,.1,.1,48000},{2,"Fallback",0,2,2,.01,.02,.1,.1,44100}};
int starts=0,stops=0,opens=0;bool failRecovery=false;Stream* current=nullptr;
void pump(unsigned frames,bool inputPresent=true) {
 std::vector<float> inL(frames,.25f),inR(frames,.5f),outL(frames),outR(frames);
 float* in[]={inL.data(),inR.data()};float* out[]={outL.data(),outR.data()};PaStreamCallbackTimeInfo ti{1,1.01,1.03};
 current->callback(inputPresent?in:nullptr,out,frames,&ti,0,current->data);
}
}
extern "C" {
PaError Pa_Initialize(){return paNoError;} PaError Pa_Terminate(){return paNoError;}
PaDeviceIndex Pa_GetDeviceCount(){return 3;}
PaDeviceIndex Pa_GetDefaultInputDevice(){return 0;}
PaDeviceIndex Pa_GetDefaultOutputDevice(){return fake::failRecovery?2:0;}
const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex x){return x>=0&&x<3?&fake::devices[x]:nullptr;}
const PaHostApiInfo* Pa_GetHostApiInfo(PaHostApiIndex){return &fake::host;}
const char* Pa_GetErrorText(PaError){return "injected device failure";}
const PaHostErrorInfo* Pa_GetLastHostErrorInfo(){static PaHostErrorInfo e{};return &e;}
PaError Pa_IsFormatSupported(const PaStreamParameters*,const PaStreamParameters* out,double rate){return out&&out->device==2&&rate!=44100?paInvalidSampleRate:paNoError;}
PaError Pa_OpenStream(PaStream** p,const PaStreamParameters* in,const PaStreamParameters* out,double rate,unsigned long,PaStreamFlags,PaStreamCallback* cb,void* data){
 ++fake::opens;if(fake::failRecovery&&out->device!=2)return paDeviceUnavailable;
 auto* s=new fake::Stream;s->info.sampleRate=rate;s->callback=cb;s->data=data;s->in=in?in->channelCount:0;s->out=out->channelCount;*p=s;fake::current=s;return paNoError;
}
PaError Pa_CloseStream(PaStream* p){if(fake::current==p)fake::current=nullptr;delete static_cast<fake::Stream*>(p);return paNoError;}
PaError Pa_StartStream(PaStream* p){++fake::starts;static_cast<fake::Stream*>(p)->active=true;return paNoError;}
PaError Pa_StopStream(PaStream* p){++fake::stops;static_cast<fake::Stream*>(p)->active=false;return paNoError;}
PaError Pa_AbortStream(PaStream* p){static_cast<fake::Stream*>(p)->active=false;return paNoError;}
PaError Pa_IsStreamActive(PaStream* p){return static_cast<fake::Stream*>(p)->active;}
const PaStreamInfo* Pa_GetStreamInfo(PaStream* p){return &static_cast<fake::Stream*>(p)->info;}
}
int main(){
 {
  daw::EngineController c;audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;
  c.initialize(config,true);
  const int before=fake::starts;auto same=c.applyAudioConfiguration(config);
  std::printf("APPLY_SAME_CONFIG success=%d extra_starts=%d\n",bool(same),fake::starts-before);
  fake::failRecovery=true;config.outputDeviceUid="Review: Candidate";auto failed=c.applyAudioConfiguration(config);
  std::printf("FALLBACK success=%d device_rate=%.0f engine_rate=%.0f project_rate=%.0f input_enabled=%d running=%d\n",bool(failed),c.audioConfiguration().sampleRate,c.sampleRate(),c.project().sampleRate,c.audioConfiguration().inputEnabled,c.isDeviceOpen());
 }
 fake::failRecovery=false;
 {
  audio::AudioDeviceManager d;d.initialize();d.start();fake::current->active=false;
  std::printf("LOST_STREAM driver_active=%d manager_running=%d state=%d\n",fake::current->active,d.isRunning(),int(d.deviceState()));
 }
 {
  daw::EngineController c;audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;c.initialize(config,true);
  c.setRecordDirectory("/private/tmp/vlt-audio-review-2026-09-10/device-captures");
  auto track=c.addTrack(daw::TrackKind::Audio,"Capture");c.setTrackInputEnabled(track,true);c.startRecording(track);
  config.sampleRate=96000;auto switched=c.applyAudioConfiguration(config);
  for(int i=0;i<300;++i)fake::pump(32);
  auto r=c.finalizeRecordingCapture();auto& take=r.tracks.front();
  std::printf("RATE_DURING_RECORD success=%d engine_rate=%.0f wav_rate=%.0f frames=%llu device_seconds=.100 wav_seconds=%.3f\n",bool(switched),c.sampleRate(),take.sampleRate,(unsigned long long)take.frames,take.durationSeconds);
  c.startRecording(track);
  for(int i=0;i<300;++i) {
   if(i==100){config.inputEnabled=false;c.applyAudioConfiguration(config);}
   if(i==200){config.inputEnabled=true;c.applyAudioConfiguration(config);}
   fake::pump(32,fake::current->in>0);
  }
  auto missing=c.finalizeRecordingCapture();auto& shortTake=missing.tracks.front();
  std::printf("DISABLE_REENABLE_DEVICE_INPUT_DURING_RECORD expected_frames=9600 captured_frames=%llu dropped=%lld\n",(unsigned long long)shortTake.capturedFrames,(long long)shortTake.droppedFrames);
 }
}
