#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <sndfile.h>

#include "daw/graph/AudioFileSource.h"
#include "daw/graph/GraphCompiler.h"
#include "daw/graph/GainNode.h"
#include "daw/graph/ProcessGraph.h"
#include "daw/graph/SummingBus.h"
#include "daw/model/Clip.h"
#include "daw/model/Source.h"

using namespace daw::graph;
using namespace daw::model;
using Catch::Approx;

namespace {

std::string createTestWav(int sampleRate, int channels, std::int64_t numFrames, double freq)
{
    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path() / "daw_test";
    fs::create_directories(tmpDir);

    auto path = (tmpDir / ("test_" + std::to_string(numFrames) + "_"
                         + std::to_string(freq) + ".wav")).string();

    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels = channels;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!f)
        return {};

    std::vector<double> buffer(static_cast<std::size_t>(numFrames) * channels);
    for (std::int64_t i = 0; i < numFrames; ++i) {
        double val = std::sin(2.0 * 3.141592653589793 * freq * i / sampleRate);
        for (int c = 0; c < channels; ++c)
            buffer[static_cast<std::size_t>(i) * channels + c] = val;
    }

    sf_writef_double(f, buffer.data(), numFrames);
    sf_close(f);

    return path;
}

} // namespace

TEST_CASE("Source: loads WAV file and reads frames", "[audio][source]") {
    auto path = createTestWav(48000, 2, 48000, 440.0);
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    CHECK(src.channels() == 2);
    CHECK(src.sampleRate() == 48000);
    CHECK(src.frames() == 48000);
    CHECK(src.durationSeconds() == Approx(1.0));

    const float* data = src.readFrames(0, 100);
    REQUIRE(data != nullptr);

    CHECK(src.channels() == 2);
    CHECK(src.sampleRate() == 48000);

    float maxVal = 0.0f;
    for (int i = 0; i < 100; ++i)
        maxVal = std::max(maxVal, std::abs(data[i]));
    CHECK(maxVal > 0.5f);

    data = src.readFrames(47999, 1);
    REQUIRE(data != nullptr);
}

TEST_CASE("Source: returns null for invalid read", "[audio][source]") {
    auto path = createTestWav(44100, 1, 1000, 1000.0);
    REQUIRE_FALSE(path.empty());

    Source src;
    REQUIRE(src.loadFromFile(path));

    CHECK(src.readFrames(-1, 10) == nullptr);
    CHECK(src.readFrames(2000, 10) == nullptr);
}

TEST_CASE("AudioFileSource: plays clip segments", "[audio][source]") {
    auto path = createTestWav(48000, 2, 48000, 261.63);

    auto source = std::make_shared<Source>();
    REQUIRE(source->loadFromFile(path));

    auto clip = std::make_shared<Clip>(source, 0, 48000);

    AudioFileSource afs(clip);

    afs.prepare(48000, 256);

    daw::audio::AudioBuffer buf(2, 256);
    auto view = buf.view();

    daw::audio::AudioBufferView outArr[] = {view};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 256;
    ctx.playing = true;
    ctx.playPosition = 0;

    afs.process(ctx);

    float sum = 0.0f;
    for (int i = 0; i < 256; ++i)
        sum += std::abs(view.channel(0)[i]);
    CHECK(sum > 0.0f);
}

TEST_CASE("AudioFileSource: silent before and after the clip window", "[audio][source]") {
    auto path = createTestWav(48000, 1, 1000, 500.0);

    auto source = std::make_shared<Source>();
    REQUIRE(source->loadFromFile(path));

    // Клип cтоит на таймлайне cо 2000-го cэмпла и длитcя 1000 cэмплов.
    auto clip = std::make_shared<Clip>(source, 0, 1000, 2000);

    AudioFileSource afs(clip);
    afs.prepare(48000, 512);

    daw::audio::AudioBuffer buf(1, 512);
    auto view = buf.view();
    daw::audio::AudioBufferView outArr[] = {view};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 512;
    ctx.playing = true;

    auto blockEnergy = [&](std::int64_t position) {
        ctx.playPosition = position;
        afs.process(ctx);
        float sum = 0.0f;
        for (int i = 0; i < 512; ++i)
            sum += std::abs(view.channel(0)[i]);
        return sum;
    };

    // Блок целиком до клипа — тишина.
    CHECK(blockEnergy(0) == Approx(0.0f));

    // Блок внутри клипа — звук еcть.
    CHECK(blockEnergy(2000) > 0.0f);

    // Блок целиком поcле клипа — cнова тишина.
    CHECK(blockEnergy(4000) == Approx(0.0f));
}

