#include "AudioRuntime.hpp"
#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "ProjectSerializer.hpp"
#include "serialization/InsertJson.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>

using namespace daw;
namespace {
int failures=0;
void check(bool ok,const char* text) { std::printf("%s %s\n",ok?"PASS":"FAIL",text); failures+=!ok; }
struct Probe final : engine::Node {
    int calls=0; float heard=0;
    std::string_view name() const noexcept override { return "Destructive analyzer with latency"; }
    engine::FrameCount latencySamples() const noexcept override { return 8192; }
    engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
    bool supportsInPlace() const noexcept override { return true; }
    void process(const engine::ProcessContext& c) noexcept override {
        ++calls; heard=c.inputs.front().data(0)[0];
        for(unsigned ch=0;ch<c.output.numChannels();++ch) std::fill_n(c.output.data(ch),c.frames,-999.f);
    }
};
struct Tone final : engine::Node {
    std::string_view name() const noexcept override { return "Source"; }
    engine::MidiNodeRole midiRole() const noexcept override { return engine::MidiNodeRole::None; }
    void process(const engine::ProcessContext& c) noexcept override {
        for(unsigned ch=0;ch<c.output.numChannels();++ch) std::fill_n(c.output.data(ch),c.frames,.125f);
    }
};
void graphIsolation() {
    engine::RealtimeEngine engine(2);
    engine.prepare(48000,256);
    auto& graph=engine.graph();
    auto source=graph.addNode(std::make_unique<Tone>());
    auto observer=std::make_shared<Probe>(); auto id=graph.adoptNode(observer);
    graph.connect(source,id); graph.setSink(source);
    check(bool(engine.commitGraph()),"terminal analyzer compiles");
    check(engine.sessionGraph()->totalLatency==0,"8192-frame analyzer adds zero output latency");
    std::array<float,256> l{},r{}; float* pointers[]={l.data(),r.data()};
    for(int i=0;i<4;++i) engine.renderBlock({pointers,2,256},nullptr,0,256);
    check(observer->calls==4 && observer->heard==.125f,"terminal analyzer runs and hears final samples");
    check(std::all_of(l.begin(),l.end(),[](float value){return value==.125f;}) && l==r,
          "destructive in-place analyzer cannot overwrite output buffer");
}
void runtimeIsolation(PluginChannelMode mode) {
    AudioRuntime runtime; check(bool(runtime.prepare(48000,256)),"runtime prepares");
    AudioSessionSpec session;
    AudioGraphSpec::Channel input; input.id="input"; input.name="Input"; input.input={true,true,0,2,3};
    session.graph.channels.push_back(input); session.graph.masterVolume=.5f;
    AudioPluginChainSpec chain; chain.channelId="master";
    AudioPluginSpec analyzer; analyzer.id="meter"; analyzer.uid="daw.equalizer"; analyzer.name="Analyzer";
    analyzer.descriptor=plugins::equalizer::EqualizerInstance::staticDescriptor(); analyzer.requiredFormat=plugins::Format::Internal;
    analyzer.channelMode=mode; analyzer.preferredChannels=mode==PluginChannelMode::Stereo?2:1;
    analyzer.parameters={{"output.gain",12}}; analyzer.rightParameters=analyzer.parameters; analyzer.analysisOnly=true;
    chain.slots.push_back(analyzer); session.pluginChains.push_back(chain);
    check(bool(runtime.applySession(session)),"analysis plugin session installs");
    audio::AudioBuffer in(2,256),out(2,256); std::fill_n(in.getChannel(0),256,.25f); std::fill_n(in.getChannel(1),256,.5f);
    audio::AudioCallbackContext context; context.inputBuffer=&in; context.outputBuffer=&out; context.numFrames=256; context.sampleRate=48000;
    for(int i=0;i<20;++i) runtime.processDeviceBlock(context);
    const auto equal=[&] { for(unsigned i=0;i<256;++i) if(out.getChannel(0)[i]!=.125f || out.getChannel(1)[i]!=.25f) return false; return true; };
    check(equal(),"analyzer +12 dB leaves master output bit-identical");
    const auto graph=runtime.routingGraph();
    bool terminal=false;
    bool processed=false;
    for(const auto& node:graph->nodes) if(node.node->name()=="Analyzer") {
        terminal=true;
        processed |= std::abs(graph->arena.block(node.outputBuffer,256).data(0)[255])>.4f;
        const auto producer=graph->inputEdges[node.firstInput].producer;
        if(mode==PluginChannelMode::Stereo) check(producer==graph->sinkNode,"analyzer taps post-fader master");
    }
    check(terminal,"analysis DSP is present, not bypassed or omitted");
    check(processed,"analyzer actually processes and amplifies its private audio buffer");
    // Inject the same fault-group state the plugin's recovery mechanism uses.
    for(const auto& node:graph->nodes) if(auto* plugin=dynamic_cast<plugins::PluginNode*>(node.node))
        plugin->faultGroup()->sides.store(1);
    runtime.commitGraph(); runtime.processDeviceBlock(context);
    check(equal(),"faulted analyzer does not engage master safety mute");
}
void documentLifecycle() {
    EngineController controller; controller.initialize(48000,256,false);
    const auto& eq=plugins::equalizer::EqualizerInstance::staticDescriptor();
    const auto slot=controller.addInsert("master",eq,size_t(-1),true);
    check(!slot.empty() && controller.project().masterInserts.back().analysisOnly,"add creates analysis role atomically");
    controller.undo(); check(controller.project().masterInserts.empty(),"undo removes analyzer");
    controller.redo(); check(controller.project().masterInserts.size()==1 && controller.project().masterInserts[0].analysisOnly,"redo restores isolated role");
    const auto restored=serialization::insertFromJson(serialization::insertToJson(controller.project().masterInserts[0]));
    check(restored.analysisOnly,"project/preset JSON preserves analysis role");
    check(!serialization::insertFromJson(nlohmann::json{{"id","old"}}).analysisOnly,"older slots remain normal effects");
    controller.replaceInsert("master",slot,eq);
    check(controller.project().masterInserts[0].analysisOnly,"replacement preserves isolated role");
}
void exportIsolation() {
    namespace fs=std::filesystem;
    const auto directory=fs::temp_directory_path()/("vlt-master-analysis-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto source=(directory/"source.wav").string(), before=(directory/"before.wav").string(), after=(directory/"after.wav").string();
    audio::platform::AudioFileWriter writer; audio::platform::WriteSpec spec; spec.encoding=audio::platform::Encoding::Float32;
    check(bool(writer.open(source,spec,48000,2,24000)),"create export fixture");
    std::vector<float> samples(24000);
    for(std::size_t i=0;i<samples.size();++i) samples[i]=.125f*std::sin(i*2*3.141592653589793*440/48000);
    const float* channels[]={samples.data(),samples.data()}; writer.write(channels,samples.size()); writer.close();
    EngineController controller; controller.initialize(48000,256,false);
    const auto track=controller.addTrack(TrackKind::Audio,"Export source"); controller.importAudio(source,track,0);
    check(bool(controller.exportMixdown(before,false)),"baseline export succeeds");
    const auto slot=controller.addInsert("master",plugins::equalizer::EqualizerInstance::staticDescriptor(),size_t(-1),true);
    controller.setInsertParameter("master",slot,"output.gain",12);
    const auto exported=controller.exportMixdown(after,false);
    if(!exported) std::printf("Export error: %s\n",exported.message().c_str());
    check(bool(exported),"export succeeds with an active analysis plugin");
    audio::platform::DecodedAudio a,b;
    const bool decoded=bool(audio::platform::decodeAudioFile(before,a)) && bool(audio::platform::decodeAudioFile(after,b));
    check(decoded && !a.interleaved.empty() && a.frames==b.frames && a.interleaved==b.interleaved,
          "export with analysis plugin is sample-for-sample identical");
    std::error_code error; fs::remove_all(directory,error);
}
}
int main() {
    graphIsolation(); runtimeIsolation(PluginChannelMode::Stereo); runtimeIsolation(PluginChannelMode::DualMono); documentLifecycle(); exportIsolation();
    return failures?1:0;
}
