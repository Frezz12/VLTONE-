#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

#include "daw/graph/GainNode.h"
#include "daw/graph/GraphCompiler.h"
#include "daw/graph/NullSource.h"
#include "daw/graph/ProcessGraph.h"
#include "daw/graph/SummingBus.h"

using namespace daw::graph;
using Catch::Approx;

namespace {

class ConstSource : public ProcessNode {
public:
    explicit ConstSource(float value) : value_(value) {}

    void process(ProcessContext& ctx) noexcept override
    {
        for (int b = 0; b < ctx.numOutputs; ++b) {
            auto& out = ctx.outputs[b];
            for (int c = 0; c < out.numChannels(); ++c) {
                float* dst = out.channel(c);
                for (int i = 0; i < ctx.numFrames; ++i)
                    dst[i] = value_;
            }
        }
    }

    int numInputBuses() const noexcept override { return 0; }
    int numOutputBuses() const noexcept override { return 1; }
    int numChannelsForOutputBus(int) const noexcept override { return 2; }

private:
    float value_ = 0.0f;
};

class CaptureSink : public ProcessNode {
public:
    explicit CaptureSink(float* outFrame)
        : outFrame_(outFrame) {}

    void process(ProcessContext& ctx) noexcept override
    {
        if (ctx.numInputs < 1 || !outFrame_)
            return;
        auto& in = ctx.inputs[0];
        if (in.numChannels() > 0 && ctx.numFrames > 0)
            *outFrame_ = in.channel(0)[ctx.numFrames - 1];
    }

    int numInputBuses() const noexcept override { return 1; }
    int numOutputBuses() const noexcept override { return 0; }
    int numChannelsForInputBus(int) const noexcept override { return 2; }

private:
    float* outFrame_ = nullptr;
};

} // namespace

TEST_CASE("ProcessGraph: empty graph compiles and processes safely", "[graph]") {
    GraphCompiler compiler;
    auto graph = compiler.compile(48000.0, 256);
    REQUIRE(graph != nullptr);
    REQUIRE(graph->empty());

    daw::audio::AudioBuffer outBuf(2, 256);
    auto outView = outBuf.view();
    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 256;
    graph->process(ctx);
}

TEST_CASE("ProcessGraph: single source node produces output", "[graph]") {
    GraphCompiler compiler;
    compiler.addNode(std::make_unique<ConstSource>(0.5f));

    auto graph = compiler.compile(48000.0, 64);
    REQUIRE(graph != nullptr);
    REQUIRE(graph->numSteps() == 1);

    daw::audio::AudioBuffer outBuf(2, 64);
    auto outView = outBuf.view();
    outView.clear();

    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 64;
    graph->process(ctx);

    REQUIRE(outView.channel(0)[0] == Approx(0.5f));
    REQUIRE(outView.channel(1)[0] == Approx(0.5f));
    REQUIRE(outView.channel(0)[63] == Approx(0.5f));
}

TEST_CASE("ProcessGraph: source -> gain chain", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(1.0f));
    auto gainNode = std::make_unique<GainNode>();
    gainNode->setGain(0.5f);
    compiler.addNode(std::move(gainNode));
    compiler.addConnection(0, 1);

    float lastFrame = 0.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&lastFrame));
    compiler.addConnection(1, 2);

    auto graph = compiler.compile(48000.0, 128);

    daw::audio::AudioBuffer outBuf(2, 128);
    auto outView = outBuf.view();
    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 128;
    ctx.playing = true;

    graph->process(ctx);

    REQUIRE(lastFrame == Approx(0.5f).epsilon(0.01f));
}

TEST_CASE("ProcessGraph: summing bus adds multiple sources", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(0.2f));
    compiler.addNode(std::make_unique<ConstSource>(0.3f));
    compiler.addNode(std::make_unique<SummingBus>());

    compiler.addConnection(0, 2);
    compiler.addConnection(1, 2);

    auto graph = compiler.compile(48000.0, 64);

    REQUIRE(graph->numSteps() >= 3);
}

