// Cross-format regressions for host/plugin contracts found in the 2026-09 review.
#include "Host/PluginNode.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/BasicNodes.hpp"
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Support.hpp"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace daw;
using namespace daw::plugins;
namespace {
void require(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
void environment(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
}
struct Environment {
    const char* name;
    explicit Environment(const char* n) : name(n) { environment(name, "1"); }
    ~Environment() { environment(name, ""); }
};
PluginDescriptor descriptor(Format format, const char* path) {
    const auto all = factoryFor(format)->inspect(path);
    require(!all.empty(), "inspect fixture");
    const auto found = std::find_if(all.begin(), all.end(), [](const auto& p) { return !p.isInstrument; });
    require(found != all.end(), "effect fixture");
    return *found;
}
std::unique_ptr<PluginInstance> create(const PluginDescriptor& d) {
    auto p = factoryFor(d.format)->create(d);
    require(bool(p), "create fixture"); return p;
}
struct Block {
    std::array<float, 128> left{}, right{}, outLeft{}, outRight{};
    const float* inputs[2]{left.data(), right.data()};
    float* outputs[2]{outLeft.data(), outRight.data()};
    PluginProcessContext context;
    Block() {
        left.fill(1); right.fill(1); outLeft.fill(-99); outRight.fill(-99);
        context.inputs = inputs; context.inputChannels = 2;
        context.outputs = outputs; context.outputChannels = 2; context.frames = 128;
    }
};
}
int main() try {
#if DAW_ENABLE_CLAP
    const auto clap = descriptor(Format::Clap, DAW_TEST_CLAP_PATH);
    {
        Environment request("DAW_TEST_CLAP_INIT_CALLBACK");
        auto instance = create(clap);
        instance->pumpMainThread();
        std::vector<std::uint8_t> state;
        require(instance->saveState(state) && state.size() >= 4 * sizeof(double), "CLAP diagnostics state");
        double calls = 0;
        std::memcpy(&calls, state.data() + 3 * sizeof(double), sizeof(double));
        require(calls == 1, "CLAP init request_callback must survive construction");
        std::cout << "PASS CLAP initialization callbacks\n";
    }
    {
        auto instance = create(clap);
        Environment malformed("DAW_TEST_CLAP_BAD_STREAM");
        std::vector<std::uint8_t> state;
        require(!instance->saveState(state) && state.empty(), "CLAP invalid state write rejected");
        std::cout << "PASS CLAP state stream validation\n";
    }
    {
        Environment validate("DAW_TEST_CLAP_VALIDATE_STEADY_TIME");
        PluginNode node("CLAP clock", create(clap));
        node.prepare({48000, 128, 2});
        Block block;
        float* inputChannels[]{block.left.data(), block.right.data()};
        std::array<engine::AudioBlock, 1> inputs{engine::AudioBlock(inputChannels, 2, 128)};
        engine::ProcessContext context;
        context.output = engine::AudioBlock(block.outputs, 2, 128);
        context.inputs = inputs; context.frames = 128; context.sampleRate = 48000;
        for (std::int64_t position : {0, 128, 128, 16, -512, 0}) {
            context.timelinePosition = position;
            context.playing = position != 128;
            node.process(context);
            require(bool(node.offlineStatus()), "CLAP steady time survives stop, seek and negative PDC time");
        }
        node.setBypassed(true);
        for (int i = 0; i < 4; ++i) node.process(context);
        node.setBypassed(false);
        node.process(context); node.process(context);
        require(bool(node.offlineStatus()), "CLAP steady time advances through sleeping bypass");
        node.reset(); node.process(context);
        require(bool(node.offlineStatus()), "CLAP reset cannot move steady time backwards");
        std::cout << "PASS CLAP monotonic sample clock independent of transport\n";
    }
#endif
#if DAW_ENABLE_VST3
    // The fixture checks ExitDll at DLL detach, after main has returned.
    Environment moduleShutdown("DAW_TEST_VST3_MODULE_EXIT");
    {
        const auto d = descriptor(Format::Vst3, DAW_TEST_VST3_PATH);
        {
            Environment baseOnly("DAW_TEST_VST3_COMPONENT_VIA_BASE");
            require(bool(factoryFor(Format::Vst3)->create(d)),
                    "a factory exposing IComponent through IPluginBase initializes");
        }
        {
            Environment failure("DAW_TEST_VST3_FAIL_INITIALIZE");
            require(!factoryFor(Format::Vst3)->create(d), "failed initialization is not terminated twice");
        }
        {
            Environment release("DAW_TEST_VST3_RELEASE_HOST");
            auto instance = create(d);
            std::vector<std::uint8_t> state;
            require(instance->saveState(state) && !state.empty(), "VST3 state fixture");
            state.pop_back();
            require(!instance->loadState(state), "truncated controller state must fail");
            instance.reset(); // Plugin destructor queries the still-live host.
        }
        std::cout << "PASS VST3 initialization failure and host teardown lifetime\n";
    }
    {
        using namespace Steinberg;
        vst3::MemoryStream stream;
        char byte = 'x'; int32 count = -1;
        require(stream.write(&byte, 1, &count) == kResultOk && count == 1, "state write");
        require(stream.seek(std::numeric_limits<int64>::max(), IBStream::kIBSeekCur, nullptr) != kResultOk,
                "state seek cannot overflow");
        int64 position = 0; stream.tell(&position);
        require(position == 1, "failed seek preserves position");
        require(stream.seek(kMaxPluginStateBytes, IBStream::kIBSeekSet, nullptr) == kResultOk, "bounded sparse seek");
        require(stream.write(nullptr, 0, &count) == kResultOk && stream.data().size() == 1,
                "zero-byte write cannot allocate a sparse state");
        require(stream.write(&byte, 1, &count) != kResultOk && count == 0 && stream.data().size() == 1,
                "state size limit checked before allocation");
        require(stream.read(nullptr, 1, &count) != kResultOk && count == 0, "null read rejected");
        std::cout << "PASS VST3 state bounds and seek overflow\n";
    }

    {
        vst3::ParameterChanges changes;
        changes.reserve(2,8194);
        for (int block=0;block<3;++block) {
            changes.clear(); auto* dense=changes.begin(42); auto* sparse=changes.begin(7);
            for (int i=0;i<8192;++i) dense->expectPoint();
            require(changes.preparePoints(),"prepare shared VST3 point pool");
            require(sparse->add(0,.5),"sparse queue beside dense curve");
            for(int i=0;i<8192;++i) require(dense->add(i,i/8191.0),"all dense VST3 points retained");
            Steinberg::int32 offset=0; double value=0;
            require(dense->getPointCount()==8192 &&
                dense->getPoint(8191,offset,value)==Steinberg::kResultOk && offset==8191 && value==1,
                "dense VST3 automation reaches its final point");
        }
        std::cout << "PASS VST3 dense shared parameter pool\n";
    }
    {
        vst3::ParameterChanges changes;
        changes.reserve(4);
        for (int block = 0; block < 4; ++block) {
            changes.clear();
            for (Steinberg::uint32 i = 0; i < 4; ++i) {
                Steinberg::int32 index = -1;
                auto* queue = changes.addParameterData(i * 65536 + block, index);
                require(queue && index == i && queue->getPointCount() == 0, "stable queue order");
                Steinberg::int32 point = -1;
                require(queue->addPoint(3, 0.25, point) == Steinberg::kResultOk, "queue point");
                require(changes.addParameterData(i * 65536 + block, index) == queue && index == i,
                        "repeated parameter keeps its queue");
            }
            require(!changes.begin(12345), "exhausted pool does not allocate");
        }
        changes.reserve(128); changes.clear();
        require(changes.begin(std::numeric_limits<Steinberg::uint32>::max()), "all parameter IDs supported");
        std::cout << "PASS VST3 parameter queue identity, collisions, capacity\n";
    }
    {
        const auto d = descriptor(Format::Vst3, DAW_TEST_VST3_PATH);
        auto node = std::make_shared<PluginNode>("sidechain-contract", create(d));
        engine::AudioGraph graph;
        const auto source = graph.addNode(std::make_unique<engine::SourceNode>("silent key",
            [](void*, const engine::AudioBlock& out, engine::FrameCount frames, engine::SamplePos) {
                for (unsigned ch = 0; ch < out.numChannels(); ++ch) std::fill_n(out.data(ch), frames, 0.f);
            }, nullptr));
        const auto effect = graph.adoptNode(node);
        require(bool(graph.connect(source, effect)), "connect main input"); graph.setSink(effect);
        engine::GraphProcessor processor(2);
        Block block;
        engine::PrepareInfo info{48000, 128, 2, false};
        const auto verify = [&](float expected) {
            auto compiled = graph.compile(info);
            require(bool(compiled), "compile sidechain graph");
            processor.setGraph(*compiled);
            PluginEvent diagnostic; diagnostic.kind = PluginEvent::Kind::ParamValue;
            diagnostic.paramIndex = 0; diagnostic.value = 0.03125;
            require(node->pushEvent(diagnostic), "queue connectivity probe");
            require(bool(processor.process(engine::AudioBlock(block.outputs, 2, 128), 128, 0, true)), "process graph");
            require(std::abs(block.outLeft.back() - expected) < 1e-6f, "VST3 aux activation matches graph edge");
        };
        verify(0.25f); // A declared but disconnected aux bus is inactive.
        require(bool(graph.connect(source, effect, engine::InputRole::Sidechain)), "connect sidechain");
        verify(0.75f); // A connected silent source is still connected.
        info.offline = true; verify(0.75f);
        require(bool(graph.disconnect(source, effect)), "disconnect inputs");
        require(bool(graph.connect(source, effect)), "reconnect main input");
        verify(0.25f);
        std::cout << "PASS VST3 sidechain connect/disconnect and offline activation\n";
    }
#endif
#if DAW_ENABLE_VST

    {
        const auto all=factoryFor(Format::Vst)->inspect(DAW_TEST_VST_SHELL_PATH);
        const auto synth=std::find_if(all.begin(),all.end(),[](const auto& d){return d.isInstrument;});
        require(synth!=all.end(),"VST2 reset fixture instrument");
        auto instance=create(*synth);
        require(instance->activate({48000,128,false}),"activate reset fixture");
        instance->startProcessing(); Block block;
        PluginEvent note; note.kind=PluginEvent::Kind::NoteOn; note.key=60; note.value=1;
        block.context.inputEvents=std::span(&note,1);
        instance->process(block.context);
        require(block.outLeft.back()!=0,"legacy voice sounding before reset");
        instance->resetForTransport(); block.context.inputEvents={};
        instance->process(block.context);
        require(instance->isProcessing() && std::all_of(block.outLeft.begin(),block.outLeft.end(),[](float x){return x==0;}),
                "control-thread VST2 reset clears voices and resumes processing");
        require(!instance->supportsRealtimeReset(),"legacy bypass must keep DSP current");
        std::cout<<"PASS VST2 transport reset\n";
    }
    {
        auto instance = create(descriptor(Format::Vst, DAW_TEST_VST_SHELL_PATH));
        require(instance->activate({48000, 128, false}), "activate VST2"); instance->startProcessing();
        Block block;
        PluginEvent edit; edit.kind = PluginEvent::Kind::ParamValue;
        edit.paramIndex = 0; edit.frameOffset = 16; edit.value = 0.25;
        block.context.inputEvents = std::span(&edit, 1);
        {
            Environment changed("DAW_TEST_VST_CHANGE_IO");
            require(instance->process(block.context) == PluginProcessDisposition::Error,
                    "VST2 mid-block IO change cannot use old pointer arrays");
        }
        require(std::all_of(block.outLeft.begin(), block.outLeft.end(), [](float x) { return x == 0; }),
                "VST2 layout failure clears the entire output");
        instance->stopProcessing(); instance->deactivate();
        require(instance->activate({48000, 128, false}), "reactivate VST2 changed layout");
        instance->startProcessing(); block.context.inputEvents = {};
        require(instance->process(block.context) != PluginProcessDisposition::Error && block.outLeft.back() == 0.25f,
                "VST2 resumes after safe reallocation");
        std::cout << "PASS VST2 dynamic IO bounds and recovery\n";
    }
#endif
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
