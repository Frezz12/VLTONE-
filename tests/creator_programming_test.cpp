#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include "Internal/CreatorPrimitiveKernel.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numeric>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace daw::plugins;
using namespace daw::plugins::mini;
static std::atomic<bool> measuring{false};
static std::atomic<unsigned> allocations{0};
void *operator new(std::size_t n) { if (measuring) ++allocations; if (auto *p = std::malloc(std::max(n, std::size_t(1)))) return p; throw std::bad_alloc(); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
static int failures = 0;
static void check(bool ok, const char *name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
static void param(NodeDefinition &n, const char *id, double v) { for (auto &p : n.parameters) if (p.id == id) { p.value = v; return; } n.parameters.push_back({id, v}); }
static MiniModuleDefinition graph() {
  MiniModuleDefinition d; d.version = 5; d.id = "test.programming"; d.name = "Programming";
  d.nodes = {makeNode("input", "in"), makeNode("output", "out")}; return d;
}
static std::vector<float> render(const MiniModuleDefinition &d, double rate, unsigned block, unsigned channels = 2, bool offline = false) {
  MiniModuleInstance fx; PluginBusLayout accepted;
  if (!fx.configure(d) || !fx.setBusLayout({{std::uint16_t(channels)}, {std::uint16_t(channels)}}, accepted) || !fx.activate({rate, block})) {
    std::printf("PREPARE: %s\n", fx.error().c_str()); ++failures; return {};
  }
  fx.startProcessing(); std::vector<float> result(2048), in(block), right(block), out(block), outR(block);
  const float *inputs[]{in.data(), right.data()}; float *outputs[]{out.data(), outR.data()};
  for (unsigned at = 0; at < result.size(); at += block) {
    std::fill(in.begin(), in.end(), 0); std::fill(right.begin(), right.end(), 0);
    if (!at) { in[0] = 1; right[0] = .25f; }
    PluginProcessContext c; c.inputs = inputs; c.outputs = outputs; c.frames = std::min(block, unsigned(result.size()) - at);
    c.inputChannels = c.outputChannels = std::uint16_t(channels); c.sampleTime = at;
    c.offline = offline;
    measuring = true; auto disposition = fx.process(c); measuring = false;
    if (disposition == PluginProcessDisposition::Error) { std::printf("PROCESS FAILURE\n"); ++failures; break; }
    std::copy_n(out.begin(), c.frames, result.begin() + at);
  }
  return result;
}
static MiniModuleDefinition feedback() {
  auto d = graph(); auto h = makeNode("history", "h"); h.valueType = "audio";
  auto scale = makeNode("audio_scale", "scale"); param(scale, "gain", .5);
  d.nodes.insert(d.nodes.end(), {h, scale, makeNode("audio_add", "sum")});
  d.connections = {{"in","sum","out","a"},{"h","scale","out","in"},{"scale","sum","out","b"},{"sum","h","out","next"},{"sum","out","out","in"}};
  return d;
}
static SubgraphDefinition body(bool map = false, bool reduce = false) {
  SubgraphDefinition g; g.id = "custom"; g.name = "Custom";
  g.inputs = map ? std::vector<GraphPort>{{"item","Item","number"},{"index","Index","integer"}} : std::vector<GraphPort>{{"input","Input","audio"}};
  if (reduce) g.inputs.push_back({"accumulator","Accumulator","number"});
  g.outputs = {{"result","Result",map?"number":"audio"}};
  for (const auto &p : g.inputs) { auto n=makeNode("subgraph_input",p.id);n.port=p.id;n.valueType=p.type;g.nodes.push_back(n); }
  auto out=makeNode("subgraph_output","result");out.port="result";out.valueType=g.outputs[0].type;g.nodes.push_back(out);
  auto op=makeNode(map?(reduce?"add":"multiply"):"audio_scale","op");param(op,map?"b":"gain",2);g.nodes.push_back(op);
  g.connections={{map?"item":"input","op","out",map?"a":"in"},{"op","result","out","in"}};
  if(reduce)g.connections.push_back({"accumulator","op","out","b"});
  return g;
}
static void primitiveTests() {
  const std::pair<const char *,double> expected[]={{"exp",std::exp(.5)},{"log",std::log(.5)},{"log2",-1},{"log10",std::log10(.5)},
    {"tanh",std::tanh(.5)},{"atan",std::atan(.5)},{"sign",1},{"floor",0},{"ceil",1},{"round",1},{"fraction",.5},
    {"linear_to_db",20*std::log10(.5)},{"db_to_linear",std::pow(10.,.025)}};
  bool formulas=true;
  for(auto [type,value]:expected){
    auto d=graph();auto n=makeNode(type,"test");param(n,"value",.5);d.nodes={n};
    CreatorPrimitiveKernel k;std::size_t memory=0;std::string error;unsigned index=0;
    if(!k.prepare(d,std::span(&index,1),48000,64,2,1,memory,error)){formulas=false;continue;}
    PrimitiveContext c;measuring=true;bool ok=k.tick(c);k.commit();measuring=false;
    formulas&=ok&&std::abs(k.outputs()[0].value->left-value)<1e-9;
  }
  check(formulas,"elementary math matches analytic formulas");
  auto d=graph();auto h=makeNode("history","h");param(h,"initial",3);param(h,"next",9);d.nodes={h};unsigned index=0;
  CreatorPrimitiveKernel k;std::size_t memory=0;std::string error;PrimitiveContext c;
  bool state=k.prepare(d,std::span(&index,1),48000,64,2,1,memory,error)&&k.tick(c)&&k.outputs()[0].value->left==3;
  k.commit();state&=k.tick(c)&&k.outputs()[0].value->left==9;k.reset();state&=k.tick(c)&&k.outputs()[0].value->left==3;
  check(state,"History reads before commit and reset restores initial value");
  auto list=makeNode("list","list");list.capacity=2;list.values={3,4};auto append=makeNode("append","append");param(append,"value",9);
  auto indexNode=makeNode("number_to_integer","index");param(indexNode,"value",7);auto get=makeNode("get","get");get.valueType="list";
  d.nodes={list,append,indexNode,get};d.connections={{"list","append","out","collection"},{"list","get","out","collection"},{"index","get","out","index"}};
  std::array<unsigned,4> selected{0,1,2,3};CreatorPrimitiveKernel collections;memory=0;
  bool boundary=collections.prepare(d,selected,48000,64,2,1,memory,error)&&collections.tick(c);
  for(const auto &output:collections.outputs()){
    if(output.node=="append"&&output.port=="out")boundary&=output.value->size==2&&output.value->elements[0]==3&&output.value->elements[1]==4;
    if(output.node=="append"&&output.port=="success")boundary&=output.value->left==0;
    if(output.node=="get")boundary&=output.value->left==0;
  }
  check(boundary,"list overflow and invalid Get preserve data and return false flags");
}
static nlohmann::json benchmark(const MiniModuleDefinition &definition) {
  constexpr unsigned channels=64, modules=3, frames=128, blocks=32;
  constexpr double rate=48000;
  MiniModuleDefinition flat;std::string error;
  if(!expandSubgraphs(definition,flat,error)){check(false,error.c_str());return {};}
  std::vector<unsigned> selected(flat.nodes.size());std::iota(selected.begin(),selected.end(),0u);
  struct Timing { double ms=0,checksum=0; } host,direct;
  for(bool wrapped:{true,false}) {
    std::vector<std::unique_ptr<MiniModuleInstance>> instances;
    std::vector<std::unique_ptr<CreatorPrimitiveKernel>> kernels;
    std::vector<const PrimitiveValue *> results;
    for(unsigned i=0;i<channels*modules;++i) {
      if(wrapped){
        auto fx=std::make_unique<MiniModuleInstance>();
        if(!fx->configure(definition)||!fx->activate({rate,frames})){check(false,fx->error().c_str());return {};}
        fx->startProcessing();instances.push_back(std::move(fx));
      }else{
        auto kernel=std::make_unique<CreatorPrimitiveKernel>();std::size_t memory=0;
        if(!kernel->prepare(flat,selected,rate,frames,2,1,memory,error)){check(false,error.c_str());return {};}
        const PrimitiveValue *output=nullptr;
        for(const auto &p:kernel->outputs())for(const auto &n:flat.nodes)if(n.type=="output"&&p.node==n.id)output=p.value;
        if(!output){check(false,"benchmark output exists");return {};}
        results.push_back(output);kernels.push_back(std::move(kernel));
      }
    }
    std::array<std::array<float,frames>,2> input;
    std::array<std::array<std::array<float,frames>,2>,2> scratch{};
    for(unsigned i=0;i<frames;++i){input[0][i]=float(.3*std::sin(i*.073));input[1][i]=float(.2*std::cos(i*.067));}
    std::array<double,3> times{};double checksum=0;
    for(unsigned round=0;round<times.size();++round){
      for(auto &fx:instances)fx->reset();for(auto &kernel:kernels)kernel->reset();
      checksum=0;
      PrimitiveContext primitive;primitive.sampleRate=rate;primitive.playing=true;
      for(unsigned i=0;i<flat.controls.size();++i)primitive.controls[i]=flat.controls[i].initial;
      const auto start=std::chrono::steady_clock::now();measuring=true;
      for(unsigned block=0;block<blocks;++block)for(unsigned channel=0;channel<channels;++channel){
        const float *in[]{input[0].data(),input[1].data()};
        for(unsigned module=0;module<modules;++module){
          const unsigned index=channel*modules+module;
          float *out[]{scratch[module%2][0].data(),scratch[module%2][1].data()};
          if(wrapped){
            PluginProcessContext ctx;ctx.inputs=in;ctx.outputs=out;ctx.frames=frames;ctx.inputChannels=ctx.outputChannels=2;ctx.sampleTime=block*frames;
            if(instances[index]->process(ctx)==PluginProcessDisposition::Error)++failures;
          }else for(unsigned i=0;i<frames;++i){
            primitive.audio={in[0][i],in[1][i]};primitive.time=double(block*frames+i)/rate;
            if(!kernels[index]->tick(primitive))++failures;
            out[0][i]=float(results[index]->left);out[1][i]=float(results[index]->right);kernels[index]->commit();
          }
          in[0]=out[0];in[1]=out[1];
        }
        checksum+=in[0][0]+in[1][frames-1];
      }
      measuring=false;times[round]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    }
    std::sort(times.begin(),times.end());(wrapped?host:direct)={times[1],checksum};
  }
  check(std::abs(host.checksum-direct.checksum)<1e-4*std::max(1.,std::abs(direct.checksum)),"64 x 3 graph wrapper matches direct prepared DSP");
  return {{"name",definition.name},{"channels",channels},{"modules_per_channel",modules},{"sample_rate",rate},{"block_size",frames},
          {"audio_duration_ms",1000.*frames*blocks/rate},{"host_ms",host.ms},{"direct_kernel_ms",direct.ms},{"host_to_direct_ratio",host.ms/direct.ms},
          {"host_single_core_percent",host.ms/(1000.*frames*blocks/rate)*100},{"checksum_difference",host.checksum-direct.checksum}};
}
static void automationAndFailureTests() {
  auto d=graph();d.nodes.push_back(makeNode("interface","ui"));d.nodes.push_back(makeNode("audio_scale","scale"));
  d.controls={{"amount","Amount","",0,1,.25}};
  d.connections={{"in","scale","out","in"},{"ui","scale","amount","gain"},{"scale","out","out","in"}};
  MiniModuleInstance fx;bool ok=fx.configure(d)&&fx.activate({48000,64});fx.startProcessing();
  std::array<float,64> left,right,outL{},outR{};left.fill(1);right.fill(.5f);
  const float *inputs[]{left.data(),right.data()};float *outputs[]{outL.data(),outR.data()};
  PluginEvent event;event.frameOffset=19;event.value=.75;event.paramIndex=0;
  PluginProcessContext ctx;ctx.inputs=inputs;ctx.outputs=outputs;ctx.frames=64;ctx.inputChannels=ctx.outputChannels=2;ctx.inputEvents=std::span(&event,1);
  measuring=true;ok&=fx.process(ctx)!=PluginProcessDisposition::Error;measuring=false;
  for(unsigned i=0;i<64;++i)ok&=outL[i]==(i<19?.25f:.75f)&&outR[i]==outL[i]*.5f;
  check(ok,"native automation preserves intra-block offsets and stereo independence");
  auto failed=feedback();param(failed.nodes[3],"gain",2);
  MiniModuleInstance broken;ok=broken.configure(failed)&&broken.activate({48000,256});broken.startProcessing();
  std::array<float,256> source{},result{},other{};source[0]=1;const float *in[]{source.data(),source.data()};float *out[]{result.data(),other.data()};
  ctx.inputs=in;ctx.outputs=out;ctx.frames=256;ctx.inputEvents={};
  measuring=true;const auto disposition=broken.process(ctx);measuring=false;
  broken.pumpMainThread();
  check(ok&&disposition==PluginProcessDisposition::Error&&!broken.error().empty(),"unstable native DSP stops with a control-thread diagnostic");
}
int main(int argc,char **argv) {
  if(argc>2&&std::string_view(argv[1])=="--catalog") {
    const auto ports=[](const auto &values){auto result=nlohmann::json::array();for(const auto &p:values)result.push_back({{"id",p.id},{"name",p.name},{"type",portTypeId(p.type)},{"required",p.required},{"parameter_index",p.parameter},{"capacity",p.capacity}});return result;};
    auto nodes=nlohmann::json::array();
    for(const auto &description:nodeRegistry()) {
      auto node=makeNode(description.id,"node");const auto desc=describeNode(node);
      auto parameters=nlohmann::json::array();
      for(const auto &p:desc.parameters)parameters.push_back({{"id",p.id},{"name",p.name},{"unit",p.unit},{"min",p.minimum},{"max",p.maximum},{"default",p.initial},{"log",p.logarithmic},{"modulatable",p.modulatable},{"choices",p.choices}});
      nodes.push_back({{"id",desc.id},{"name",desc.name},{"category",desc.category},{"version",desc.version},{"inputs",ports(desc.inputs)},{"outputs",ports(desc.outputs)},{"parameters",parameters}});
    }
    std::ofstream file(argv[2]);file<<nlohmann::json{{"format","creator-node-catalog"},{"version",1},{"mini_module_version",5},{"nodes",nodes}}.dump(2);return file?0:1;
  }
  auto d = feedback();
  const auto validation = validate(d); if (!validation.empty()) std::printf("VALIDATION: %s\n", validation.c_str());
  check(validation.empty(), "previous-sample feedback validates");
  check(fromJson(toJson(d)) == d, "version 5 graph roundtrip");
  for (auto rate : {44100., 48000., 96000., 192000.}) for (unsigned channels : {1u, 2u}) {
    const auto one = render(d, rate, 1, channels), many = render(d, rate, 127, channels);
    check(one == many && !one.empty(), "feedback is independent of block size and channel count");
    if (one.size() > 2) check(std::abs(one[0] - 1) < 1e-7 && std::abs(one[1] - .5) < 1e-7 && std::abs(one[2] - .25) < 1e-7, "feedback impulse is exact");
  }
  auto bad = d; bad.nodes[2] = makeNode("audio_scale", "h"); bad.connections[3].toPort = "in";
  check(!validate(bad).empty(), "algebraic feedback is rejected");
  bad = d; bad.nodes.push_back(makeNode("chorus", "fx")); bad.connections[3].to = "fx"; bad.connections[3].toPort = "in"; bad.connections.push_back({"fx","h","out","next"});
  check(!validate(bad).empty(), "feedback through a full effect is rejected");
  auto collection = graph();
  auto array = makeNode("array", "a"); array.capacity = 3; array.values = {1,2,3};
  auto index = makeNode("number_to_integer", "i"); param(index,"value",1);
  auto set = makeNode("set", "set"); param(set,"value",7);
  collection.nodes.insert(collection.nodes.end(), {array,index,set,makeNode("sum","sum"),makeNode("number_to_audio","audio")});
  collection.connections = {{"a","set","out","collection"},{"i","set","out","index"},{"set","sum","out","collection"},{"sum","audio","out","value"},{"audio","out","out","in"}};
  auto audio = render(collection,48000,64);
  check(!audio.empty() && audio[0] == 11, "array Set and Sum");
  param(collection.nodes[3],"value",99); audio = render(collection,48000,64);
  check(!audio.empty() && audio[0] == 6, "invalid collection write preserves content");
  primitiveTests();
  automationAndFailureTests();
  auto delayed=graph();auto buffer=makeNode("delay_buffer","buffer");param(buffer,"maximum",20);
  auto tap=makeNode("delay_read","tap");param(tap,"time",1);
  delayed.nodes.insert(delayed.nodes.end(),{buffer,tap});delayed.connections={{"in","buffer","out","in"},{"buffer","tap","out","buffer"},{"tap","out","out","in"}};
  auto single=render(delayed,48000,1),block=render(delayed,48000,127);
  check(single==block&&single.size()>48&&single[0]==0&&single[48]==1,"prepared delay tap preserves sample time across blocks");
  MiniModuleInstance delayInstance;check(delayInstance.configure(delayed)&&delayInstance.activate({48000,127})&&delayInstance.latencySamples()==0&&!delayInstance.tailSamplesKnown(),"algorithmic delay has no PDC and declares unknown tail");
  auto crossed=delayed;auto color=makeNode("color","color");param(color,"drive",0);auto zero=makeNode("multiply","zero");param(zero,"b",0);
  auto compare=makeNode("compare","reset");param(compare,"b",16);param(compare,"operation",2);
  crossed.nodes.insert(crossed.nodes.end(),{color,makeNode("audio_to_number","control"),zero,makeNode("accumulator","clock"),compare});
  crossed.connections.insert(crossed.connections.end(),{{"in","color","out","in"},{"color","control","out","in"},{"control","zero","out","a"},{"zero","tap","out","time"},{"clock","reset","out","a"},{"reset","buffer","out","reset"}});
  auto crossedOne=render(crossed,48000,1),crossedBlock=render(crossed,48000,127);
  check(!crossedOne.empty()&&crossedOne==crossedBlock&&crossedOne[49]==1,"Buffer snapshots and reset epochs survive block/PDC island boundaries");
  auto pole=graph();auto pn=makeNode("one_pole","p");param(pn,"coefficient",.5);pole.nodes.push_back(pn);pole.connections={{"in","p","out","in"},{"p","out","out","in"}};
  auto response=render(pole,48000,127);check(response.size()>3&&response[0]==.5&&response[1]==.25&&response[2]==.125,"one-pole impulse response");
  auto fir=graph();auto coeff=makeNode("array","coeff");coeff.capacity=3;coeff.values={.25,.5,.25};fir.nodes.insert(fir.nodes.end(),{coeff,makeNode("fir","f")});fir.connections={{"in","f","out","in"},{"coeff","f","out","coefficients"},{"f","out","out","in"}};
  response=render(fir,48000,127);check(response.size()>3&&response[0]==.25&&response[1]==.5&&response[2]==.25&&response[3]==0,"FIR uses coefficient array once per sample");
  auto biquad=graph();auto bq=makeNode("array","coeff");bq.capacity=5;bq.values={.5,0,0,-.5,0};biquad.nodes.insert(biquad.nodes.end(),{bq,makeNode("biquad","b")});biquad.connections={{"in","b","out","in"},{"coeff","b","out","coefficients"},{"b","out","out","in"}};
  check(render(biquad,48000,17)==render(pole,48000,17),"biquad normalized coefficients implement the same one-pole transfer");
  auto mapped=graph();auto values=makeNode("array","array");values.capacity=3;values.values={1,2,3};auto map=makeNode("map","map");map.subgraph="custom";
  mapped.subgraphs={body(true)};mapped.nodes.insert(mapped.nodes.end(),{values,map,makeNode("sum","sum"),makeNode("number_to_audio","audio")});
  mapped.connections={{"array","map","out","collection"},{"map","sum","out","collection"},{"sum","audio","out","value"},{"audio","out","out","in"}};
  response=render(mapped,48000,17);check(!response.empty()&&response[0]==12,"Map executes bounded nested element graph");
  auto reduced=mapped;reduced.nodes[3]=makeNode("reduce","map");reduced.nodes[3].subgraph="custom";reduced.subgraphs={body(true,true)};
  reduced.connections={{"array","map","out","collection"},{"map","audio","out","value"},{"audio","out","out","in"}};
  reduced.nodes.erase(reduced.nodes.begin()+4);
  response=render(reduced,48000,17);check(!response.empty()&&response[0]==6,"Reduce passes accumulator through each element");
  auto custom=graph();auto instance=makeNode("subgraph","custom1");instance.subgraph="custom";custom.subgraphs={body()};custom.nodes.push_back(instance);custom.connections={{"in","custom1","out","input"},{"custom1","out","result","in"}};
  response=render(custom,48000,17);check(!response.empty()&&response[0]==2,"custom graph expands and executes");
  auto roundtrip=fromJson(toJson(custom));check(roundtrip==custom&&render(roundtrip,48000,17)==response,"embedded definitions preserve sound after serialization");
  auto shared=custom;auto second=instance;second.id="custom2";shared.nodes.push_back(second);
  shared.subgraphs[0].nodes.back()=makeNode("one_pole","op");param(shared.subgraphs[0].nodes.back(),"coefficient",.5);
  shared.connections={{"in","custom1","out","input"},{"custom1","custom2","result","input"},{"custom2","out","result","in"}};
  auto sharedResponse=render(shared,48000,17);check(sharedResponse.size()>2&&sharedResponse[0]==.25&&sharedResponse[1]==.25&&sharedResponse[2]==.1875,"instances of a shared definition keep independent filter state");
  for(auto factor:{2u,4u}) {
    custom.subgraphs[0].oversampling=factor;
    MiniModuleInstance fx;const bool prepared=fx.configure(custom)&&fx.activate({48000,127});
    response=render(custom,48000,127);const auto ref=render(custom,48000,1);
    const auto peak=response.empty()?0u:unsigned(std::max_element(response.begin(),response.end())-response.begin());
    check(prepared&&fx.latencySamples()==(factor==2?32u:48u)&&peak==fx.latencySamples()&&response==ref,"oversampling reports FIR latency and is block independent");
  }
  auto parallel=custom;parallel.nodes.push_back(makeNode("audio_add","sum"));parallel.connections={{"in","custom1","out","input"},{"in","sum","out","a"},{"custom1","sum","result","b"},{"sum","out","out","in"}};
  response=render(parallel,48000,127);check(response.size()>48&&response[48]>2.5f&&std::abs(response[0])<1e-7,"dry branch is aligned with oversampling latency");
  auto recursive=custom;recursive.subgraphs[0].nodes.push_back(instance);check(!validate(recursive).empty(),"recursive custom definitions are rejected");
  auto memoryLimit=graph();for(unsigned i=0;i<12;++i){auto n=makeNode("delay_buffer","d"+std::to_string(i));param(n,"maximum",10000);memoryLimit.nodes.push_back(n);memoryLimit.connections.push_back({"in",n.id,"out","in"});auto t=makeNode("delay_read","t"+std::to_string(i));memoryLimit.nodes.push_back(t);memoryLimit.connections.push_back({n.id,t.id,"out","buffer"});}
  auto mix=makeNode("audio_add","mix");memoryLimit.nodes.push_back(mix);memoryLimit.connections.push_back({"t0","mix","out","a"});memoryLimit.connections.push_back({"t1","mix","out","b"});memoryLimit.connections.push_back({"mix","out","out","in"});
  MiniModuleInstance limited;check(limited.configure(memoryLimit)&&!limited.activate({192000,127}),"prepared state exceeding 16 MiB is rejected");
  if(argc>1) {
    nlohmann::json benchmarks=nlohmann::json::array();
    for(const auto &file:std::filesystem::directory_iterator(argv[1])) if(file.path().extension()==".vltmini") {
      std::ifstream in(file.path());nlohmann::json data;in>>data;auto example=fromJson(data.at("definition"));
      bool valid=true;
      for(auto rate:{44100.,48000.,96000.,192000.}) for(auto channels:{1u,2u}) {
        auto result=render(example,rate,127,channels), offline=render(example,rate,31,channels,true);
        valid&=!result.empty()&&result==offline;
      }
      check(valid,file.path().filename().string().c_str());
      if(argc>2&&std::string_view(argv[2])=="--benchmark")benchmarks.push_back(benchmark(example));
    }
    if(!benchmarks.empty()){
      std::ofstream report(argc>3?argv[3]:"creator-programming-benchmark.json");report<<benchmarks.dump(2);
      std::printf("BENCHMARK %s\n",benchmarks.dump().c_str());
    }
  }
  check(allocations == 0, "processing performs no allocations");
  return failures ? 1 : 0;
}
