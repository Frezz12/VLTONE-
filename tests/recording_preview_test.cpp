#include "EngineController.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("vlt-recording-preview-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    // GUI reads only after 2 seconds, well after the impulse's audio block.
    for (unsigned block : {32, 64, 128, 512}) {
        audio::AudioRecorder recorder;
        recorder.initialize(48000, 2);
        recorder.setRecordPath(dir.string());
        recorder.setInputChannels(2, 1);
        check(recorder.startRecording(1, 0).isOk(), "start delayed-reader capture");
        audio::AudioBuffer input(4, block);
        constexpr unsigned total = 96000;
        for (unsigned start = 0; start < total; start += block) {
            const auto frames = std::min(block, total - start);
            for (unsigned f = 0; f < frames; ++f) {
                input.getChannel(0)[f] = 0.9f;
                input.getChannel(1)[f] = 0.95f;
                input.getChannel(2)[f] = (start + f) % 1200 == 73 ? -0.8f : 0.1f;
                input.getChannel(3)[f] = 0.99f;
            }
            recorder.process(input, frames);
        }
        bool peaks = recorder.recordedFrames() == total;
        for (unsigned i = 0; i < 80; ++i) {
            float peak = 0;
            peaks &= recorder.readPeakBucket(i, peak) && std::fabs(peak - .8f) < 1e-6;
        }
        check(peaks, "25 ms buckets retain input 3 transients without polling or neighboring-channel leakage");
        check(recorder.stopRecording().isOk(), "finalize delayed-reader capture");
        audio::platform::DecodedAudio wave;
        bool exact = audio::platform::decodeAudioFile(recorder.session().filePath, wave).isOk() && wave.frames == total;
        if (exact) for (unsigned f = 0; f < total; ++f) {
            const float expected = f % 1200 == 73 ? -.8f : .1f;
            exact &= std::fabs(wave.interleaved[f*2] - expected) < 1e-6 &&
                std::fabs(wave.interleaved[f*2+1] - expected) < 1e-6;
        }
        check(exact, "mono route is duplicated coherently into the already-open stereo WAV");
    }
    {
        audio::AudioRecorder recorder;
        recorder.initialize(48000, 2); recorder.setRecordPath(dir.string());
        recorder.setInputChannels(0, 1);
        recorder.startRecording(2, 0);
        constexpr unsigned block = 64, blocks = 1500;
        audio::AudioBuffer input(4, block);
        const std::array<float,4> levels{.1f,.3f,.6f,.8f};
        for (unsigned ch=0;ch<4;++ch) std::fill_n(input.getChannel(ch), block, levels[ch]);
        std::atomic<bool> run{true};
        std::thread control([&] {
            while (run.load(std::memory_order_relaxed)) {
                recorder.setInputChannels(0, 1);
                recorder.setInputChannels(2, 2);
                recorder.setInputChannels(1, 1, false);
            }
        });
        for (unsigned i=0;i<blocks;++i) recorder.process(input,block);
        run.store(false); control.join();
        recorder.stopRecording();
        audio::platform::DecodedAudio wave;
        bool coherent = audio::platform::decodeAudioFile(recorder.session().filePath,wave).isOk() && wave.frames==block*blocks;
        if (coherent) for (unsigned i=0;i<blocks;++i) {
            const float l=wave.interleaved[i*block*2], r=wave.interleaved[i*block*2+1];
            coherent &= (l==.1f && r==.1f) || (l==.6f && r==.8f) || (l==0 && r==0);
            for(unsigned f=0;f<block;++f) coherent &= wave.interleaved[(i*block+f)*2]==l && wave.interleaved[(i*block+f)*2+1]==r;
        }
        check(coherent, "concurrent first/count/enabled edits cannot tear a route within an audio block");
    }
    {
        daw::EngineController controller;
        controller.initialize(48000,32,false);
        controller.setRecordDirectory(dir.string());
        const auto track=controller.addTrack(daw::TrackKind::Audio,"Input 3");
        controller.setTrackInputChannel(track,2);
        controller.setTrackInputChannelCount(track,1);
        controller.setTrackInputEnabled(track,true);
        controller.startRecording(track);
        audio::AudioBuffer input(4,32), output(2,32);
        for(unsigned ch=0;ch<4;++ch) std::fill_n(input.getChannel(ch),32,ch==2?.4f:.9f);
        for(unsigned i=0;i<75;++i) controller.processDeviceBlockForTest(input,output,32);
        controller.pumpRecordingEnvelopes();
        auto preview=controller.recordingPreview(track);
        check(preview.envelope.size()==2 && preview.envelope[0]==.4f && preview.envelope[1]==.4f,
            "real callback and controller use the selected input for preview");
        controller.setTrackInputEnabled(track,false);
        for(unsigned i=0;i<75;++i) controller.processDeviceBlockForTest(input,output,32);
        controller.pumpRecordingEnvelopes();
        preview=controller.recordingPreview(track);
        check(preview.envelope.size()==4 && preview.envelope[2]==0 && preview.envelope[3]==0,
            "No Input immediately silences an active capture through the controller");
        controller.setTrackInputEnabled(track,true);
        for(unsigned i=0;i<75;++i) controller.processDeviceBlockForTest(input,output,32);
        controller.pumpRecordingEnvelopes();
        preview=controller.recordingPreview(track);
        check(preview.envelope.size()==6 && preview.envelope[4]==.4f && preview.envelope[5]==.4f,
            "re-enabling the input resumes capture without replacing the recorder");
        controller.stopRecording();
    }
    // Hundreds of old loop passes must not become hundreds of UI primitives.
    for (bool keepTakes : {false, true}) {
        daw::EngineController controller;
        controller.initialize(48000,512,false);
        controller.setRecordDirectory(dir.string());
        const auto track=controller.addTrack(daw::TrackKind::Audio,"Loop");
        auto prefs=controller.recordingPrefs(); prefs.loopCreatesTakes=keepTakes;
        prefs.mode=daw::RecordMode::Layers; controller.setRecordingPrefs(prefs);
        controller.setLoopRangeSeconds(0, .1); controller.setLoopEnabled(true);
        controller.seekSeconds(0); controller.startRecording(track);
        controller.seedRecordingForShot(track,30.125);
        const auto preview=controller.recordingPreview(track);
        check(preview.passCount==302 && preview.spans.size()==(keepTakes?2:1),
            "302 recorded passes occupy at most two live preview spans");
        check(std::fabs(preview.spans.back().captureOffsetSeconds-30.1)<1e-7 &&
              std::fabs(preview.spans.back().endSeconds-.025)<1e-7,
            "long-loop preview preserves capture-time alignment of the live pass");
        controller.stopRecording();
        const auto* landed=controller.project().findTrack(track);
        std::size_t takes=0;
        for (const auto& clip:landed->clips) takes+=clip.takes.size();
        check(!keepTakes || takes>=302,
            "bounded live preview still lands every recorded loop take");
    }
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
