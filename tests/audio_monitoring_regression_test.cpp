#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
int failures=0;
void check(bool ok,const char* what){std::printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)++failures;}
float render(daw::EngineController& c){audio::AudioBuffer in(4,32),out(2,32);for(unsigned i=0;i<4;++i)std::fill_n(in.getChannel(i),32,.25f*(i+1));for(int i=0;i<100;++i)c.processDeviceBlockForTest(in,out,32);return out.getChannel(0)[31]+out.getChannel(1)[31];}
int main(){
 const auto dir=std::filesystem::temp_directory_path()/("vlt-monitor-regression-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(dir);
 {
  daw::EngineController c;c.initialize(48000,32,false);c.setRecordDirectory(dir.string());c.newProject(true);
  const auto first=c.project().tracks.front().id;c.setTrackMonitor(first,true);
  check(std::abs(render(c)-.5f)<1e-5,"new project monitor works without selecting Input 1");
  const auto count=c.graphRebuildCountForTest();for(int i=0;i<100;++i)c.setTrackInputRouting(first,i%2,1+i%2,true);
  check(c.graphRebuildCountForTest()==count,"routing edits do not rebuild the project graph");
  c.setTrackInputRouting(first,0,1,false);check(render(c)==0,"explicit No Input remains silent");
  std::string json;daw::ProjectSerializer::serializeDocument(c.project(),json);daw::ProjectModel loaded;
  check(bool(daw::ProjectSerializer::deserializeDocument(loaded,json))&&!loaded.findTrack(first)->inputEnabled,"new No Input survives persistence while monitor is on");
  const std::string marker="\"inputRoutingVersion\":1,";const auto at=json.find(marker);
  check(at!=std::string::npos,"input routing migration marker written");if(at!=std::string::npos)json.erase(at,marker.size());
  check(bool(daw::ProjectSerializer::deserializeDocument(loaded,json))&&loaded.findTrack(first)->inputEnabled,"legacy monitored track migrates to active Input 1");
  c.setTrackInputRouting(first,0,1,true);c.setTrackMuted(first,true);
  auto second=c.addTrack(daw::TrackKind::Audio,"Record");auto prefs=c.recordingPrefs();prefs.autoMonitorOnRecord=true;c.setRecordingPrefs(prefs);
  check(c.startRecording(second)&&c.project().findTrack(second)->monitor,"muted old monitor does not suppress new microphone");
  check(std::abs(render(c)-.5f)<1e-5,"muted owner leaves live microphone audible");
  c.setTrackMuted(first,false);check(!c.project().findTrack(second)->monitor,"audible manual owner suppresses duplicate automatically");
  c.setTrackMuted(first,true);check(c.project().findTrack(second)->monitor,"muting owner reopens automatic monitor");
  c.setTrackMuted(first,false);c.setTrackInputRouting(second,0,2,true);
  check(c.project().findTrack(second)->monitor&&c.project().findTrack(second)->monitorInputMask==2,"partial stereo overlap suppresses only shared physical input");
  c.finalizeRecordingCapture();check(!c.project().findTrack(second)->monitor,"Stop restores previous monitor state");
  c.setTrackMonitor(first,false);c.setTrackMonitor(second,false);c.armCountIn({second},4);c.cancelCountIn();
  check(!c.project().findTrack(second)->monitor&&!c.project().findTrack(second)->monitorAuto,"count-in cancellation restores monitor ownership");
  c.setTrackMonitor(first,true);const auto gates=c.gatedAudioBlocks();
  check(c.liveAudioActivity()&&!c.refreshRecoveryPluginStates(1)&&c.gatedAudioBlocks()==gates,"recovery state refresh cannot gate live monitoring");
  daw::EngineController::TrackCreationRequest request;std::vector<std::string> ids;request.count=2;
  check(bool(c.createTracks(request,ids))&&ids.size()==2&&c.project().findTrack(ids.front())->inputEnabled,"track creation defaults to enabled mono input");
 }
 {
  daw::EngineController c;c.initialize(48000,32,false);auto track=c.addTrack(daw::TrackKind::Midi,"Live MIDI");
  check(c.liveMidiEvent(track,0x90,60,100),"live MIDI accepted");
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  check(c.liveAudioActivity(),"held MIDI protects recovery after the recent-event timeout");
  c.liveMidiEvent(track,0xb0,64,127);c.liveMidiEvent(track,0x80,60,0);c.liveMidiEvent(track,0xb0,123,0);
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  check(c.liveAudioActivity(),"sustain survives All Notes Off for recovery protection");
  c.liveMidiEvent(track,0xb0,64,0);std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  check(!c.liveAudioActivity(),"released silent MIDI becomes eligible for idle state capture");
 }
 {
  audio::AudioRecorder rec;rec.initialize(48000,1);rec.setRecordPath(dir.string());rec.startRecording(3,0);
  const auto path=rec.session().filePath;
  std::filesystem::rename(path,path+".moved"); // force final header failure, regardless of writer scheduling
  check(!rec.stopRecording()&&!rec.session().fileWriteSucceeded,"failed WAV finalization cannot report durable success");
 }
 {
  daw::engine::RealtimeEngine engine;engine.prepare(48000,32,2);
  float left[64]{},right[64]{};float* channels[]{left,right};
  daw::engine::AudioBlock output(channels,2,64);
  engine.renderBlock(output,nullptr,0,64);
  check(engine.lastBlockResult()==daw::engine::RealtimeEngine::BlockResult::Failed&&engine.failedBlocks()==1&&engine.lastRenderError()>=0,"graph failure preserves its error code and increments the failure counter");
  const daw::engine::RealtimeEngine::RenderGate gate(engine);
  engine.renderBlock(output,nullptr,0,64);
  check(engine.lastBlockResult()==daw::engine::RealtimeEngine::BlockResult::Gated&&engine.failedBlocks()==1&&engine.gatedBlocks()==1,"intentional render gate is distinguished from processing failure");
 }
 // Force overflow deterministically with a block larger than the complete ring.
 // Verify both a middle hole and a trailing hole without racing disk speed.
 for(bool tail:{false,true}){
  audio::AudioRecorder rec;rec.initialize(48000,1);rec.setRecordPath(dir.string());rec.startRecording(1,0);
  audio::AudioBuffer input(1,32);std::fill_n(input.getChannel(0),32,.4f);rec.process(input,32);
  rec.process(nullptr,300000); // too large to fit, 6.25 seconds of lost input
  if(!tail)rec.process(input,32);rec.stopRecording();
  auto session=rec.session();audio::platform::DecodedAudio wave;
  bool okay=bool(audio::platform::decodeAudioFile(session.filePath,wave))&&wave.frames==(tail?300032u:300064u)&&session.droppedFrames==300000;
  if(okay)for(size_t i=0;i<wave.frames;++i)okay&=std::abs(wave.interleaved[i]-(i<32||(!tail&&i>=300032)?.4f:0))<1e-6;
  check(okay,tail?"trailing overflow is padded through final capture position":"overflow preserves exact position of subsequent audio");
 }
 std::filesystem::remove_all(dir);return failures?1:0;
}
