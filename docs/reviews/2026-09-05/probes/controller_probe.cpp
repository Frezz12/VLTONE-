#include "EngineController.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sys/resource.h>
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
double ms(Clock::time_point from) { return std::chrono::duration<double,std::milli>(Clock::now()-from).count(); }
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string dir = "/tmp/vlt-review-20260905/media";
    fs::create_directories(dir);
    daw::EngineController c;
    if (!c.initialize(48000,64,false)) return 1;
    float l[4800], r[4800];
    std::fill_n(l,4800,0.1f); std::fill_n(r,4800,0.1f);
    const float* channels[]{l,r};
    for (int i=0;i<8;++i) {
        const auto path = dir+"/source-"+std::to_string(i)+".wav";
        audio::platform::AudioFileWriter writer;
        if (!writer.open(path,48000,2,48000*30)) return 2;
        for (int n=0;n<300;++n) if (!writer.write(channels,4800)) return 3;
        if (!writer.close()) return 4;
        auto id = c.addTrack(daw::TrackKind::Audio,"track "+std::to_string(i));
        if (c.importAudio(path,id,0).empty()) return 5;
    }
    daw::rendering::Spec spec;
    spec.outputDir=dir; spec.baseName="mix";
    spec.range=daw::rendering::Range::Custom;
    spec.customEndSeconds=0.1; spec.tail=daw::rendering::Tail::None;
    for(double rate:{48000.,96000.}) {
        spec.sampleRate=rate;
        const auto started=Clock::now(); double first=-1;
        daw::rendering::Report report;
        auto status=c.renderProject(spec,[&](const daw::rendering::Progress&){first=ms(started);return false;},report);
        rusage usage{}; getrusage(RUSAGE_SELF,&usage);
        std::printf("rate=%.0f first callback=%.2f ms total incl. restore=%.2f ms peak RSS=%.1f MiB status=%d\n",rate,first,ms(started),usage.ru_maxrss/1048576.,bool(status));
    }
    spec.sampleRate=48000;
    daw::rendering::Report good,cancelled;
    auto status=c.renderProject(spec,{},good);
    if (!status || good.files.empty()) return 6;
    const auto path=good.files.front();
    std::printf("previous successful output exists=%d size=%lld\n",fs::exists(path),(long long)fs::file_size(path));
    c.renderProject(spec,[](const auto&){return false;},cancelled);
    std::printf("after cancelling next export: cancelled=%d previous output exists=%d\n",cancelled.cancelled,fs::exists(path));
    c.shutdown();
}
