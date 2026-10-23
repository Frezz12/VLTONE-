// Cross-format regressions for host/plugin contracts found in the 2026-09 review.
#include "Host/PluginNode.hpp"
#include "Host/PluginAudioBuffers.hpp"
#include "Host/PluginMidi.hpp"
#include "Graph/AudioGraph.hpp"
#include "Graph/GraphProcessor.hpp"
#include "Nodes/BasicNodes.hpp"
#if DAW_ENABLE_VST3
#include "Vst3/Vst3Support.hpp"
#include "Vst3/Vst3Module.hpp"
#endif
#if defined(_WIN32)
#include <objbase.h>
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
struct EventCapture final : EventSink {
    std::array<PluginEvent, 256> events{};
    std::size_t count = 0;
    void push(const PluginEvent& event) noexcept override {
        if (count < events.size()) events[count++] = event;
    }
};
void verifyEventOnly(Format format, const char* path, const char* setting) {
    Environment enabled(setting);
    auto instance = create(descriptor(format, path));
    require(instance->busLayout().outputs.empty(), "event-only plugin declares no audio outputs");
    require(instance->activate({48000, 128, false}), "activate event-only plugin");
    instance->startProcessing();
    PluginEvent event; event.kind = PluginEvent::Kind::ParamValue; event.value = .75; event.frameOffset = 17;
    EventCapture sink;
    PluginProcessContext context; context.frames = 128; context.inputEvents = std::span(&event, 1);
    context.outputEvents = &sink;
    require(instance->process(context) != PluginProcessDisposition::Error && sink.count == 1 &&
            sink.events[0].frameOffset == 17 && std::abs(sink.events[0].value - .75) < 1e-6,
            "event-only processor must run and return events without audio buses");
    context.frames = 129;
    require(instance->process(context) == PluginProcessDisposition::Error,
            "oversized blocks cannot masquerade as successfully rendered silence");
}
void verifySteadyClock(Format format, const char* path, const char* setting) {
    Environment validate(setting);
    PluginNode node("clock", create(descriptor(format, path)));
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
        require(bool(node.offlineStatus()), "steady time survives stop, seek and negative PDC time");
    }
    node.setBypassed(true);
    for (int i = 0; i < 4; ++i) node.process(context);
    node.setBypassed(false);
    node.process(context); node.process(context);
    require(bool(node.offlineStatus()), "steady time advances through sleeping bypass");
    node.reset(); node.process(context);
    require(bool(node.offlineStatus()), "reset cannot move steady time backwards");
}
}
int main() try {
    {
        Block block;
        PluginAudioBus bus;
        bus.allocate(3, 128);
        require(bus.bindInput(block.context, 0, 128) == 4 && bus.pointers[0] == block.left.data(),
                "main channels bind directly and missing channels are silent");
        bus.pointers[2][0] = 99;
        require(bus.bindInput(block.context, 1, 128) == 7 && bus.pointers[2][0] == 0,
                "silent scratch is reset each block");
        const float* sparse[] = {nullptr, block.right.data()};
        block.context.inputs = sparse;
        require(bus.bindInput(block.context, 0, 128) == 5, "null channels use silent scratch");
        bus.bindOutput(block.context, 0, 128);
        require(bus.pointers[0] == block.outLeft.data() && bus.pointers[2] != block.outRight.data(),
                "only supplied main outputs use host storage");
        bus.bindOutput(block.context, 1, 128);
        require(bus.pointers[0] != block.outLeft.data(), "aux outputs have independent scratch");
        std::cout << "PASS shared audio bus mapping and silence\n";
    }
    {
        for (auto kind : {PluginEvent::Kind::NoteOn, PluginEvent::Kind::NoteOff,
                          PluginEvent::Kind::NoteChoke, PluginEvent::Kind::PolyPressure,
                          PluginEvent::Kind::MidiController}) {
            for (unsigned controller : {0u, 64u, 127u, 128u, 129u, 130u}) {
                PluginEvent input; input.kind = kind; input.channel = 15; input.key = 127;
                input.paramIndex = controller; input.value = 0.75;
                std::array<std::uint8_t, 3> bytes;
                PluginEvent output;
                require(encodePluginMidi(input, bytes) && decodePluginMidi(bytes.data(), output), "MIDI 1 round trip");
                require(output.channel == 15 && output.kind ==
                    (kind == PluginEvent::Kind::NoteChoke ? PluginEvent::Kind::NoteOff : kind), "MIDI kind and channel");
                require(kind == PluginEvent::Kind::MidiController ? output.paramIndex == controller : output.key == 127,
                        "MIDI controller/note identity");
                require(std::abs(output.value - (kind == PluginEvent::Kind::NoteChoke ? 0 : input.value)) < 1.0 / 127,
                        "MIDI quantization and choke velocity");
            }
        }
        PluginEvent invalid; invalid.kind = PluginEvent::Kind::NoteOn;
        std::array<std::uint8_t, 3> bytes;
        invalid.value = std::numeric_limits<double>::quiet_NaN();
        require(!encodePluginMidi(invalid, bytes), "nonfinite MIDI rejected before rounding");
        invalid.value = 1; invalid.channel = -1;
        require(!encodePluginMidi(invalid, bytes), "wildcard is not a MIDI channel");
        std::cout << "PASS shared MIDI conversion for VST, CLAP and AU\n";
    }
#if DAW_ENABLE_CLAP
    const auto clap = descriptor(Format::Clap, DAW_TEST_CLAP_PATH);
    verifyEventOnly(Format::Clap, DAW_TEST_CLAP_PATH, "DAW_TEST_CLAP_EVENT_ONLY");
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
    verifySteadyClock(Format::Clap, DAW_TEST_CLAP_PATH, "DAW_TEST_CLAP_VALIDATE_STEADY_TIME");
#endif
#if DAW_ENABLE_VST3
    // The fixture checks ExitDll at DLL detach, after main has returned.
    Environment moduleShutdown("DAW_TEST_VST3_MODULE_EXIT");
    verifyEventOnly(Format::Vst3, DAW_TEST_VST3_PATH, "DAW_TEST_VST3_EVENT_ONLY");
    verifySteadyClock(Format::Vst3, DAW_TEST_VST3_PATH, "DAW_TEST_VST3_VALIDATE_STEADY_TIME");
    {
        const auto d = descriptor(Format::Vst3, DAW_TEST_VST3_PATH);
        Environment notices("DAW_TEST_VST3_ACTIVATION_RESTART_FLAGS");
        const auto latencyFlag = std::to_string(int(Steinberg::Vst::kLatencyChanged));
        environment(notices.name, latencyFlag.c_str());
        PluginNode node("activation latency", create(d));
        for (int cycle = 0; cycle < 4; ++cycle) {
            node.prepare({48000, 128, 2});
            require(node.isReady() && node.latencySamples() == 64, "activation reads reported latency");
            require(!node.takeLatencyChanged() && !node.takeRestartRequested(),
                    "activation latency must not trigger another activation (Gullfoss)");
        }
        {
            Environment values("DAW_TEST_VST3_RESTART_FLAGS");
            const auto flag = std::to_string(int(Steinberg::Vst::kParamValuesChanged));
            environment(values.name, flag.c_str());
            node.instance()->setParameterFromHost(1, .25);
            node.instance()->pumpMainThread();
            PluginEvent event; bool refreshed = false;
            while (node.popNotification(event))
                refreshed |= event.paramIndex == 1 && std::abs(event.value - .25) < 1e-6;
            require(refreshed, "acknowledged activation latency cannot block later value notifications");
        }
        const auto combined = std::to_string(int(Steinberg::Vst::kLatencyChanged |
                                                Steinberg::Vst::kParamTitlesChanged));
        {
            Environment asynchronous("DAW_TEST_VST3_RESTART_FLAGS");
            environment(asynchronous.name, latencyFlag.c_str());
            // A delayed notification after activation is common too. The
            // graph must remain live when the reported value did not move.
            for (int repeat = 0; repeat < 8; ++repeat) {
                node.instance()->setParameterFromHost(1, .25);
                node.instance()->pumpMainThread();
                require(!node.takeLatencyChanged() && !node.takeRestartRequested(),
                        "unchanged asynchronous latency must not reconfigure the graph (Rev LX-24)");
            }
            Environment latency("DAW_TEST_VST3_LATENCY");
            environment(latency.name, "128");
            node.instance()->setParameterFromHost(1, .25);
            node.instance()->pumpMainThread();
            require(node.takeLatencyChanged() && node.instance()->latencySamples() == 128,
                    "a real latency change still reaches graph compensation");
            node.prepare({48000, 128, 2});
            require(node.latencySamples() == 128, "real latency is applied during graph preparation");
        }
        environment(notices.name, combined.c_str());
        node.prepare({48000, 128, 2});
        require(node.takeRestartRequested(), "activation cannot swallow unrelated metadata changes");
        std::cout << "PASS VST3 activation latency does not loop\n";
    }
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
            auto sibling = create(d);
            require(bool(sibling), "VST3 instances share the module-retained host application");
            std::vector<std::uint8_t> state;
            require(instance->saveState(state) && !state.empty(), "VST3 state fixture");
            state.pop_back();
            require(!instance->loadState(state), "truncated controller state must fail");
            instance.reset(); // Plugin destructor queries the still-live host.
            sibling.reset();
            require(bool(create(d)), "module-retained host survives destruction of all instances");
        }