TEST_CASE("ProcessGraph: gain node attenuates signal with smoothing", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(1.0f));
    auto gainNode = std::make_unique<GainNode>();
    gainNode->setGain(0.5f);
    compiler.addNode(std::move(gainNode));
    compiler.addConnection(0, 1);

    float lastFrame = 0.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&lastFrame));
    compiler.addConnection(1, 2);

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

    graph->process(ctx);

    REQUIRE(lastFrame == Approx(0.5f).epsilon(0.01f));
}

TEST_CASE("ProcessGraph: compiler handles disconnected graphs", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<NullSource>());
    compiler.addNode(std::make_unique<NullSource>());

    auto graph = compiler.compile(48000.0, 64);

    REQUIRE(graph != nullptr);
    REQUIRE(graph->numSteps() == 2);
}

TEST_CASE("ProcessGraph: multiple compile calls produce independent graphs", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(1.0f));
    float frame1 = 0.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&frame1));
    compiler.addConnection(0, 1);

    auto graph1 = compiler.compile(48000.0, 128);

    compiler.clear();
    compiler.addNode(std::make_unique<ConstSource>(0.5f));
    float frame2 = 0.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&frame2));
    compiler.addConnection(0, 1);

    auto graph2 = compiler.compile(48000.0, 128);

    REQUIRE(graph1->numSteps() == 2);
    REQUIRE(graph2->numSteps() == 2);
}

TEST_CASE("ProcessGraph: graph processes blocks correctly with varying size", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(0.75f));
    auto gainNode = std::make_unique<GainNode>();
    gainNode->setGain(1.0f);
    compiler.addNode(std::move(gainNode));
    compiler.addConnection(0, 1);

    auto graph = compiler.compile(48000.0, 1024);

    for (int blockSize : {64, 128, 256, 512}) {
        daw::audio::AudioBuffer outBuf(2, blockSize);
        auto outView = outBuf.view();
        daw::audio::AudioBufferView outArr[] = {outView};

        ProcessContext ctx;
        ctx.inputs = nullptr;
        ctx.outputs = outArr;
        ctx.numInputs = 0;
        ctx.numOutputs = 1;
        ctx.numFrames = blockSize;

        graph->process(ctx);

        float val = outView.channel(0)[blockSize / 2];
        REQUIRE(val > 0.0f);
    }
}

TEST_CASE("ProcessGraph: null source produces silence", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<NullSource>());
    float frame = 1.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&frame));
    compiler.addConnection(0, 1);

    auto graph = compiler.compile(48000.0, 64);

    daw::audio::AudioBuffer outBuf(2, 64);
    auto outView = outBuf.view();
    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 64;

    graph->process(ctx);

    REQUIRE(outView.channel(0)[0] == Approx(0.0f));
}

TEST_CASE("ProcessGraph: source connects to summing bus with capture", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(0.7f));
    compiler.addNode(std::make_unique<SummingBus>());
    compiler.addConnection(0, 1);

    float last = 0.0f;
    compiler.addNode(std::make_unique<CaptureSink>(&last));
    compiler.addConnection(1, 2);

    auto graph = compiler.compile(48000.0, 128);

    daw::audio::AudioBuffer outBuf(2, 128);
    auto outView = outBuf.view();
    daw::audio::AudioBufferView outArr[] = {outView};

    ProcessContext ctx;
    ctx.inputs = nullptr;
    ctx.outputs = outArr;
    ctx.numInputs = 0;
    ctx.numOutputs = 1;
    ctx.numFrames = 128;

    graph->process(ctx);
}

TEST_CASE("ProcessGraph: construct with 2 sources, gain, summing bus", "[graph]") {
    GraphCompiler compiler;

    compiler.addNode(std::make_unique<ConstSource>(0.1f));
    compiler.addNode(std::make_unique<ConstSource>(0.2f));
    auto gain1 = std::make_unique<GainNode>();
    gain1->setGain(2.0f);
    compiler.addNode(std::move(gain1));
    auto gain2 = std::make_unique<GainNode>();
    gain2->setGain(3.0f);
    compiler.addNode(std::move(gain2));
    compiler.addNode(std::make_unique<SummingBus>());

    compiler.addConnection(0, 2);
    compiler.addConnection(1, 3);
    compiler.addConnection(2, 4);
    compiler.addConnection(3, 4);

    auto graph = compiler.compile(48000.0, 64);
    REQUIRE(graph != nullptr);
    REQUIRE(graph->numSteps() == 5);
}
