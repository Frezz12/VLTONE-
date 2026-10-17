#pragma once
#include "Host/PluginInstance.hpp"
#include <array>
#include <chrono>
#include <complex>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <vector>

// Reproducible black-box measurement. Frequencies are integer Hz and the
// analysis window is exactly one second: no leakage window or THD noise bias.
// A half-second warm-up is discarded independently for each point.
inline bool measurePlugin(daw::plugins::PluginInstance& plugin,const std::string& path,
                          double rate,unsigned block,double amplitudeDb,double frequency,
                          double secondFrequency,std::span<const daw::plugins::PluginEvent> edits) {
    std::ofstream file(path);if(!file)return false;
    file<<"frequency_hz,input_peak_dbfs,output_peak_dbfs,gain_db,h2_dbc,h3_dbc,h4_dbc,h5_dbc,h6_dbc,h7_dbc,h8_dbc,h9_dbc,imd_difference_dbc,peak,dc,latency_samples,cpu_seconds,folded_harmonics_dbc\n"<<std::setprecision(12);
    std::vector<double> frequencies;
    if(frequency>0)frequencies.push_back(frequency);else for(double f:{20.,30.,40.,50.,60.,80.,100.,160.,220.,300.,360.,500.,700.,1000.,1600.,2400.,3200.,4800.,7200.,10000.,12000.,16000.,20000.})if(f<rate*.49)frequencies.push_back(f);
    std::vector<float> input(block),output(block),other(block);const float* ins[]{input.data(),input.data()};float* outs[]{output.data(),other.data()};
    const auto db=[](double v){return 20*std::log10(std::max(std::abs(v),1e-20));};
    for(double hz:frequencies){
        if(hz!=std::floor(hz)||hz<=0||hz>=rate*.5||secondFrequency>=rate*.5)return false;
        plugin.reset();
        const unsigned settle=unsigned(rate*.5),count=unsigned(rate),total=settle+count;
        std::array<std::complex<double>,10> sums{};double peak=0,mean=0,cpu=0;
        const double amplitude=std::pow(10.,amplitudeDb/20);
        for(unsigned at=0;at<total;at+=block){const unsigned n=std::min(block,total-at);
            for(unsigned i=0;i<n;++i){double t=(at+i)/rate;input[i]=float(amplitude*(std::sin(2*std::numbers::pi*hz*t)+(secondFrequency>0?std::sin(2*std::numbers::pi*secondFrequency*t):0)));}
            daw::plugins::PluginProcessContext c;c.inputs=ins;c.outputs=outs;c.inputChannels=c.outputChannels=2;c.frames=n;c.playing=true;c.offline=true;c.sampleTime=at;
            c.transport.tempo=120;c.transport.ppqPosition=at/rate*2;if(at==0)c.inputEvents=edits;
            const auto start=std::chrono::steady_clock::now();const auto result=plugin.process(c);cpu+=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            if(result==daw::plugins::PluginProcessDisposition::Error)return false;plugin.pumpMainThread();
            for(unsigned i=0;i<n;++i)if(at+i>=settle){double y=output[i];if(!std::isfinite(y))return false;peak=std::max(peak,std::abs(y));mean+=y;const double t=(at+i-settle)/rate;
                const std::complex<double> fundamental=std::polar(1.,-2*std::numbers::pi*hz*t);auto rotor=fundamental;
                for(unsigned h=0;h<9;++h){sums[h]+=y*rotor;rotor*=fundamental;}
                if(secondFrequency>0)sums[9]+=y*std::polar(1.,-2*std::numbers::pi*std::abs(secondFrequency-hz)*t);
            }
        }
        const double fundamental=2*std::abs(sums[0])/count;
        file<<hz<<','<<amplitudeDb<<','<<db(fundamental)<<','<<db(fundamental)-amplitudeDb;
        for(unsigned h=1;h<9;++h)file<<','<<(hz*(h+1)<rate*.5?db(std::abs(sums[h])/std::max(1e-30,std::abs(sums[0]))):-400);
        double folded=0;
        for(unsigned h=1;h<9;++h)if(hz*(h+1)>=rate*.5){double f=std::remainder(hz*(h+1),rate);if(std::abs(std::abs(f)-hz)>1)folded+=std::norm(sums[h]);}
        file<<','<<(secondFrequency>0?db(std::abs(sums[9])/std::max(1e-30,std::abs(sums[0]))):-400)<<','<<peak<<','<<mean/count<<','<<plugin.latencySamples()<<','<<cpu<<','<<db(std::sqrt(folded)/std::max(1e-30,std::abs(sums[0])))<<'\n';
        std::printf("MEASURE %.0f Hz, input %.1f dBFS, gain %.4f dB, H2 %.2f dBc, H3 %.2f dBc\n",hz,amplitudeDb,db(fundamental)-amplitudeDb,db(std::abs(sums[1])/std::max(1e-30,std::abs(sums[0]))),db(std::abs(sums[2])/std::max(1e-30,std::abs(sums[0]))));
    }
    return bool(file);
}