#if defined(_WIN32)
        ULONG_PTR apartmentToken = 0;
        require(SUCCEEDED(CoGetContextToken(&apartmentToken)), "VST3 host owns a COM apartment");
        const HRESULT anotherPlugin = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        require(SUCCEEDED(anotherPlugin), "another plugin can initialize COM");
        CoUninitialize();
        require(SUCCEEDED(CoGetContextToken(&apartmentToken)),
                "another plugin's teardown cannot end the host COM apartment");
#endif
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
        auto instance = create(descriptor(Format::Vst, DAW_TEST_VST_SHELL_PATH));
        require(instance->activate({48000, 128, false}), "activate dense VST2 automation");
        instance->startProcessing();
        Block block;
        std::vector<PluginEvent> events;
        for (unsigned frame = 0; frame < 128; ++frame) {
            PluginEvent parameter; parameter.frameOffset = frame; parameter.value = double(frame) / 127;
            events.push_back(parameter);
            if (frame == 0 || frame == 63 || frame == 127) {
                PluginEvent midi; midi.kind = PluginEvent::Kind::MidiController;
                midi.paramIndex = 64; midi.frameOffset = frame; midi.value = 1;
                events.push_back(midi);
            }
        }
        EventCapture sink;
        block.context.inputEvents = events; block.context.outputEvents = &sink;
        for (int repeat = 0; repeat < 3; ++repeat) {
            sink.count = 0;
            require(instance->process(block.context) != PluginProcessDisposition::Error, "process dense VST2 block");
            for (unsigned frame = 0; frame < 128; ++frame)
                require(std::abs(block.outLeft[frame] - float(frame) / 127) < 1e-6, "VST2 automation is sample accurate");
            require(sink.count == 3 && sink.events[0].frameOffset == 0 && sink.events[1].frameOffset == 63 &&
                    sink.events[2].frameOffset == 127, "MIDI is delivered once at each automation segment boundary");
        }
        std::cout << "PASS VST2 dense automation with interleaved MIDI\n";
    }
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
#if DAW_ENABLE_VST3
    {
        const auto d = descriptor(Format::Vst3, DAW_TEST_VST3_PATH);
        auto live = create(d);
        Vst3Module::releaseUnusedModules();
        std::vector<std::uint8_t> state;
        require(live->saveState(state), "module cleanup retains live processors");
        live.reset();
        Vst3Module::releaseUnusedModules();
        require(bool(create(d)), "module may reopen after controlled shutdown");
        Vst3Module::releaseUnusedModules();
        std::cout << "PASS VST3 controlled module shutdown\n";
    }
#endif
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
