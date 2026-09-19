// Opt-in local vocal audition/measurement. Never uploads audio or modifies the
// source. Outputs are sample-aligned after trimming exactly the reported PDC.
#include "Internal/PitchCorrectorInstance.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

using namespace daw::plugins;
using namespace daw::plugins::pitch;
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
bool write(const std::filesystem::path& file, const std::array<std::vector<float>, 2>& audio,
           unsigned channels, double rate) {
    audio::platform::AudioFileWriter writer;
    if (!writer.open(utf8(file), rate, audio::ChannelCount(channels), audio[0].size())) return false;
    const float* data[]{audio[0].data(), audio[1].data()};
    return writer.write(data, audio::FrameCount(audio[0].size())).isOk() && writer.close().isOk();
}
int run(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::fprintf(stderr, "usage: pitch_corrector_bench INPUT_WAV OUTPUT_DIR [seconds=24] [block=64] [scale=0] [key=0]\n");
        return 2;
    }
    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(args[1], decoded)) { std::fprintf(stderr, "Cannot decode source\n"); return 2; }
    if (decoded.channels < 1 || decoded.channels > 2 || decoded.frames < 1) return 2;
    const double seconds = args.size() > 3 ? std::stod(args[3]) : 24;
    const unsigned block = args.size() > 4 ? unsigned(std::stoul(args[4])) : 64;
    if (!block || block > 8192) return 2;
    const auto destination = std::filesystem::u8path(args[2]);
    std::filesystem::create_directories(destination);
    const unsigned channels = decoded.channels;
    const double rate = decoded.sampleRate;
    std::size_t first = 0;
    // Start a quarter second before the first meaningful phrase, instead of
    // accidentally auditing only the empty beginning of a consolidated take.
    if (seconds > 0) {
        for (std::size_t i = 0; i < decoded.interleaved.size(); i += channels) {
            if (std::abs(decoded.interleaved[i]) > .025f) { first = i / channels; break; }
        }
        first -= std::min(first, std::size_t(rate * .25));
    }
    const auto frames = seconds > 0 ? std::min<std::size_t>(decoded.frames - first, std::size_t(rate * seconds)) : std::size_t(decoded.frames);
    std::array<std::vector<float>, 2> input;
    for (auto& channel : input) channel.resize(frames);
    double inputPeak = 0, inputEnergy = 0;
    for (std::size_t i = 0; i < frames; ++i) for (unsigned channel = 0; channel < channels; ++channel) {
        const float value = decoded.interleaved[(first+i)*channels+channel];
        input[channel][i] = value;
        inputPeak = std::max(inputPeak, std::abs(double(value))); inputEnergy += double(value)*value;
    }
    if (channels == 1) input[1] = input[0];
    if (!write(destination / "dry.wav", input, channels, rate)) return 2;
    std::ofstream summary(destination / "metrics.csv");
    summary << "mode,preset,rate,channels,frames,latency_samples,latency_ms,input_peak,output_peak,input_rms,output_rms,voiced_percent,mean_abs_correction_cents,cpu_percent,block_p95_us,block_p99_us,block_max_us,block_deadline_us,nonfinite_samples,clipped_samples\n";
    std::printf("Local vocal: %.0f Hz, %u channels, source %.3fs; measuring %.3fs from %.3fs, input peak %.6f\n",
        rate, channels, decoded.frames/rate, frames/rate, first/rate, inputPeak);
    bool valid = true;
    for (int quality : {0,1}) for (int presetIndex : {0,3}) {
        PitchCorrectorInstance plugin;
        const auto& preset = factoryPresets()[presetIndex];
        plugin.setParameterFromHost(unsigned(Param::Tune), preset.tune);
        plugin.setParameterFromHost(unsigned(Param::Humanize), preset.humanize);
        plugin.setParameterFromHost(unsigned(Param::Vibrato), preset.vibrato);
        plugin.setParameterFromHost(unsigned(Param::Quality), quality);
        if (args.size() > 5) plugin.setParameterFromHost(unsigned(Param::Scale), std::stod(args[5]));
        if (args.size() > 6) plugin.setParameterFromHost(unsigned(Param::Key), std::stod(args[6]));
        PluginBusLayout accepted;
        plugin.setBusLayout({{std::uint16_t(channels)}, {std::uint16_t(channels)}}, accepted);
        if (!plugin.activate({rate, block, false})) return 2;
        plugin.startProcessing();
        const auto delay = plugin.latencySamples();
        const auto total = frames + delay;
        std::array<std::vector<float>,2> output, inBlock, outBlock;
        for (auto& channel : output) channel.resize(frames);
        for (auto& channel : inBlock) channel.resize(block);
        for (auto& channel : outBlock) channel.resize(block);
        std::vector<double> durations; durations.reserve(total / block + 1);
        const std::string name = std::string(quality ? "hd-" : "realtime-") + (presetIndex ? "hard" : "natural");
        std::ofstream trajectory(destination / (name + "-pitch.csv"));
        trajectory << "time_seconds,input_hz,target_hz,confidence,correction_cents,voiced\n";
        double energy = 0, peak = 0, correction = 0;
        std::size_t voiced = 0, observations = 0, invalid = 0, clipped = 0;
        for (std::size_t offset = 0; offset < total; offset += block) {
            const auto count = unsigned(std::min<std::size_t>(block, total-offset));
            for (unsigned c = 0; c < channels; ++c) for (unsigned i = 0; i < count; ++i)
                inBlock[c][i] = offset+i < frames ? input[c][offset+i] : 0;
            const float* ins[]{inBlock[0].data(), inBlock[1].data()};
            float* outs[]{outBlock[0].data(), outBlock[1].data()};
            PluginProcessContext context;
            context.inputs = ins; context.outputs = outs; context.inputChannels = context.outputChannels = std::uint16_t(channels);
            context.frames = count; context.sampleTime = std::int64_t(offset);
            const auto start = std::chrono::steady_clock::now();
            plugin.process(context);
            durations.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count());
            for (unsigned c = 0; c < channels; ++c) for (unsigned i = 0; i < count; ++i) {
                if (offset+i < delay || offset+i-delay >= frames) continue;
                const double value = outBlock[c][i];
                invalid += !std::isfinite(value); clipped += std::abs(value) >= 1;
                output[c][offset+i-delay] = float(value);
                peak = std::max(peak, std::abs(value)); energy += value*value;
            }
            const auto telemetry = plugin.telemetrySnapshot();
            if (offset < frames) {
                ++observations;
                if (telemetry.voiced) { ++voiced; correction += std::abs(telemetry.correctionCents); }
                trajectory << offset/rate << ',' << telemetry.inputHz << ',' << telemetry.targetHz << ','
                    << telemetry.confidence << ',' << telemetry.correctionCents << ',' << telemetry.voiced << '\n';
            }
        }
        double totalMicros = 0;
        for (double duration : durations) totalMicros += duration;
        std::sort(durations.begin(), durations.end());
        const double p95 = durations[std::min(durations.size()-1, std::size_t(durations.size()*.95))];
        const double p99 = durations[std::min(durations.size()-1, std::size_t(durations.size()*.99))];
        const double cpu = totalMicros/1e6/(total/rate)*100;
        std::printf("%s: latency %.3fms, peak %.6f, RMS %.6f, voiced %.1f%%, mean correction %.2fc, CPU %.2f%%, p99 %.1fus / deadline %.1fus, invalid=%zu clip=%zu\n",
            name.c_str(), delay*1000/rate, peak, std::sqrt(energy/(frames*channels)), 100.*voiced/std::max<std::size_t>(1,observations),
            correction/std::max<std::size_t>(1,voiced), cpu, p99, block*1e6/rate, invalid, clipped);
        summary << (quality ? "HD" : "Real-Time") << ',' << preset.name << ',' << rate << ',' << channels << ',' << frames << ',' << delay << ','
            << 1000*delay/rate << ',' << inputPeak << ',' << peak << ',' << std::sqrt(inputEnergy/(frames*channels)) << ',' << std::sqrt(energy/(frames*channels)) << ','
            << 100.*voiced/std::max<std::size_t>(1,observations) << ',' << correction/std::max<std::size_t>(1,voiced) << ',' << cpu << ',' << p95 << ',' << p99 << ','
            << durations.back() << ',' << block*1e6/rate << ',' << invalid << ',' << clipped << '\n';
        valid = write(destination/(name+".wav"), output, channels, rate) && invalid == 0 && valid;
    }
    return valid ? 0 : 1;
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(std::size_t(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text.data(), count, nullptr, nullptr);
        text.pop_back(); args.push_back(std::move(text));
    }
    return run(args);
}
#else
int main(int argc, char** argv) { return run({argv, argv+argc}); }
#endif
