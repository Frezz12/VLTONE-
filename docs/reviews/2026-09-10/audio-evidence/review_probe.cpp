#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using daw::EngineController;
constexpr unsigned frames = 32;
float block(EngineController& c) {
    audio::AudioBuffer input(2, frames), output(2, frames);
    std::fill_n(input.getChannel(0), frames, 0.25f);
    std::fill_n(input.getChannel(1), frames, 0.5f);
    c.processDeviceBlockForTest(input, output, frames);
    float peak = 0;
    for (unsigned ch=0; ch<2; ++ch) for(unsigned f=0;f<frames;++f)
        peak = std::max(peak, std::abs(output.getChannel(ch)[f]));
    return peak;
}
int main() {
    EngineController c;
    c.initialize(48000, frames, false);
    c.setRecordDirectory("/private/tmp/vlt-audio-review-2026-09-10/captures");
    c.newProject(true);
    auto first = c.project().tracks.front().id;
    c.setTrackMonitor(first, true);
    for(int i=0;i<100;++i) block(c);
    std::printf("DEFAULT_TRACK enabled=%d monitor=%d output_peak=%.6f\n", c.project().findTrack(first)->inputEnabled,c.project().findTrack(first)->monitor,block(c));
    c.setTrackInputEnabled(first, true);
    for(int i=0;i<100;++i) block(c);
    std::printf("SELECT_INPUT_1 output_peak=%.6f\n", block(c));
    c.setTrackMuted(first,true);
    auto second = c.addTrack(daw::TrackKind::Audio, "New recording");
    c.setTrackInputEnabled(second,true);
    auto prefs=c.recordingPrefs();prefs.autoMonitorOnRecord=true;c.setRecordingPrefs(prefs);
    bool started=c.startRecording(second);
    for(int i=0;i<100;++i) block(c);
    std::printf("MUTED_EXISTING_MONITOR recording=%d new_monitor=%d output_peak=%.6f\n",started,c.project().findTrack(second)->monitor,block(c));
    c.finalizeRecordingCapture();
    c.setTrackMonitor(second,true);
    for(int i=0;i<100;++i) block(c);
    std::printf("MANUAL_MONITOR output_peak=%.6f\n",block(c));
    c.setTrackInputEnabled(second,false);
    auto before=c.graphRebuildCountForTest();
    c.setTrackInputChannel(second,1);
    c.setTrackInputChannelCount(second,2);
    c.setTrackInputEnabled(second,true);
    std::printf("ONE_INPUT_MENU_PICK graph_rebuilds=%llu\n",(unsigned long long)(c.graphRebuildCountForTest()-before));
    c.setTrackInputChannel(second,0); c.setTrackInputChannelCount(second,1);
    std::atomic<bool> running{true}; std::atomic<unsigned> silent{0}, calls{0};
    std::thread render([&]{
        auto next=std::chrono::steady_clock::now();
        while(running.load()) {
            if(block(c)<0.001f) ++silent;
            ++calls;next+=std::chrono::nanoseconds(666667);std::this_thread::sleep_until(next);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto gated=c.gatedAudioBlocks(); auto begin=std::chrono::steady_clock::now();
    for(int i=0;i<4;++i){EngineController::TrackCreationRequest r;r.count=32;std::vector<std::string> ids;c.createTracks(r,ids);}
    auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    running.store(false);render.join();
    std::printf("CREATE_128_TRACKS ms=%.3f gated_blocks=%llu silent_blocks=%u calls=%u\n",ms,(unsigned long long)(c.gatedAudioBlocks()-gated),silent.load(),calls.load());
}