TEST_CASE("AudioFileSource: stops at clip boundary", "[audio][source]") {
    auto path = createTestWav(48000, 1, 1000, 500.0);

    auto source = std::make_shared<Source>();
    REQUIRE(source->loadFromFile(path));

    // Клип начинаетcя в нуле таймлайна и кончаетcя на 1000-м cэмпле.
    auto clip = std::make_shared<Clip>(source, 0, 1000, 0);

    AudioFileSource afs(clip);
    afs.prepare(48000, 512);

    daw::audio::AudioBuffer buf(1, 512);
    auto view = buf.view();
    daw::audio::AudioBufferView outArr[] = {view};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 512;
    ctx.playing = true;

    // Блок 768..1279 переcекает конец клипа: первые 232 cэмпла звучат,
    // оcтаток обязан быть тишиной — иначе за границей клипа поедет муcор.
    ctx.playPosition = 768;
    afs.process(ctx);

    float insideClip = 0.0f;
    for (int i = 0; i < 232; ++i)
        insideClip += std::abs(view.channel(0)[i]);
    CHECK(insideClip > 0.0f);

    for (int i = 232; i < 512; ++i)
        CHECK(view.channel(0)[i] == Approx(0.0f));
}

TEST_CASE("AudioFileSource: works in graph pipeline", "[audio][source][graph]") {
    auto path = createTestWav(48000, 2, 5000, 440.0);

    auto source = std::make_shared<Source>();
    REQUIRE(source->loadFromFile(path));

    auto clip = std::make_shared<Clip>(source, 0, 5000);

    GraphCompiler compiler;
    compiler.addNode(std::make_unique<AudioFileSource>(clip));
    auto gain = std::make_unique<GainNode>();
    gain->setGain(0.5f);
    compiler.addNode(std::move(gain));
    compiler.addConnection(0, 1);

    auto graph = compiler.compile(48000.0, 256);

    daw::audio::AudioBuffer outBuf(2, 256);
    auto outView = outBuf.view();
    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 256;
    ctx.playing = true;

    graph->process(ctx);

    float sum = 0.0f;
    for (int i = 0; i < 256; ++i)
        sum += std::abs(outView.channel(0)[i]);
    CHECK(sum > 0.0f);
}

TEST_CASE("AudioFileSource: multiple clips on same source", "[audio][source]") {
    auto path = createTestWav(48000, 1, 10000, 220.0);

    auto src = std::make_shared<Source>();
    REQUIRE(src->loadFromFile(path));

    auto clip1 = std::make_shared<Clip>(src, 0, 2000);
    auto clip2 = std::make_shared<Clip>(src, 3000, 2000);

    AudioFileSource afs1(clip1);
    AudioFileSource afs2(clip2);

    afs1.prepare(48000, 256);
    afs2.prepare(48000, 256);

    daw::audio::AudioBuffer buf(1, 256);
    auto v1 = buf.view();
    daw::audio::AudioBufferView out1[] = {v1};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = out1;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 256;

    afs1.process(ctx);
    float sum1 = 0.0f;
    for (int i = 0; i < 256; ++i)
        sum1 += std::abs(v1.channel(0)[i]);

    afs2.process(ctx);
    float sum2 = 0.0f;
    for (int i = 0; i < 256; ++i)
        sum2 += std::abs(v1.channel(0)[i]);

    CHECK(sum1 > 0.0f);
    CHECK(sum2 > 0.0f);
}
