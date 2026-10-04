#include "Compiler.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Creator/CompilerProcess.hpp"
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace creator {
namespace {
void replaceCacheFile(const std::filesystem::path &from,
                      const std::filesystem::path &to) {
#ifdef _WIN32
  if (!MoveFileExW(from.c_str(), to.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot publish local code cache");
#else
  std::filesystem::rename(from, to);
#endif
}
std::string exe(const char *name) {
#ifdef _WIN32
  return std::string(name) + ".exe";
#else
  return name;
#endif
}
struct Node {
  std::string id;
  unsigned index;
  bool scheduled;
  FunctionDefinition f;
};
std::string type(const FunctionPort &p) {
  return p.type == "audio"  ? "vlt::AudioFrame"
         : p.type == "gate" ? "bool"
                            : "float";
}
std::string sourceUnit(const json &request, const std::filesystem::path &tools,
                       json &diagnostics) {
  std::vector<Node> nodes;
  std::map<std::string, unsigned> ids;
  if (!request.at("functions").is_array() ||
      request.at("functions").size() > 64)
    throw std::runtime_error("Invalid function count");
  std::string includes = "#include <vlt/creator.hpp>\n";
  std::vector<std::string> sources;
  for (const auto &item : request.at("functions")) {
    auto original = functionFromJson(item.at("function"));
    auto analyzed = analyze(original, tools, item.at("id"), false);
    for (auto &d : analyzed.diagnostics)
      diagnostics.push_back(d);
    if (!analyzed.error.empty())
      throw std::runtime_error(analyzed.error);
    if (functionToJson(original) != functionToJson(analyzed.function))
      throw std::runtime_error(
          "Function ports changed; update the node before compiling: " +
          item.at("id").get<std::string>());
    Node n{item.at("id"), item.at("index"), item.value("scheduled", true),
           std::move(analyzed.function)};
    if (n.index >= 64 || !ids.emplace(n.id, unsigned(nodes.size())).second)
      throw std::runtime_error("Invalid function identity");
    // Includes remain at translation-unit scope. Each function's declarations
    // live in their own namespace, so common names like State never collide.
    std::istringstream lines(n.f.source);
    std::string line, body;
    while (std::getline(lines, line)) {
      const auto first = line.find_first_not_of(" \t");
      if (first != std::string::npos &&
          line.compare(first, 8, "#include") == 0) {
        if (line.ends_with('\\'))
          throw std::runtime_error("Use a single-line #include directive");
        includes += line + "\n";
        body += "\n";
      } else
        body += line + "\n";
    }
    sources.push_back(std::move(body));
    nodes.push_back(std::move(n));
  }
  std::map<std::pair<unsigned, std::string>, unsigned> bindings;
  for (const auto &b : request.at("bindings")) {
    if (!ids.contains(b.at("from")) || !ids.contains(b.at("to")))
      throw std::runtime_error("Missing function dependency");
    auto from = ids.at(b.at("from")), to = ids.at(b.at("to"));
    const auto port = b.at("port").get<std::string>();
    auto input = std::find_if(
        nodes[to].f.inputs.begin(), nodes[to].f.inputs.end(),
        [&](const auto &p) { return p.id == port && p.type == "function"; });
    if (input == nodes[to].f.inputs.end() ||
        input->signature != callableSignature(nodes[from].f) ||
        !bindings.emplace(std::pair{to, port}, from).second)
      throw std::runtime_error("Incompatible function binding");
  }
  std::vector<unsigned> order, mark(nodes.size());
  std::function<void(unsigned)> visit = [&](unsigned i) {
    if (mark[i] == 1)
      throw std::runtime_error("Recursive function dependency");
    if (mark[i] == 2)
      return;
    mark[i] = 1;
    for (const auto &p : nodes[i].f.inputs)
      if (p.type == "function") {
        auto b = bindings.find({i, p.id});
        if (b == bindings.end())
          throw std::runtime_error("Connect function input: " + nodes[i].id +
                                   "." + p.name);
        visit(b->second);
      }
    mark[i] = 2;
    order.push_back(i);
  };
  for (unsigned i = 0; i < nodes.size(); ++i)
    visit(i);
  std::vector<unsigned> expansion(nodes.size());
  for (auto i : order) {
    expansion[i] = 1;
    for (const auto &p : nodes[i].f.inputs)
      if (p.type == "function")
        expansion[i] += expansion[bindings.at({i, p.id})];
    if (expansion[i] > 512)
      throw std::runtime_error("Function dependencies expand to too many state "
                               "instances (maximum 512)");
  }
  std::string text = includes + R"CPP(
extern "C" void *__wrap_malloc(unsigned){__builtin_trap();}
extern "C" void *__wrap_calloc(unsigned,unsigned){__builtin_trap();}
extern "C" void *__wrap_realloc(void *,unsigned){__builtin_trap();}
extern "C" void __wrap_free(void *){__builtin_trap();}
)CPP";
  for (unsigned i = 0; i < nodes.size(); ++i) {
    text += "\nnamespace n" + std::to_string(i) + " {\n#line 1 " +
            json(nodes[i].id + ".cpp").dump() + "\n" + sources[i] + "\n}\n";
  }
  text += "\n#line 1 \"creator-generated.cpp\"\n";
  for (auto i : order) {
    const auto &f = nodes[i].f;
    const auto ns = "n" + std::to_string(i) + "::";
    const auto root = "Root" + std::to_string(i);
    text += "struct " + root + " {\n";
    if (!f.stateType.empty())
      text += ns + f.stateType + " state{};\n";
    for (unsigned p = 0; p < f.inputs.size(); ++p)
      if (f.inputs[p].type == "function")
        text += "Root" + std::to_string(bindings.at({i, f.inputs[p].id})) +
                " dependency" + std::to_string(p) + ";\n";
    text += "void prepare(vlt::PrepareContext &c) {\n";
    for (unsigned p = 0; p < f.inputs.size(); ++p)
      if (f.inputs[p].type == "function")
        text += "{auto child=c;child.seed^=" +
                std::to_string((i + 1) * 7919 + (p + 1) * 104729) +
                "ULL; dependency" + std::to_string(p) + ".prepare(child);}\n";
    if (f.hasPrepare)
      text += ns + "prepare(state,c);\n";
    text += "}\nvoid reset(){\n";
    for (unsigned p = 0; p < f.inputs.size(); ++p)
      if (f.inputs[p].type == "function")
        text += "dependency" + std::to_string(p) + ".reset();\n";
    if (f.hasReset)
      text += ns + "reset(state);\n";
    text += "}\nusing Result = vlt::FunctionTraits<decltype(" + ns + f.entry +
            ")>::Result;\nResult call(const vlt::Context &ctx";
    for (unsigned p = 0; p < f.inputs.size(); ++p)
      if (f.inputs[p].type != "function")
        text += "," + type(f.inputs[p]) + " arg" + std::to_string(p);
    text += ") {\n";
    for (unsigned p = 0; p < f.inputs.size(); ++p)
      if (f.inputs[p].type == "function") {
        const auto child = bindings.at({i, f.inputs[p].id});
        const auto &cf = nodes[child].f;
        const auto pos =
            std::find(f.arguments.begin(), f.arguments.end(), f.inputs[p].id) -
            f.arguments.begin();
        const auto alias = "Fn" + std::to_string(p);
        text += "using " + alias + " = vlt::FunctionArgument<" +
                std::to_string(pos) + ",decltype(" + ns + f.entry +
                ")>::Type;\n";
        text += alias + " arg" + std::to_string(p) + "(&dependency" +
                std::to_string(p) +
                ",&ctx, +[](void *ptr,const vlt::Context &context";
        for (unsigned a = 0; a < cf.inputs.size(); ++a)
          if (cf.inputs[a].type != "function")
            text += "," + type(cf.inputs[a]) + " v" + std::to_string(a);
        text += ") -> " + alias + "::Result {auto result=static_cast<Root" +
                std::to_string(child) + " *>(ptr)->call(context";
        for (unsigned a = 0; a < cf.inputs.size(); ++a)
          if (cf.inputs[a].type != "function")
            text += ",v" + std::to_string(a);
        text += ");return ";
        if (!cf.aggregateReturn)
          text += "result";
        else {
          text += "{";
          for (unsigned o = 0; o < cf.outputs.size(); ++o) {
            if (o)
              text += ",";
            text += "result." + cf.outputs[o].name;
          }
          text += "}";
        }
        text += ";});\n";
      }
    text += "return " + ns + f.entry + "(";
    for (unsigned a = 0; a < f.arguments.size(); ++a) {
      if (a)
        text += ",";
      if (f.arguments[a] == "$state")
        text += "state";
      else if (f.arguments[a] == "$context")
        text += "ctx";
      else {
        auto p =
            std::find_if(f.inputs.begin(), f.inputs.end(),
                         [&](const auto &p) { return p.id == f.arguments[a]; });
        if (p == f.inputs.end())
          throw std::runtime_error("Invalid function argument mapping");
        text += "arg" + std::to_string(p - f.inputs.begin());
      }
    }
    text += ");}\n};\n";
    if (nodes[i].scheduled)
      text += root + " root" + std::to_string(i) + ";\n";
  }
  text += R"CPP(
struct Slot { unsigned inputs[16]{}, outputs[16]{}, latency=0, tail=0; };
Slot slots[64]; vlt::Context context;
extern "C" unsigned char __heap_base;
extern "C" void __wasm_call_ctors();
extern "C" unsigned vlt_heap_base(){return reinterpret_cast<unsigned>(&__heap_base);}
extern "C" unsigned vlt_abi(){return 1;}
extern "C" unsigned vlt_table(){return reinterpret_cast<unsigned>(slots);}
extern "C" void vlt_init(double rate,unsigned channels,unsigned frames,unsigned long long seed){
  __wasm_call_ctors();context.sampleRate=rate;context.channels=channels;context.seed=seed;
)CPP";
  for (unsigned i = 0; i < nodes.size(); ++i)
    if (nodes[i].scheduled) {
      const auto &n = nodes[i];
      auto s = "slots[" + std::to_string(n.index) + "]";
      text += "{vlt::PrepareContext "
              "c;c.sampleRate=rate;c.channels=channels;c.maxBlockSize=frames;c."
              "seed=seed^" +
              std::to_string(i + 1) + "ULL;root" + std::to_string(i) +
              ".prepare(c);" + s + ".latency=c.latency;" + s +
              ".tail=c.tail;}\n";
      for (unsigned p = 0; p < n.f.inputs.size(); ++p)
        if (n.f.inputs[p].type != "function")
          text += s + ".inputs[" + std::to_string(p) +
                  "]=vlt::detail::reserve(frames*8,4);\n";
      for (unsigned p = 0; p < n.f.outputs.size(); ++p)
        text += s + ".outputs[" + std::to_string(p) +
                "]=vlt::detail::reserve(frames*8,4);\n";
    }
  text += "}\nextern \"C\" void vlt_reset(){\n";
  for (unsigned i = 0; i < nodes.size(); ++i)
    if (nodes[i].scheduled)
      text += "root" + std::to_string(i) + ".reset();\n";
  text += R"CPP(}
extern "C" void vlt_process(unsigned node,unsigned frames,double beat,double tempo,long long sample,unsigned playing){
  context.tempo=tempo;context.playing=playing!=0;
  switch(node){
)CPP";
  for (unsigned i = 0; i < nodes.size(); ++i)
    if (nodes[i].scheduled) {
      const auto &n = nodes[i];
      const auto s = "slots[" + std::to_string(n.index) + "]";
      text += "case " + std::to_string(n.index) +
              ": for(unsigned "
              "frame=0;frame<frames;++frame){context.sampleTime=sample+frame;"
              "context.beat=beat+double(frame)*tempo/"
              "(60*context.sampleRate);auto result=root" +
              std::to_string(i) + ".call(context";
      for (unsigned p = 0; p < n.f.inputs.size(); ++p) {
        const auto &port = n.f.inputs[p];
        if (port.type == "function")
          continue;
        const auto ptr = "reinterpret_cast<float *>(" + s + ".inputs[" +
                         std::to_string(p) + "])";
        if (port.type == "audio")
          text +=
              ",vlt::AudioFrame{" + ptr + "[frame*2]," + ptr + "[frame*2+1]}";
        else
          text += "," + ptr + "[frame*2]" + (port.type == "gate" ? "!=0" : "");
      }
      text += ");\n";
      for (unsigned p = 0; p < n.f.outputs.size(); ++p) {
        const auto &port = n.f.outputs[p];
        const auto value =
            n.f.aggregateReturn ? "result." + port.name : "result";
        const auto ptr = "reinterpret_cast<float *>(" + s + ".outputs[" +
                         std::to_string(p) + "])";
        text += ptr + "[frame*2]=" + value +
                (port.type == "audio" ? ".left" : "") + ";";
        text += ptr + "[frame*2+1]=" + value +
                (port.type == "audio" ? ".right" : "") + ";\n";
      }
      text += "} break;\n";
    }
  text += "default: __builtin_trap();}}\n";
  return text;
}
} // namespace
json compile(const json &request, const std::filesystem::path &tools,
             const std::filesystem::path &work) {
  json diagnostics = json::array();
  try {
    const auto source = sourceUnit(request, tools, diagnostics);
    if (!writeCreatorText(work / "module.cpp", source))
      throw std::runtime_error("Cannot write generated source");
    emitObject(source, tools, work / "module.o", diagnostics);
    std::vector<std::string> args = {
        "--no-entry",
        "--export=vlt_heap_base",
        "--export=vlt_abi",
        "--export=vlt_table",
        "--export=vlt_init",
        "--export=vlt_process",
        "--export=vlt_reset",
        "--export-if-defined=__stack_pointer",
        "--initial-memory=262144",
        "--max-memory=16777216",
        "-z",
        "stack-size=65536",
        "--wrap=malloc",
        "--wrap=calloc",
        "--wrap=realloc",
        "--wrap=free",
        "-L" + pathUtf8(tools / "sysroot/lib/wasm32-wasip1"),
        pathUtf8(work / "module.o"),
        "-lc++",
        "-lc++abi",
        "-lc",
        "-lm",
        "-o",
        pathUtf8(work / "module.wasm")};
    std::string error;
    if (!runCreatorProcess(tools / exe("wasm-ld"), args, work / "clang.log",
                           90000, nullptr, error, false))
      throw std::runtime_error(error);
    const auto bytes =
        readCreatorText(work / "module.wasm", kMaxCodeArtifactBytes);
    if (bytes.empty())
      throw std::runtime_error("Compiler produced no WebAssembly");
    // Validate/instrument now as well as on receipt; only uninstrumented Wasm
    // travels with the module, and received binaries always get checked
    // locally.
    instrument(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    return {{"ok", true},
            {"wasm", encodeCode(std::span(
                         reinterpret_cast<const std::uint8_t *>(bytes.data()),
                         bytes.size()))},
            {"diagnostics", diagnostics}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"error", e.what()}, {"diagnostics", diagnostics}};
  }
}
json prepare(const json &request, const std::filesystem::path &tools,
             const std::filesystem::path &work) {
  try {
    auto bytes = decodeCode(request.at("wasm").get<std::string>());
    if (bytes.empty())
      throw std::runtime_error("Invalid WebAssembly encoding");
    const auto compilerHash =
        codeHash(readCreatorText(tools / exe("wamrc"), 256 * 1024 * 1024));
#if defined(__aarch64__) || defined(_M_ARM64)
    // AArch64 has no LLVM medium code model.
    const std::string codeModel = "--size-level=3";
#else
    const std::string codeModel = "--size-level=1";
#endif
    const auto key = codeHash(
        std::string(reinterpret_cast<const char *>(bytes.data()),
                    bytes.size()) +
        "creator-abi1-budget3-sdk1-wamr2.4.5-o2-" + codeModel + compilerHash +
#ifdef _WIN32
        "windows-x64-xip"
#elif defined(__APPLE__) && defined(__aarch64__)
        "macos-arm64"
#elif defined(__APPLE__)
        "macos-x64"
#elif defined(__aarch64__) || defined(_M_ARM64)
        "arm64"
#else
        "unix-x64"
#endif
    );
    // A private local cache; AOT is never read from a portable .vltmini
    // payload.
    auto cache = std::filesystem::temp_directory_path();
#ifdef _WIN32
    if (const auto *local = _wgetenv(L"LOCALAPPDATA"))
      cache = std::filesystem::path(local);
#else
    if (const auto *home = std::getenv("HOME"))
      cache = fromUtf8(home) /
#ifdef __APPLE__
              "Library/Caches";
#else
              ".cache";
#endif
#endif
    cache /= "VLTONE/Creator/AOT-v1";
    std::filesystem::create_directories(cache);
#ifndef _WIN32
    std::filesystem::permissions(cache, std::filesystem::perms::owner_all);
#endif
    const auto target = cache / (key + ".aot");
    const auto checksum = cache / (key + ".sha256");
    auto cached = readCreatorText(target, kMaxCodeArtifactBytes * 4);
    auto digest = readCreatorText(checksum, 128);
    if (cached.empty() || digest != codeHash(cached)) {
      auto metered = instrument(std::move(bytes));
      if (!writeCreatorText(
              work / "metered.wasm",
              std::string(reinterpret_cast<const char *>(metered.data()),
                          metered.size())))
        throw std::runtime_error("Cannot write prepared WebAssembly");
      std::string error;
      std::vector<std::string> args{"--bounds-checks=1",
                                    "--stack-bounds-checks=1", "--opt-level=2",
                                    codeModel};
#ifdef _WIN32
      // Direct COFF code can contain absolute 32-bit math-library constant
      // addresses even with the medium model. XIP routes these through WAMR's
      // symbol table; the Windows host supplies a dedicated executable mapping.
      args.push_back("--xip");
#endif
      args.insert(args.end(), {"-o", pathUtf8(work / "module.aot"),
                               pathUtf8(work / "metered.wasm")});
      if (!runCreatorProcess(tools / exe("wamrc"), args, work / "wamrc.log",
                             90000, nullptr, error, false))
        throw std::runtime_error(error);
      cached = readCreatorText(work / "module.aot", kMaxCodeArtifactBytes * 4);
      if (cached.empty())
        throw std::runtime_error("AOT compiler produced no code");
      digest = codeHash(cached);
      const auto pending =
          cache / (key + "." + pathUtf8(work.filename()) + ".pending");
      const auto pendingDigest =
          cache / (key + "." + pathUtf8(work.filename()) + ".digest");
      if (!writeCreatorText(pending, cached) ||
          !writeCreatorText(pendingDigest, digest))
        throw std::runtime_error("Cannot write local code cache");
      replaceCacheFile(pending, target);
      replaceCacheFile(pendingDigest, checksum);
    }
    return {{"ok", true}, {"aot", pathUtf8(target)}, {"digest", digest}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"error", e.what()}};
  }
}
} // namespace creator
