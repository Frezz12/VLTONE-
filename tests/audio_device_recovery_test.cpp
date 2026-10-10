// Fault-injection at the PortAudio boundary: no real device is opened.
#include "EngineController.hpp"
#include "Device/AudioDeviceManager.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <portaudio.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <limits>
#include <thread>
namespace fake {
struct Stream { bool active=false; int device=0; PaStreamInfo info{1,0.01,0.02,48000}; PaStreamCallback* callback=nullptr; PaStreamFinishedCallback* finished=nullptr; void* data=nullptr; int in=0,out=2; };
PaHostApiInfo host{1,paInDevelopment,"Test",3,0,0};
PaDeviceInfo devices[]={{2,"Original",0,8,2,.01,.02,.1,.1,48000},{2,"Candidate",0,8,2,.01,.02,.1,.1,48000},{2,"Fallback",0,8,2,.01,.02,.1,.1,44100}};
int starts=0, opens=0; bool failRecovery=false, failStart=false, failAll=false; Stream* current=nullptr;
void pump(unsigned frames, bool inputPresent=true, bool timed=false, unsigned flags=0, bool noTime=false) {
 std::vector<std::vector<float>> storage(10,std::vector<float>(frames));
 float* in[8]; float* out[2];
 for(int i=0;i<8;++i){std::fill(storage[i].begin(),storage[i].end(),.1f*(i+1));in[i]=storage[i].data();}
 out[0]=storage[8].data();out[1]=storage[9].data();
 PaStreamCallbackTimeInfo ti{1, timed?1.01:1, timed?1.03:1};
 current->callback(inputPresent?in:nullptr,out,frames,noTime?nullptr:&ti,flags,current->data);
}
}
extern "C" {
PaError Pa_Initialize(){return paNoError;} PaError Pa_Terminate(){return paNoError;}
PaDeviceIndex Pa_GetDeviceCount(){return 3;}
PaDeviceIndex Pa_GetDefaultInputDevice(){return fake::failRecovery?2:0;}
PaDeviceIndex Pa_GetDefaultOutputDevice(){return fake::failRecovery?2:0;}
const PaDeviceInfo* Pa_GetDeviceInfo(PaDeviceIndex x){return x>=0&&x<3?&fake::devices[x]:nullptr;}
const PaHostApiInfo* Pa_GetHostApiInfo(PaHostApiIndex){return &fake::host;}
const char* Pa_GetErrorText(PaError){return "injected device failure";}
const PaHostErrorInfo* Pa_GetLastHostErrorInfo(){static PaHostErrorInfo e{};return &e;}
PaError Pa_IsFormatSupported(const PaStreamParameters*,const PaStreamParameters* out,double rate){return out&&out->device==2&&rate!=44100?paInvalidSampleRate:paNoError;}
PaError Pa_OpenStream(PaStream** p,const PaStreamParameters* in,const PaStreamParameters* out,double rate,unsigned long,PaStreamFlags,PaStreamCallback* cb,void* data){
 ++fake::opens;if(fake::failAll || (fake::failRecovery&&out->device!=2))return paDeviceUnavailable;
 auto* s=new fake::Stream;s->device=out->device;s->info.sampleRate=rate;s->callback=cb;s->data=data;s->in=in?in->channelCount:0;s->out=out->channelCount;*p=s;fake::current=s;return paNoError;
}
PaError Pa_CloseStream(PaStream* p){if(fake::current==p)fake::current=nullptr;delete static_cast<fake::Stream*>(p);return paNoError;}
PaError Pa_SetStreamFinishedCallback(PaStream* p,PaStreamFinishedCallback* cb){static_cast<fake::Stream*>(p)->finished=cb;return paNoError;}
PaError Pa_StartStream(PaStream* p){++fake::starts;auto* s=static_cast<fake::Stream*>(p);if(fake::failStart&&s->device==1)return paDeviceUnavailable;s->active=true;return paNoError;}
PaError Pa_StopStream(PaStream* p){static_cast<fake::Stream*>(p)->active=false;return paNoError;}
PaError Pa_AbortStream(PaStream* p){static_cast<fake::Stream*>(p)->active=false;return paNoError;}
PaError Pa_IsStreamActive(PaStream* p){return static_cast<fake::Stream*>(p)->active;}
const PaStreamInfo* Pa_GetStreamInfo(PaStream* p){return &static_cast<fake::Stream*>(p)->info;}
}
int failures=0;
void check(bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)++failures;}
int main(){
 const auto dir=std::filesystem::temp_directory_path()/("vlt-device-regression-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 {
  daw::EngineController c{};audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;
  check(bool(c.initialize(config,true)),"initialize with one prepared stream");
  const int before=fake::starts;check(bool(c.applyAudioConfiguration(config))&&fake::starts==before,"unchanged Apply starts no stream");
  config.bufferSize=64;check(bool(c.applyAudioConfiguration(config))&&fake::starts==before+1,"changed Apply starts exactly once");
  fake::failStart=true;config.outputDeviceUid="Test: Candidate";
  check(!c.applyAudioConfiguration(config)&&c.isDeviceOpen()&&fake::current->device==0,"start failure restores working previous stream");
  fake::failStart=false;fake::failRecovery=true;
  check(!c.applyAudioConfiguration(config)&&c.sampleRate()==44100&&c.audioConfiguration().sampleRate==44100&&c.project().sampleRate==44100&&c.isDeviceOpen(),"fallback synchronizes engine, project and device rate even on error");
 }
 fake::failRecovery=false;
 {
  daw::EngineController c{}; audio::AudioDeviceConfig config;
  config.sampleRate=48000;config.bufferSize=32;config.inputEnabled=false;
  check(bool(c.initialize(config,true)),"output-only recording fixture initializes");
  const auto captureDir=dir/"input-preflight";
  c.setRecordDirectory(captureDir.string());
  const auto track=c.addTrack(daw::TrackKind::Audio,"Mic");
  const auto midi=c.addTrack(daw::TrackKind::Midi,"Keys");
  c.seekSeconds(2);
  const auto position=c.positionSeconds();
  const auto undo=c.undoDepth();
  check(!c.canStartRecordingTracksExactly({track})&&c.canStartRecordingTracksExactly({midi}),
        "cloud preflight checks audio input before reserving tracks, without blocking MIDI");
  check(!c.startRecording(track)&&!c.isRecording(),"disabled device input rejects recording before capture");
  check(!c.armCountIn({track},4)&&!c.isCountingIn(),"disabled input rejects count-in before clicks or monitoring");
  check(!c.startRecordingTracksExactly({midi,track})&&!c.isRecording(),"mixed exact start cannot leave a partial MIDI recording");
  check(!c.armCountInExactly({track},4)&&!c.isCountingIn(),"exact count-in validates hardware input too");
  const auto* unchanged=c.project().findTrack(track);
  check(unchanged&&!unchanged->armed&&!unchanged->monitor&&unchanged->clips.empty()&&
        c.undoDepth()==undo&&c.positionSeconds()==position&&!std::filesystem::exists(captureDir),
        "rejected input leaves transport, track, Undo and recording directory untouched");
  const auto notices=c.takeAudioFailureNotices();
  check(notices.size()==1&&notices.front().operation=="Recording input"&&
        notices.front().detail.find("No active audio input")!=std::string::npos,
        "unavailable input produces one actionable pre-recording notice");
  check(c.startRecording(midi),"MIDI recording remains available with output-only audio");
  c.stopRecording();
  c.setTrackInputEnabled(track,false);
  check(c.startRecording(track),"explicit track No Input remains a valid silent capture");
  for(int i=0;i<100;++i)fake::pump(32,false);
  c.stopRecording();
  check(c.recordingWarning().empty(),"intentional silence does not report missing audio");
  c.setTrackInputEnabled(track,true);
  config.inputEnabled=true;
  check(bool(c.applyAudioConfiguration(config)),"enable the device input after rejected capture");
  c.setTrackInputRouting(track,7,2,true);
  check(!c.startRecording(track)&&!c.isRecording(),"stereo route beyond opened channels is rejected");
  c.setTrackInputRouting(track,8,1,true);
  check(!c.startRecording(track),"unavailable mono input is rejected");
  c.setTrackInputRouting(track,7,1,true);
  check(c.startRecording(track),"valid last input starts after correcting the settings");
  for(int i=0;i<100;++i)fake::pump(32);
  const auto path=c.stopRecording();
  audio::platform::DecodedAudio recorded;
  check(!path.empty()&&c.recordingWarning().empty()&&
        bool(audio::platform::decodeAudioFile(path,recorded))&&recorded.frames==3200&&
        std::all_of(recorded.interleaved.begin(),recorded.interleaved.end(),
            [](float sample){return std::abs(sample-.8f)<1e-6f;}),
        "ordinary Stop after valid input preserves every sample without interruption warning");
 }
 {
  daw::EngineController c{}; audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;
  check(bool(c.initialize(config,true)),"recovery budget fixture initializes without a journal");
  fake::current->active=false;fake::current->finished(fake::current->data);fake::failAll=true;
  std::uint64_t incident=0;
  for (int attempt=0;attempt<3;++attempt) {
   if(attempt)std::this_thread::sleep_for(std::chrono::milliseconds(3050));
   check(!c.recoverAudioDevice(),"automatic reconnection failure is returned");
   auto notices=c.takeAudioFailureNotices();
   check(notices.size()==1 && (!incident || incident==notices[0].incident),"device attempts update one incident");
   if(!notices.empty())incident=notices[0].incident;
   const int opens=fake::opens;(void)c.recoverAudioDevice();
   check(fake::opens==opens,"reconnection respects retry interval and exhaustion");
  }
  check(c.audioDeviceRecoveryExhausted(),"three failures require manual recovery");
  check(!c.recoverAudioDevice(true)&&c.audioDeviceRecoveryExhausted(),"failed manual attempt does not reopen the automatic retry budget");
  fake::failAll=false;
  check(bool(c.recoverAudioDevice(true))&&c.isDeviceOpen()&&!c.audioDeviceRecoveryExhausted(),"successful manual recovery begins a new cycle");
 }
 {
  daw::EngineController c{}; audio::AudioDeviceConfig config;config.sampleRate=48000;config.bufferSize=32;c.initialize(config,true);
  fake::current->active=false;fake::current->finished(fake::current->data);
  check(!c.isDeviceOpen()&&c.audioDeviceNeedsRecovery(),"finished stream cannot report healthy Running");
  check(bool(c.recoverAudioDevice())&&c.isDeviceOpen(),"lost stream recovers on control thread");
  c.setRecordDirectory(dir.string());auto track=c.addTrack(daw::TrackKind::Audio,"Capture");
  c.seekSeconds(1);c.startRecording(track);
  const auto projectGeneration=c.projectGeneration();const auto undoDepth=c.undoDepth();
  check(!c.newProject() && c.projectGeneration()==projectGeneration && c.project().findTrack(track) &&
        c.isRecording() && c.undoDepth()==undoDepth,"rejected new project preserves document, Undo and recording");
  config.sampleRate=96000;check(!c.applyAudioConfiguration(config)&&c.sampleRate()==48000,"sample-rate change rejected during capture");
  config.sampleRate=48000;config.inputEnabled=false;check(!c.applyAudioConfiguration(config),"device input change rejected during capture");
  check(fake::current->in==8,"all eight non-ASIO inputs opened");
  c.setTrackInputRouting(track,7,1,true);
  for(int i=0;i<300;++i) fake::pump(32,i<100||i>=200,false,i==200?paInputOverflow:0);
  auto result=c.finalizeRecordingCapture();const auto& take=result.tracks.front();
  check(take.frames==9600&&take.capturedFrames==9600&&take.droppedFrames==3200&&take.inputXruns==1,"missing input keeps timeline duration and xrun accounting");
  audio::platform::DecodedAudio audio;bool samples=bool(audio::platform::decodeAudioFile(take.closedWavPath,audio));
  for(unsigned i=0;samples&&i<9600;++i) samples=std::abs(audio.interleaved[i]-(i>=3200&&i<6400?0:.8f))<1e-6;
  check(samples,"input eight and silence gap are written at exact positions");
  c.seekSeconds(2);c.startRecording(track);fake::pump(32,true,true);
  auto timed=c.finalizeRecordingCapture();check(std::abs(timed.tracks.front().startSeconds-1.97)<1e-9,"ADC/DAC delta aligns first capture to audible timeline");
  c.seekSeconds(2);c.startRecording(track);fake::pump(32,true,false,0,true);
  auto estimated=c.finalizeRecordingCapture();
  check(std::abs(estimated.tracks.front().startSeconds-1.97)<1e-9&&!c.audioDeviceDiagnostics().inputUsesDeviceTime,"missing hardware timestamps use explicit input/output latency estimate");
  c.seekSeconds(0);c.startRecording(track);for(int i=0;i<100;++i)fake::pump(32,true,true);
  auto zero=c.finalizeRecordingCapture();check(zero.tracks.front().startSeconds==0&&zero.tracks.front().frames==3200-1440,"negative pre-roll trimmed once at project zero");
  c.startRecording(track);fake::pump(32,false);c.markRecordingInterrupted();c.stopRecording();
  check(!c.recordingWarning().empty(),"local Stop exposes missing audio and interruption");
  c.seekSeconds(5);check(c.startRecording(track),"device-loss take starts");
  for(int i=0;i<100;++i)fake::pump(32);
  fake::current->active=false;fake::current->finished(fake::current->data);
  const int opens=fake::opens;
  check(c.audioDeviceNeedsRecovery()&&!c.recoverAudioDevice()&&c.isRecording()&&fake::opens==opens,
        "device loss cannot reconnect over an unfinished take");
  c.markRecordingInterrupted();const auto retainedPath=c.stopRecording();
  const auto* recorded=c.project().findTrack(track);
  audio::platform::DecodedAudio retained;
  const bool published=recorded&&std::any_of(recorded->clips.begin(),recorded->clips.end(),[&](const auto& clip){
    return clip.filePath==retainedPath||std::any_of(clip.takes.begin(),clip.takes.end(),[&](const auto& take){return take.filePath==retainedPath;});
  });
  check(published&&!retainedPath.empty()&&
        bool(audio::platform::decodeAudioFile(retainedPath,retained))&&
        !retained.interleaved.empty()&&!c.recordingWarning().empty(),
        "device-loss finalization publishes a readable interrupted take");
  check(bool(c.recoverAudioDevice())&&c.isDeviceOpen()&&!c.isRecording(),
        "reconnection after finalization never resumes recording");
 }
 {
  audio::AudioDeviceManager d;d.initialize();d.start();fake::pump(32);
  check(d.diagCallbackCount()==1&&d.diagRenderCallCount()==0&&d.diagLastRenderStatus()==-1,"callback without renderer is diagnosed explicitly");
  fake::pump(32,false);check(d.diagInputPeak(0)==0&&d.diagInputRMS(0)==0,"missing input clears stale meters");
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  check(!d.isRunning()&&d.callbackStalled()&&d.deviceState()==audio::AudioDeviceState::Failed,"active driver without callbacks is diagnosed as stalled");
  fake::devices[1].name="Original";auto config=d.configuration();config.outputDeviceUid="Test: Original";
  check(!d.applyConfiguration(config),"ambiguous legacy display-name ID cannot select first duplicate");fake::devices[1].name="Candidate";
 }
 std::filesystem::remove_all(dir);return failures?1:0;
}
