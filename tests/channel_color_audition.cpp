#include "Internal/ChannelColorInstance.hpp"
#include "DSP/LoudnessMeter.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "platform/PathUtils.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

// Offline, level-matched audition using the production processor. Source and
// destination are explicit; this utility never rewrites the supplied recording.
namespace {
using Samples=std::array<std::vector<float>,2>;
double loudness(Samples& samples,double rate) {
    daw::engine::LoudnessMeter meter; meter.prepare(rate);
    for(unsigned at=0;at<samples[0].size();at+=256) {
        const auto n=unsigned(std::min<size_t>(256,samples[0].size()-at));
        float* channels[]{samples[0].data()+at,samples[1].data()+at};
        meter.process({channels,2,n},n,true);
    }
    return meter.levels().integrated;
}
}
int main(int argc,char** argv) {
    if(argc!=3) {std::fprintf(stderr,"Usage: channel_color_audition source-metadata.json output-directory\n");return 2;}
    std::ifstream input(argv[1],std::ios::binary); nlohmann::json metadata; input>>metadata;
    const auto sourcePath=metadata.at("path").get<std::string>();
    audio::platform::DecodedAudio source;
    const auto decoded=audio::platform::decodeAudioFile(sourcePath,source);
    if(!decoded || !source.frames || source.channels<1 || source.channels>2) {std::fprintf(stderr,"Cannot decode audition source\n");return 1;}
    const unsigned count=unsigned(std::min<audio::FrameCount>(source.frames,audio::FrameCount(source.sampleRate*16)));
    const unsigned step=unsigned(source.sampleRate),margin=unsigned(source.sampleRate);
    unsigned start=0; double maximum=-1;
    // Choose an active 16-second phrase on one-second boundaries, retaining a
    // second of history on each side for magnetic state and the FIR latency.
    for(unsigned at=margin;at+count+margin<source.frames;at+=step) {
        double energy=0;
        for(unsigned i=0;i<count;i+=16) for(unsigned ch=0;ch<source.channels;++ch) {
            const auto value=source.interleaved[(at+i)*source.channels+ch];energy+=value*value;
        }
        if(energy>maximum) {maximum=energy;start=at;}
    }
    std::array<Samples,3> variants;
    for(auto& signal:variants) for(auto& channel:signal) channel.resize(count);
    for(unsigned i=0;i<count;++i) for(unsigned ch=0;ch<2;++ch) variants[0][ch][i]=source.interleaved[(start+i)*source.channels+std::min(ch,unsigned(source.channels-1))];
    constexpr std::array<double,3> drives{0,-20,40};
    constexpr std::array<const char*,3> names{"01-original","02-tape-drive-minus20","03-tape-tube-drive-plus40"};
    for(unsigned variant=1;variant<3;++variant) {
        daw::plugins::channel_color::ChannelColorInstance color;
        color.setProfileSeed(0x564c544f4e450001ULL);color.setParameterFromHost(0,drives[variant]);color.activate({source.sampleRate,256});
        const unsigned from=start>margin?start-margin:0;
        std::array<float,256> left{},right{},wetLeft{},wetRight{};
        const float* ins[]{left.data(),right.data()};float* outs[]{wetLeft.data(),wetRight.data()};
        for(unsigned at=from;at<start+count+48;at+=256) {
            const auto n=std::min(256u,start+count+48-at);
            for(unsigned i=0;i<n;++i) for(unsigned ch=0;ch<2;++ch) {
                const float value=at+i<source.frames?source.interleaved[(at+i)*source.channels+std::min(ch,unsigned(source.channels-1))]:0;
                (ch?right:left)[i]=value;
            }
            daw::plugins::PluginProcessContext ctx;ctx.inputs=ins;ctx.outputs=outs;ctx.frames=n;ctx.inputChannels=ctx.outputChannels=2;color.process(ctx);
            for(unsigned i=0;i<n;++i) if(at+i>=start+48 && at+i<start+count+48)
                for(unsigned ch=0;ch<2;++ch) variants[variant][ch][at+i-start-48]=(ch?wetRight:wetLeft)[i];
        }
    }
    const double reference=loudness(variants[0],source.sampleRate);
    double peak=0;std::array<double,3> offsets{};
    for(unsigned variant=0;variant<3;++variant) {
        offsets[variant]=reference-loudness(variants[variant],source.sampleRate);
        const double gain=std::pow(10.0,offsets[variant]/20);
        for(auto& channel:variants[variant]) for(auto& sample:channel) {sample=float(sample*gain);peak=std::max(peak,std::abs(double(sample)));}
    }
    const double common=std::min(1.0,.95/std::max(peak,1e-12));
    const auto directory=daw::platform::pathFromUtf8(argv[2]);std::filesystem::create_directories(directory);
    nlohmann::json report{{"source",sourcePath},{"sourceSha256",metadata.at("sha256")},{"sampleRate",source.sampleRate},
        {"startSeconds",double(start)/source.sampleRate},{"durationSeconds",double(count)/source.sampleRate},{"matching","ITU-R BS.1770 integrated loudness"},
        {"commonAttenuationDb",20*std::log10(common)},{"variants",nlohmann::json::array()}};
    audio::AudioRecorder writer;writer.initialize(source.sampleRate,2);
    for(unsigned variant=0;variant<3;++variant) {
        audio::AudioBuffer audio(2,count);
        for(unsigned ch=0;ch<2;++ch) for(unsigned i=0;i<count;++i) audio.getChannel(ch)[i]=variants[variant][ch][i]=float(variants[variant][ch][i]*common);
        const auto file=daw::platform::pathToUtf8(directory/(std::string(names[variant])+".wav"));
        if(!writer.writeWAVFile(file,audio,source.sampleRate)) return 1;
        const double actual=loudness(variants[variant],source.sampleRate);
        std::printf("%s: %.4f LUFS; match gain %+.4f dB\n",names[variant],actual,offsets[variant]);
        report["variants"].push_back({{"file",file},{"drive",drives[variant]},{"tone",0},{"lufs",actual},{"matchGainDb",offsets[variant]}});
    }
    std::ofstream output(directory/"comparison.json",std::ios::binary);output<<report.dump(2);
    std::printf("Active excerpt %.3f to %.3f seconds\n",double(start)/source.sampleRate,double(start+count)/source.sampleRate);
    return 0;
}
