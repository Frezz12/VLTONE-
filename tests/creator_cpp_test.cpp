#include "Creator/CodeUtilities.hpp"
#include "Creator/CompilerProcess.hpp"
#include "Creator/CreatorCompilerClient.hpp"
#include "Creator/WasmProgram.hpp"
#include "Internal/MiniModuleInstance.hpp"
#include "Internal/MiniNodeRegistry.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <nlohmann/json.hpp>
#include <thread>

using namespace daw::plugins;
using namespace daw::plugins::mini;
using nlohmann::json;
static std::atomic<bool> counting{false};
static std::atomic<unsigned> allocations{0};
void *operator new(std::size_t size) {
  if (counting)
    ++allocations;
  if (auto *p = std::malloc(std::max(size, std::size_t(1))))
    return p;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
static int failures = 0;
static void check(bool ok, const char *name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  failures += !ok;
}
static FunctionDefinition function(std::string source,
                                   std::string name = "process") {
  FunctionDefinition f;
  f.source = "#include <vlt/creator.hpp>\n" + source;
  f.entry = name;
  auto a = analyzeFunction(f);
  if (!a) {
    std::printf("ANALYSIS: %s\n", a.error.c_str());
    throw std::runtime_error("Analysis failed");
  }
  return a.function;
}
static MiniModuleDefinition graph(FunctionDefinition f) {
  MiniModuleDefinition d;
  d.version = 4;
  d.id = "test.cpp";
  d.name = "C++ test";
  d.nodes = {makeNode("input", "in"), makeNode("output", "out"),
             makeNode("cpp_function", "cpp"), makeNode("interface", "ui")};
  d.nodes[2].function = std::move(f);
  for (const auto &p : d.nodes[2].function->inputs)
    if (p.type == "audio") {
      d.connections.push_back({"in", "cpp", "out", p.id});
      break;
    }
  for (const auto &p : d.nodes[2].function->outputs)
    if (p.type == "audio") {
      d.connections.push_back({"cpp", "out", p.id, "in"});
      break;
    }
  return d;
}
static bool compile(MiniModuleDefinition &d) {
  std::string error;
  std::vector<CodeDiagnostic> ds;
  const bool ok = compileCppGraph(d, error, ds);
  if (!ok)
    std::printf("COMPILE: %s\n", error.c_str());
  return ok;
}
static bool prepare(MiniModuleInstance &module, const MiniModuleDefinition &d,
                    double rate = 48000, unsigned size = 64,
                    unsigned channels = 2) {
  PluginBusLayout accepted;
  const bool ok =
      module.configure(d) &&
      module.setBusLayout(
          {{std::uint16_t(channels)}, {std::uint16_t(channels)}}, accepted) &&
      module.activate({rate, size});
  if (!ok)
    std::printf("PREPARE: %s\n", module.error().c_str());
  return ok;
}
struct Audio {
  std::vector<float> l, r, ol, oright;
  const float *in[2];
  float *out[2];
  explicit Audio(unsigned n)
      : l(n, .4f), r(n, -.2f), ol(n), oright(n), in{l.data(), r.data()},
        out{ol.data(), oright.data()} {}
  PluginProcessDisposition run(MiniModuleInstance &m, unsigned channels = 2,
                               std::span<const PluginEvent> events = {},
                               bool offline = false) {
    PluginProcessContext c;
    c.inputs = in;
    c.outputs = out;
    c.inputChannels = c.outputChannels = std::uint16_t(channels);
    c.frames = unsigned(l.size());
    c.inputEvents = events;
    c.offline = offline;
    c.playing = true;
    c.transport.tempo = 120;
    return m.process(c);
  }
};
int main(int argc, char **argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  try {
    check(creatorCompilerAvailable(), "bundled compiler is installed");
    auto f = function("vlt::AudioFrame process(vlt::AudioFrame input,float "
                      "drive=.5f){return input*drive;}");
    check(f.inputs.size() == 2 && f.outputs.size() == 1 &&
              f.inputs[1].initial == .5,
          "Clang extracts real types and defaults");
    auto renamed = f;
    auto at = renamed.source.find("float drive");
    renamed.source.replace(at, 11, "float amount");
    at = renamed.source.find("input*drive");
    renamed.source.replace(at, 11, "input*amount");
    auto renamedAnalysis = analyzeFunction(renamed);
    check(renamedAnalysis &&
              renamedAnalysis.function.inputs[1].id == f.inputs[1].id,
          "parameter rename preserves its annotated ID");
    auto reordered = function("float process(float a,float b){return a+b;}");
    const auto oldPorts = reordered.inputs;
    auto first = reordered.source.find("VLT_PORT(");
    auto second = reordered.source.find("VLT_PORT(", first + 1);
    auto end = reordered.source.find("){", second);
    const auto left = reordered.source.substr(first, second - first - 1);
    const auto right = reordered.source.substr(second, end - second);
    reordered.source.replace(first, end - first, right + "," + left);
    auto reorderAnalysis = analyzeFunction(reordered);
    check(reorderAnalysis &&
              reorderAnalysis.function.inputs[0].id == oldPorts[1].id &&
              reorderAnalysis.function.inputs[1].id == oldPorts[0].id,
          "reordering parameters preserves stable port IDs");
    auto broken = f;
    broken.source += "\nthis is invalid C++;";
    auto bad = analyzeFunction(broken);
    check(!bad && !bad.diagnostics.empty() && bad.diagnostics.front().line > 0,
          "compiler diagnostics include source locations");
    auto d = graph(f);
    check(compile(d), "C++23 builds portable WebAssembly");
    check(fromJson(toJson(d)) == d,
          "source signature and portable code survive JSON roundtrip");
    bool exact = true, noAlloc = true;
    const auto runtimeMemoryBefore = WasmProgram::realtimeMemoryOperations();
    for (double rate : {44100., 48000., 96000., 192000.})
      for (unsigned channels : {1u, 2u})
        for (unsigned size : {1u, 17u, 256u}) {
          MiniModuleInstance module;
          if (!prepare(module, d, rate, size, channels)) {
            exact = false;
            continue;
          }
          Audio audio(513);
          allocations = 0;
          counting = true;
          const auto result = audio.run(module, channels);
          counting = false;
          noAlloc &= allocations == 0;
          exact &= result == PluginProcessDisposition::Continue;
          for (unsigned i = 0; i < audio.l.size(); ++i)
            exact &= audio.ol[i] == .2f &&
                     (channels == 1 || audio.oright[i] == -.1f);
        }
    check(exact, "mono/stereo and 44.1/48/96/192 kHz across block sizes");
    check(noAlloc, "C++ graph rendering performs no C++ allocations");
    auto automated = d;
    automated.controls = {{"drive", "Drive", "", 0, 1, .5}};
    automated.connections.push_back({"ui", "cpp", "drive", f.inputs[1].id});
    check(compile(automated), "compile interface modulation");
    MiniModuleInstance automation;
    check(prepare(automation, automated), "prepare automation graph");
    Audio a(97);
    PluginEvent events[2]{};
    events[0].frameOffset = 17;
    events[0].value = .25;
    events[1].frameOffset = 67;
    events[1].value = .75;
    check(a.run(automation, 2, events) == PluginProcessDisposition::Continue &&
              a.ol[16] == .2f && a.ol[17] == .1f && a.ol[66] == .1f &&
              a.ol[67] == .3f,
          "external automation retains frame offsets");
    auto state =
        function("struct State {float lastL=0,lastR=0;}; vlt::AudioFrame "
                 "process(State &s,const vlt::Context &c,vlt::AudioFrame "
                 "input){s.lastL+=(input.left-s.lastL)*.1f;s.lastR+=(input."
                 "right-s.lastR)*.1f;return {s.lastL,s.lastR};}");
    auto stateGraph = graph(state);
    check(compile(stateGraph), "stateful filter compiles");
    MiniModuleInstance live, offline;
    bool prepared = prepare(live, stateGraph, 48000, 17) &&
                    prepare(offline, stateGraph, 48000, 256);
    Audio l(511), o(511);
    bool stateOk =
        prepared && l.run(live) == PluginProcessDisposition::Continue &&
        o.run(offline, 2, {}, true) == PluginProcessDisposition::Continue &&
        l.ol == o.ol && l.oright == o.oright;
    live.reset();
    Audio reset(511);
    stateOk &= reset.run(live) == PluginProcessDisposition::Continue &&
               reset.ol == l.ol;
    check(stateOk, "explicit state is independent of block partition, "
                   "live/offline and reset");
    bool threadOk = false;
    std::thread audioWorker([&] {
      Audio audio(97);
      threadOk = audio.run(live) == PluginProcessDisposition::Continue;
    });
    audioWorker.join();
    check(threadOk, "prepared DSP runs on a different audio worker");
    auto helper = function("struct State{float n=0;};float helper(State "
                           "&s,float x){s.n+=1;return x+s.n;}",
                           "helper");
    auto caller =
        function("vlt::AudioFrame process(vlt::AudioFrame "
                 "input,vlt::Function<float(float)> helper={}){return "
                 "{helper(input.left),input.right};}");
    auto composed = graph(caller);
    auto child = makeNode("cpp_function", "helper");
    child.function = helper;
    composed.nodes.push_back(child);
    composed.connections.push_back(
        {"helper", "cpp", "function", caller.inputs[1].id});
    check(compile(composed),
          "function dependency compiles with private child state");
    MiniModuleInstance composite;
    Audio c(3);
    check(prepare(composite, composed) &&
              c.run(composite) == PluginProcessDisposition::Continue &&
              std::abs(c.ol[0] - 1.4f) < 1e-6 &&
              std::abs(c.ol[2] - 3.4f) < 1e-6,
          "caller arguments and callable state drive the helper");
    auto independent = composed;
    auto caller2 = independent.nodes[2];
    caller2.id = "caller2";
    independent.nodes.push_back(caller2);
    independent.nodes.push_back(makeNode("mix", "mix"));
    independent.connections[1] = {"cpp", "mix", caller.outputs[0].id, "a"};
    independent.connections.push_back(
        {"in", "caller2", "out", caller.inputs[0].id});
    independent.connections.push_back(
        {"helper", "caller2", "function", caller.inputs[1].id});
    independent.connections.push_back(
        {"caller2", "mix", caller.outputs[0].id, "b"});
    independent.connections.push_back({"mix", "out", "out", "in"});
    MiniModuleInstance callers;
    Audio ia(3);
    check(compile(independent) && prepare(callers, independent) &&
              ia.run(callers) == PluginProcessDisposition::Continue &&
              ia.ol == c.ol,
          "two callers share code but keep independent helper state");
    auto intermediate =
        function("float wrapper(float x,vlt::Function<float(float)> "
                 "next={}){return next(x)+2;}",
                 "wrapper");
    auto nested = composed;
    auto wrapper = makeNode("cpp_function", "wrapper");
    wrapper.function = intermediate;
    nested.nodes.push_back(wrapper);
    nested.connections.back().from = "wrapper";
    nested.connections.push_back(
        {"helper", "wrapper", "function", intermediate.inputs[1].id});
    MiniModuleInstance nestedInstance;
    Audio na(3);
    check(compile(nested) && prepare(nestedInstance, nested) &&
              na.run(nestedInstance) == PluginProcessDisposition::Continue &&
              std::abs(na.ol[0] - 3.4f) < 1e-6,
          "nested callable dependencies run with explicit arguments");
    auto recursive = nested;
    recursive.nodes[4].function = intermediate;
    recursive.connections.push_back(
        {"wrapper", "helper", "function", intermediate.inputs[1].id});
    check(validateTypedGraph(recursive).find("recursive cycle") !=
              std::string::npos,
          "recursive function dependencies are rejected separately from signal "
          "flow");
    auto local =
        function("VLT_NODE float helper(float x){return x*.5f;}\nVLT_NODE "
                 "vlt::AudioFrame process(vlt::AudioFrame input){return "
                 "{helper(input.left),helper(input.right)};}");
    auto extraction =
        creatorCompilerRequest({{"action", "extract"},
                                {"function", functionToJson(local)},
                                {"name", "helper"}});
    check(extraction.value("ok", false),
          "Clang extracts a helper into a typed function binding");
    if (extraction.value("ok", false)) {
      auto extracted = graph(functionFromJson(extraction["parent"]));
      auto child = makeNode("cpp_function", "helper");
      child.function = functionFromJson(extraction["child"]);
      extracted.nodes.push_back(child);
      extracted.connections.push_back(
          {"helper", "cpp", "function", extraction["port"]});
      MiniModuleInstance module;
      Audio audio(64);
      check(compile(extracted) && prepare(module, extracted) &&
                audio.run(module) == PluginProcessDisposition::Continue &&
                audio.ol[0] == .2f && audio.oright[0] == -.1f,
            "extracted helper preserves audio result");
    } else
      std::printf("EXTRACT: %s\n", extraction.dump().c_str());
    auto multi =
        function("struct State{float n=0;};struct Outputs{vlt::AudioFrame "
                 "audio;float count;};Outputs process(State &s,vlt::AudioFrame "
                 "input){return {input,++s.n};}");
    auto multiple = graph(multi);
    multiple.nodes.push_back(makeNode("number_to_audio", "number"));
    multiple.nodes.push_back(makeNode("mix", "mix"));
    multiple.connections = {{"in", "cpp", "out", multi.inputs[0].id},
                            {"cpp", "number", multi.outputs[1].id, "value"},
                            {"cpp", "mix", multi.outputs[0].id, "a"},
                            {"number", "mix", "out", "b"},
                            {"mix", "out", "out", "in"}};
    MiniModuleInstance multipleInstance;
    Audio ma(3);
    check(compile(multiple) && prepare(multipleInstance, multiple) &&
              ma.run(multipleInstance) == PluginProcessDisposition::Continue &&
              std::abs(ma.ol[0] - .7f) < 1e-6 &&
              std::abs(ma.ol[2] - 1.7f) < 1e-6,
          "multiple outputs are computed once, including state changes");
    auto singleStruct =
        graph(function("struct Output{vlt::AudioFrame audio;};Output "
                       "process(vlt::AudioFrame input){return {input*.5f};}"));
    MiniModuleInstance structInstance;
    Audio sa(17);
    check(compile(singleStruct) && prepare(structInstance, singleStruct) &&
              sa.run(structInstance) == PluginProcessDisposition::Continue &&
              sa.ol[0] == .2f,
          "single-field aggregate output retains its C++ return type");
    auto delay = function(R"CPP(
struct State {vlt::Buffer<vlt::AudioFrame> data; unsigned at=0;};
void prepare(State &s,vlt::PrepareContext &c){s.data.prepare(5);c.latency=5;c.tail=5;}
void reset(State &s){s.data.clear();s.at=0;}
vlt::AudioFrame process(State &s,vlt::AudioFrame input){auto output=s.data[s.at];s.data[s.at]=input;s.at=(s.at+1)%5;return output;}
)CPP");
    auto delayed = graph(delay);
    delayed.nodes.push_back(makeNode("mix", "mix"));
    delayed.connections[1] = {"cpp", "mix", delay.outputs[0].id, "a"};
    delayed.connections.push_back({"in", "mix", "out", "b"});
    delayed.connections.push_back({"mix", "out", "out", "in"});
    MiniModuleInstance delayedInstance;
    Audio da(27);
    bool aligned =
        compile(delayed) && prepare(delayedInstance, delayed, 48000, 3) &&
        da.run(delayedInstance) == PluginProcessDisposition::Continue &&
        delayedInstance.latencySamples() == 5 &&
        delayedInstance.tailSamples() >= 5;
    for (unsigned i = 0; i < da.l.size(); ++i)
      aligned &= da.ol[i] == (i < 5 ? 0.f : .4f);
    delayedInstance.reset();
    Audio ra(27);
    aligned &= ra.run(delayedInstance) == PluginProcessDisposition::Continue &&
               ra.ol == da.ol;
    check(aligned, "prepared delay buffers, reset and parallel branch latency "
                   "compensation");
    std::atomic<bool> cancel{true};
    auto cancelled = analyzeFunction(f, &cancel);
    check(!cancelled,
          "compiler cancellation returns without publishing a result");
    for (const auto &source : std::vector<std::string>{
             "vlt::AudioFrame process(vlt::AudioFrame input){volatile unsigned "
             "i=0;for(;;){i=i+1;}return input;}",
             "vlt::AudioFrame process(vlt::AudioFrame input){auto "
             "*p=reinterpret_cast<volatile float *>(0xfffffffc);return "
             "{*p,input.right};}",
             "vlt::AudioFrame process(vlt::AudioFrame "
             "input){vlt::Buffer<float> b;b.prepare(8);return input;}",
             "#include <cstdlib>\nvlt::AudioFrame process(vlt::AudioFrame "
             "input){auto *p=static_cast<volatile float "
             "*>(std::malloc(1024));*p=input.left;return {*p,input.right};}",
             "__attribute__((noinline)) float recurse(unsigned n){volatile "
             "float data[1024];data[n%1024]=float(n);return "
             "recurse(n+1)+data[n%1024];}vlt::AudioFrame "
             "process(vlt::AudioFrame input){return "
             "{recurse(1),input.right};}"}) {
      auto fault = graph(function(source));
      MiniModuleInstance module;
      Audio audio(17);
      const bool ready = compile(fault) && prepare(module, fault);
      allocations = 0;
      counting = true;
      const auto start = std::chrono::steady_clock::now();
      const bool trapped =
          ready && audio.run(module) == PluginProcessDisposition::Error;
      const auto elapsed = std::chrono::steady_clock::now() - start;
      counting = false;
      check(trapped && elapsed < std::chrono::seconds(1) && allocations == 0,
            "loop/memory/allocation fault is contained without allocation");
      module.pumpMainThread();
      check(!module.error().empty(),
            "runtime diagnostics reach the host outside the audio callback");
    }
    auto recoverable = graph(function(
        "__attribute__((noinline)) float recurse(unsigned n){volatile float "
        "data[1024];data[n%1024]=float(n);return recurse(n+1)+data[n%1024];}"
        "vlt::AudioFrame process(vlt::AudioFrame input){return "
        "input.left<0?vlt::AudioFrame{recurse(1),0}:input;}"));
    MiniModuleInstance recovery;
    Audio faultInput(17), goodInput(17);
    std::fill(faultInput.l.begin(), faultInput.l.end(), -.4f);
    const bool faulted =
        compile(recoverable) && prepare(recovery, recoverable) &&
        faultInput.run(recovery) == PluginProcessDisposition::Error;
    recovery.reset();
    check(
        faulted &&
            goodInput.run(recovery) == PluginProcessDisposition::Continue &&
            goodInput.ol == goodInput.l,
        "reset restores the compiler ABI stack after a trapped recursive call");
    check(
        WasmProgram::realtimeMemoryOperations() == runtimeMemoryBefore,
        "WAMR performs no malloc, realloc or free during DSP, reset or faults");
    auto forbidden = graph(
        function("#include <cstdio>\nvlt::AudioFrame process(vlt::AudioFrame "
                 "input){std::puts(\"forbidden\");return input;}"));
    std::string rejected;
    std::vector<CodeDiagnostic> rejectedDiagnostics;
    check(!compileCppGraph(forbidden, rejected, rejectedDiagnostics),
          "system imports are unavailable to portable code");
    auto corrupt = d;
    corrupt.code.wasm = "AGFzbQ==";
    MiniModuleInstance invalid;
    check(!prepare(invalid, corrupt), "corrupt portable code never starts DSP");
    auto unavailable = toJson(d);
    unavailable["nodes"][2]["function"]["sdk"] = 99;
    unavailable["nodes"][2]["function"]["futureState"] = {{"schema", 7},
                                                          {"data", {1, 2, 3}}};
    const auto unknown = fromJson(unavailable);
    check(toJson(unknown) == unavailable && !validate(unknown).empty(),
          "unavailable SDK definitions preserve sources without loss");
    auto futureCode = toJson(d);
    futureCode["code"]["abi"] = 99;
    futureCode["code"]["futurePayload"] = {{"data", {1, 2, 3}}};
    const auto unknownCode = fromJson(futureCode);
    check(toJson(unknownCode) == futureCode && !validate(unknownCode).empty(),
          "unavailable code ABI preserves the complete portable payload");
    for (const auto *name : {"Waveshaper", "OnePole", "Composed"}) {
      FunctionDefinition exampleFunction;
      exampleFunction.source = readCreatorText(
          fromUtf8(CREATOR_EXAMPLES_DIRECTORY) / (std::string(name) + ".cpp"),
          kMaxFunctionSourceBytes);
      auto analysis = analyzeFunction(exampleFunction);
      if (!analysis)
        throw std::runtime_error(analysis.error);
      auto example = graph(analysis.function);
      example.id = std::string("example.cpp.") + name;
      example.name = name;
      for (const auto &p : analysis.function.inputs)
        if (p.type == "number") {
          const bool frequency = p.name == "cutoff";
          const double minimum = frequency ? 20. : p.name == "drive" ? .1 : 0.;
          const double maximum = frequency           ? 20000.
                                 : p.name == "drive" ? 6.
                                                     : 1.;
          example.controls.push_back({p.id, p.name, frequency ? "Hz" : "",
                                      minimum, maximum, p.initial, frequency});
          example.connections.push_back({"ui", "cpp", p.id, p.id});
        }
      if (std::string_view(name) == "Composed") {
        const auto extracted = creatorCompilerRequest(
            {{"action", "extract"},
             {"function", functionToJson(analysis.function)},
             {"name", "saturate"}});
        if (!extracted.value("ok", false))
          throw std::runtime_error(
              extracted.value("error", "Example extraction failed"));
        example.nodes[2].function = functionFromJson(extracted["parent"]);
        auto helper = makeNode("cpp_function", "saturate");
        helper.function = functionFromJson(extracted["child"]);
        example.nodes.push_back(helper);
        example.connections.push_back(
            {"saturate", "cpp", "function", extracted.at("port")});
      }
      MiniModuleInstance instance;
      Audio samples(117);
      const bool valid =
          validate(example).empty() && compile(example) &&
          prepare(instance, example) &&
          samples.run(instance) == PluginProcessDisposition::Continue;
      check(valid, (std::string("portable example: ") + name).c_str());
      if (valid && argc == 3 &&
          std::string_view(argv[1]) == "--write-examples") {
        const auto folder = fromUtf8(argv[2]);
        std::filesystem::create_directories(folder);
        const json project{{"format", "vltcreator"},
                           {"version", 2},
                           {"name", name},
                           {"activeMode", ""},
                           {"codeNode", "cpp"},
                           {"codeCursors", json::object()},
                           {"viewports", json::object()},
                           {"definition", toJson(example)},
                           {"positions",
                            {{"",
                              {{"in", {30, 60}},
                               {"cpp", {430, 60}},
                               {"out", {810, 60}},
                               {"ui", {30, 285}},
                               {"saturate", {30, 510}}}}}}};
        check(writeCreatorText(folder / (std::string(name) + ".vltcreator"),
                               project.dump(2)) &&
                  writeCreatorText(folder / (std::string(name) + ".vltmini"),
                                   json{{"format", "vltmini"},
                                        {"version", 4},
                                        {"definition", toJson(example)}}
                                       .dump(2)),
              "example project and mini-module written");
      }
    }

    // 64 stereo strips, three identical modules each. Compare the whole module
    // boundary to the equivalent native Gain graph, not a bare multiply loop.
    auto native = d;
    native.code = {};
    native.nodes[2] = makeNode("gain", "cpp");
    native.connections = {{"in", "cpp", "out", "in"},
                          {"cpp", "out", "out", "in"}};
    for (auto &p : native.nodes[2].parameters)
      if (p.id == "gain")
        p.value = .5;
    const auto benchmark = [&](const MiniModuleDefinition &definition) {
      std::vector<std::unique_ptr<MiniModuleInstance>> instances;
      for (unsigned i = 0; i < 192; ++i) {
        auto p = std::make_unique<MiniModuleInstance>();
        if (!prepare(*p, definition, 48000, 256))
          return -1.;
        instances.push_back(std::move(p));
      }
      Audio block(256);
      for (auto &p : instances)
        block.run(*p);
      const auto start = std::chrono::steady_clock::now();
      for (unsigned repeat = 0; repeat < 64; ++repeat)
        for (auto &p : instances)
          if (block.run(*p) != PluginProcessDisposition::Continue)
            return -1.;
      return std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - start)
                 .count() /
             64;
    };
    const double nativeMs = benchmark(native), cppMs = benchmark(d);
    check(nativeMs > 0 && cppMs > 0,
          "64 channels with three C++ modules complete the benchmark");
    std::printf("MEASURE 192 modules, stereo 256/48k: native %.3f ms; C++ %.3f "
                "ms; ratio %.2fx; device period 5.333 ms\n",
                nativeMs, cppMs, cppMs / nativeMs);
  } catch (const std::exception &e) {
    std::printf("FAIL exception: %s\n", e.what());
    ++failures;
  }
  return failures ? 1 : 0;
}
