#include "EngineController.hpp"
#include "Core/AudioBuffer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    daw::EngineController c;c.initialize(48000,32,false);
    auto target=c.addTrack(daw::TrackKind::Audio,"Live microphone");
    daw::plugins::PluginDescriptor descriptor;descriptor.format=daw::plugins::Format::Clap;descriptor.uid="com.daw.test.gain";
    descriptor.path="/private/tmp/vlt-audio-review-2026-09-10/SlowState.clap";descriptor.name="20 ms state fixture";
    const auto inserted=c.addInsert(target,descriptor);
    if(inserted.empty()){std::printf("INSERT_FAILED\n");return 1;}
    c.setTrackInputEnabled(target,true);c.setTrackMonitor(target,true);
    unsigned live=0;for(const auto& t:c.project().tracks)for(const auto& i:t.inserts)if(c.insertInstance(t.id,i.id))++live;
    std::printf("PROJECT opened=1 live_inserts=%u monitor=%d playing=%d\n",live,c.isInputMonitoringActive(),c.isPlaying());std::fflush(stdout);
    audio::AudioBuffer in(2,32),out(2,32);std::fill_n(in.getChannel(0),32,.25f);
    std::atomic<bool> running{true};std::atomic<unsigned> calls{0};
    std::thread render([&]{auto next=std::chrono::steady_clock::now();while(running){c.processDeviceBlockForTest(in,out,32);++calls;next+=std::chrono::nanoseconds(666667);std::this_thread::sleep_until(next);}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for(int i=0;i<5;++i){
        const auto before=c.gatedAudioBlocks();const auto start=std::chrono::steady_clock::now();
        c.refreshRecoveryPluginStates(1);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::printf("RECOVERY_TICK index=%d ms=%.3f gated_blocks=%llu\n",i,ms,(unsigned long long)(c.gatedAudioBlocks()-before));std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    running=false;render.join();std::printf("TOTAL callbacks=%u gated=%llu\n",calls.load(),(unsigned long long)c.gatedAudioBlocks());
}
