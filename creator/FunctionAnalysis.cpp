#include "Compiler.hpp"
#include "Creator/CodeUtilities.hpp"
#include "Creator/CompilerProcess.hpp"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <set>

extern "C" void LLVMInitializeWebAssemblyTargetInfo();
extern "C" void LLVMInitializeWebAssemblyTarget();
extern "C" void LLVMInitializeWebAssemblyTargetMC();
extern "C" void LLVMInitializeWebAssemblyAsmPrinter();

namespace creator {
namespace {
class Diagnostics final : public clang::DiagnosticConsumer {
public:
  Diagnostics(Analysis &r, std::string node)
      : result(r), node(std::move(node)) {}
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &d) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, d);
    llvm::SmallString<256> text;
    d.FormatDiagnostic(text);
    unsigned line = 0, column = 0;
    std::string sourceNode = node;
    if (d.hasSourceManager() && d.getLocation().isValid()) {
      const auto loc = d.getSourceManager().getPresumedLoc(d.getLocation());
      if (loc.isValid()) {
        line = loc.getLine();
        column = loc.getColumn();
        const std::string file = loc.getFilename();
        if (node == "generated" && file.ends_with(".cpp") &&
            file != "creator-generated.cpp")
          sourceNode = file.substr(0, file.size() - 4);
      }
    }
    result.diagnostics.push_back(
        {{"node", sourceNode},
         {"line", line},
         {"column", column},
         {"message", text.str().str()},
         {"severity",
          level >= clang::DiagnosticsEngine::Error ? "error" : "warning"}});
    if (level >= clang::DiagnosticsEngine::Error && result.error.empty())
      result.error = text.str().str();
  }
  Analysis &result;
  std::string node;
};
class RestrictedFiles final : public llvm::vfs::ProxyFileSystem {
public:
  explicit RestrictedFiles(const std::filesystem::path &tools)
      : ProxyFileSystem(llvm::vfs::getRealFileSystem()) {
    for (const auto *dir : {"sdk", "sysroot", "lib/clang/20"})
      roots.push_back(std::filesystem::weakly_canonical(tools / dir));
  }
  bool allowed(llvm::Twine path) const {
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(fromUtf8(path.str()), error);
    if (error)
      return false;
    for (const auto &root : roots) {
      const auto relative = canonical.lexically_relative(root);
      if (!relative.empty() && !relative.is_absolute() &&
          *relative.begin() != "..")
        return true;
    }
    return false;
  }
  llvm::ErrorOr<llvm::vfs::Status> status(const llvm::Twine &p) override {
    if (!allowed(p))
      return std::make_error_code(std::errc::no_such_file_or_directory);
    return getUnderlyingFS().status(p);
  }
  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForRead(const llvm::Twine &p) override {
    if (!allowed(p))
      return std::make_error_code(std::errc::permission_denied);
    return getUnderlyingFS().openFileForRead(p);
  }

private:
  std::vector<std::filesystem::path> roots;
};
struct Edit {
  unsigned offset, length;
  std::string text;
};
std::string applyEdits(std::string text, std::vector<Edit> edits) {
  std::stable_sort(
      edits.begin(), edits.end(),
      [](const auto &a, const auto &b) { return a.offset > b.offset; });
  for (const auto &e : edits) {
    if (e.offset > text.size() || e.length > text.size() - e.offset)
      throw std::runtime_error("Invalid C++ source edit");
    text.replace(e.offset, e.length, e.text);
  }
  return text;
}
class Visitor final : public clang::RecursiveASTVisitor<Visitor> {
public:
  Visitor(clang::ASTContext &context, Analysis &r, bool annotate)
      : context(context), sm(context.getSourceManager()), result(r),
        original(r.function), annotate(annotate) {}
  bool VisitFunctionDecl(clang::FunctionDecl *f) {
    if (sm.isWrittenInMainFile(f->getLocation()) &&
        f->isThisDeclarationADefinition() &&
        !llvm::isa<clang::CXXMethodDecl>(f))
      functions.push_back(f);
    return true;
  }
  bool VisitVarDecl(clang::VarDecl *v) {
    if (sm.isWrittenInMainFile(v->getLocation()) && v->hasGlobalStorage() &&
        !v->getType().isConstQualified() && !v->isConstexpr())
      fail("Mutable global/static variables are not supported; put DSP memory "
           "in State",
           v->getLocation());
    return true;
  }
  void fail(const std::string &message, clang::SourceLocation location) {
    const auto loc = sm.getPresumedLoc(location);
    result.diagnostics.push_back(
        {{"message", message},
         {"severity", "error"},
         {"line", loc.isValid() ? loc.getLine() : 0},
         {"column", loc.isValid() ? loc.getColumn() : 0}});
    if (result.error.empty())
      result.error = message;
  }
  std::string kind(clang::QualType type) {
    type = type.getNonReferenceType().getUnqualifiedType();
    if (type->isSpecificBuiltinType(clang::BuiltinType::Float))
      return "number";
    if (type->isBooleanType())
      return "gate";
    if (const auto *r = type->getAsCXXRecordDecl();
        r && r->getQualifiedNameAsString() == "vlt::AudioFrame")
      return "audio";
    return {};
  }
  std::string functionSignature(clang::QualType type) {
    const auto *record = type->getAsCXXRecordDecl();
    const auto *special =
        llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
    if (!special ||
        special->getSpecializedTemplate()->getQualifiedNameAsString() !=
            "vlt::Function")
      return {};
    auto t = special->getTemplateArgs()[0].getAsType();
    const auto *function = t->getAs<clang::FunctionProtoType>();
    if (!function)
      return {};
    FunctionDefinition descriptor;
    auto returnType = function->getReturnType();
    if (auto k = kind(returnType); !k.empty())
      descriptor.outputs.push_back({"out", "out", k});
    else if (const auto *r = returnType->getAsCXXRecordDecl()) {
      descriptor.aggregateReturn = true;
      for (const auto *field : r->fields()) {
        auto k = kind(field->getType());
        if (k.empty())
          return {};
        descriptor.outputs.push_back(
            {field->getNameAsString(), field->getNameAsString(), k});
      }
    } else
      return {};
    for (auto arg : function->param_types()) {
      auto k = kind(arg);
      if (k.empty())
        return {};
      descriptor.inputs.push_back({"", "", k});
    }
    return callableSignature(descriptor);
  }
  FunctionPort port(clang::ValueDecl *v, bool output) {
    FunctionPort p;
    p.name = v->getNameAsString();
    p.cppType = v->getType().getUnqualifiedType().getAsString();
    p.type = kind(v->getType());
    p.signature = functionSignature(v->getType());
    if (!p.signature.empty())
      p.type = "function";
    for (auto *attr : v->specific_attrs<clang::AnnotateAttr>())
      if (attr->getAnnotation().starts_with("vlt.port:"))
        p.id = attr->getAnnotation().drop_front(9).str();
    if (p.id.empty()) {
      p.id = "p_" + codeHash(result.function.entry + ":" + p.name + ":" +
                             (output ? "out" : "in"))
                        .substr(0, 20);
      if (annotate)
        edits.push_back({sm.getFileOffset(sm.getExpansionLoc(v->getBeginLoc())),
                         0, "VLT_PORT(\"" + p.id + "\") "});
    }
    const auto &old = output ? original.outputs : original.inputs;
    for (const auto &o : old)
      if (o.id == p.id) {
        p.initial = o.initial;
        p.minimum = o.minimum;
        p.maximum = o.maximum;
      }
    if (auto *param = llvm::dyn_cast<clang::ParmVarDecl>(v);
        param && param->hasDefaultArg()) {
      clang::Expr::EvalResult evaluated;
      if (param->getDefaultArg()->EvaluateAsRValue(evaluated, context)) {
        if (evaluated.Val.isFloat())
          p.initial = evaluated.Val.getFloat().convertToDouble();
        else if (evaluated.Val.isInt())
          p.initial = double(evaluated.Val.getInt().getSExtValue());
      }
    }
    if (p.type == "gate") {
      p.minimum = 0;
      p.maximum = 1;
      p.initial = p.initial != 0 ? 1 : 0;
    }
    if (!isCppIdentifier(p.name) || p.type.empty() || p.id.empty() ||
        p.id == "function" || p.id.front() == '$')
      fail("Ports support float, bool, vlt::AudioFrame and "
           "vlt::Function<Signature>",
           v->getLocation());
    return p;
  }
  void finish() {
    clang::FunctionDecl *entry = nullptr;
    json details = json::array();
    for (auto *f : functions) {
      if (f->getNameAsString() == original.entry) {
        if (entry)
          fail("The entry function cannot be overloaded", f->getLocation());
        entry = f;
      }
      const auto begin = sm.getFileOffset(sm.getExpansionLoc(f->getBeginLoc()));
      const auto end = sm.getFileOffset(clang::Lexer::getLocForEndOfToken(
          sm.getExpansionLoc(f->getEndLoc()), 0, sm, context.getLangOpts()));
      details.push_back({{"name", f->getNameAsString()},
                         {"begin", begin},
                         {"end", end},
                         {"line", sm.getSpellingLineNumber(f->getLocation())},
                         {"signature", f->getType().getAsString()}});
    }
    result.details["functions"] = details;
    if (!entry) {
      result.error = "Entry function not found: " + original.entry;
      return;
    }
    if (entry->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate ||
        !entry->getDeclContext()->isTranslationUnit())
      fail("The entry must be a non-template free function",
           entry->getLocation());
    auto &f = result.function;
    f.inputs.clear();
    f.outputs.clear();
    f.arguments.clear();
    f.stateType.clear();
    f.hasContext = f.hasPrepare = f.hasReset = false;
    f.aggregateReturn = false;
    for (auto *p : entry->parameters()) {
      const auto type = p->getType().getNonReferenceType().getUnqualifiedType();
      const auto *record = type->getAsCXXRecordDecl();
      if (record && record->getQualifiedNameAsString() == "vlt::Context") {
        if (f.hasContext)
          fail("Only one Context argument is supported", p->getLocation());
        f.hasContext = true;
        f.arguments.push_back("$context");
      } else if (p->getType()->isLValueReferenceType() && record &&
                 kind(type).empty() && functionSignature(type).empty()) {
        if (!f.stateType.empty())
          fail("Only one State argument is supported", p->getLocation());
        if (!record->isTriviallyCopyable() || !record->hasTrivialDestructor())
          fail("State must have a trivial destructor; use vlt::Buffer for "
               "prepared storage",
               p->getLocation());
        f.stateType = type.getAsString();
        f.arguments.push_back("$state");
      } else {
        if (p->getType()->isReferenceType() || p->getType()->isPointerType())
          fail("Signal ports are passed by value", p->getLocation());
        f.inputs.push_back(port(p, false));
        f.arguments.push_back(f.inputs.back().id);
      }
    }
    f.returnType = entry->getReturnType().getAsString();
    if (auto k = kind(entry->getReturnType()); !k.empty())
      f.outputs.push_back({"out", "Output", k, f.returnType});
    else if (const auto *record =
                 entry->getReturnType()->getAsCXXRecordDecl()) {
      f.aggregateReturn = true;
      if (!record->isAggregate())
        fail("Multiple outputs must be an aggregate struct",
             entry->getLocation());
      for (auto *field : record->fields())
        f.outputs.push_back(port(field, true));
    } else
      fail("Return float, bool, AudioFrame or a struct of named outputs",
           entry->getLocation());
    for (auto *other : functions) {
      if (other->getNameAsString() == "prepare" && other != entry)
        f.hasPrepare = true;
      if (other->getNameAsString() == "reset" && other != entry)
        f.hasReset = true;
    }
    if ((f.hasPrepare || f.hasReset) && f.stateType.empty())
      fail("prepare/reset require a State argument", entry->getLocation());
    if (f.inputs.size() > kMaxFunctionPorts || f.outputs.empty() ||
        f.outputs.size() > kMaxFunctionPorts)
      fail("A function supports 1–16 outputs and up to 16 inputs",
           entry->getLocation());
    std::set<std::string> ids;
    for (const auto &p : f.inputs)
      if (!ids.insert(p.id).second)
        fail("Duplicate input port ID", entry->getLocation());
    ids.clear();
    for (const auto &p : f.outputs)
      if (!ids.insert(p.id).second)
        fail("Duplicate output port ID", entry->getLocation());
    if (result.error.empty()) {
      f.source = applyEdits(f.source, edits);
      f.analyzedHash = functionSourceHash(f);
    }
  }
  clang::ASTContext &context;
  clang::SourceManager &sm;
  Analysis &result;
  FunctionDefinition original;
  bool annotate;
  std::vector<clang::FunctionDecl *> functions;
  std::vector<Edit> edits;
};
} // namespace
std::vector<std::string> compilerFlags(const std::filesystem::path &tools) {
  return {"-std=c++23",
          "--target=wasm32-wasip1",
          "--sysroot=" + pathUtf8(tools / "sysroot"),
          "-resource-dir=" + pathUtf8(tools / "lib/clang/20"),
          "-I" + pathUtf8(tools / "sdk"),
          "-O2",
          "-fno-exceptions",
          "-fno-rtti",
          "-fno-threadsafe-statics",
          "-ffp-contract=off",
          "-Wno-unknown-attributes",
          "-Wno-unused-parameter",
          "-fno-color-diagnostics"};
}
Analysis analyze(FunctionDefinition f, const std::filesystem::path &tools,
                 const std::string &node, bool annotate) {
  Analysis result;
  result.function = std::move(f);
  if (result.function.source.size() > kMaxFunctionSourceBytes ||
      !isCppIdentifier(result.function.entry)) {
    result.error = "Invalid function name or source size";
    return result;
  }
  Diagnostics diagnostics(result, node);
  auto fs = llvm::makeIntrusiveRefCnt<RestrictedFiles>(tools);
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      result.function.source, compilerFlags(tools), node + ".cpp",
      pathUtf8(tools / "clang"),
      std::make_shared<clang::PCHContainerOperations>(),
      clang::tooling::getClangStripDependencyFileAdjuster(), {}, &diagnostics,
      fs);
  if (!ast || !result.error.empty()) {
    if (result.error.empty())
      result.error = "C++ analysis failed";
    return result;
  }
  Visitor visitor(ast->getASTContext(), result, annotate);
  visitor.TraverseDecl(ast->getASTContext().getTranslationUnitDecl());
  visitor.finish();
  return result;
}
void emitObject(const std::string &source, const std::filesystem::path &tools,
                const std::filesystem::path &output, json &diagnostics) {
  LLVMInitializeWebAssemblyTargetInfo();
  LLVMInitializeWebAssemblyTarget();
  LLVMInitializeWebAssemblyTargetMC();
  LLVMInitializeWebAssemblyAsmPrinter();
  Analysis result;
  class Action final : public clang::EmitObjAction {
  public:
    Action(Analysis &result, std::string output)
        : diagnostics(result, "generated"), output(std::move(output)) {}
    bool BeginInvocation(clang::CompilerInstance &instance) override {
      instance.getDiagnostics().setClient(&diagnostics, false);
      instance.getFrontendOpts().OutputFile = output;
      return true;
    }
    Diagnostics diagnostics;
    std::string output;
  };
  auto args = compilerFlags(tools);
  args.push_back("-o");
  args.push_back(pathUtf8(output));
  auto files = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(
      llvm::makeIntrusiveRefCnt<RestrictedFiles>(tools));
  auto memory = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  const auto filename = pathUtf8(tools / "creator-module.cpp");
  memory->addFile(filename, 0, llvm::MemoryBuffer::getMemBufferCopy(source));
  files->pushOverlay(memory);
  const bool ok = clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<Action>(result, pathUtf8(output)), source, files, args,
      filename, pathUtf8(tools / "clang"));
  for (const auto &d : result.diagnostics)
    diagnostics.push_back(d);
  if (!ok)
    throw std::runtime_error(result.error.empty() ? "C++ code generation failed"
                                                  : result.error);
}
static json connectFunction(FunctionDefinition original,
                            const FunctionDefinition &child,
                            const std::filesystem::path &tools,
                            std::vector<Edit> edits = {}) {
  Analysis parsed;
  parsed.function = original;
  Diagnostics sink(parsed, "node");
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      original.source, compilerFlags(tools), "node.cpp",
      pathUtf8(tools / "clang"),
      std::make_shared<clang::PCHContainerOperations>(),
      clang::tooling::getClangStripDependencyFileAdjuster(), {}, &sink,
      llvm::makeIntrusiveRefCnt<RestrictedFiles>(tools));
  if (!ast || !parsed.error.empty())
    return {{"ok", false}, {"error", parsed.error}};
  clang::FunctionDecl *entry = nullptr;
  for (auto *d : ast->getASTContext().getTranslationUnitDecl()->decls())
    if (auto *f = llvm::dyn_cast<clang::FunctionDecl>(d);
        f && f->getNameAsString() == original.entry &&
        f->isThisDeclarationADefinition())
      entry = f;
  if (!entry)
    return {{"ok", false}, {"error", "Entry function not found"}};
  for (const auto *parameter : entry->parameters())
    if (parameter->getNameAsString() == child.entry)
      return {{"ok", false},
              {"error", "An input already has this function name"}};
  const auto portId =
      "p_" + codeHash(original.source + child.entry).substr(0, 20);
  const auto suffix = portId.substr(2, 12);
  const auto alias = "VltFunction_" + suffix;
  std::string declaration, returnType = child.outputs.front().cppType;
  if (child.aggregateReturn) {
    returnType = "VltResult_" + suffix;
    declaration = "struct " + returnType + " {\n";
    for (const auto &p : child.outputs)
      declaration += p.cppType + " " + p.name + ";\n";
    declaration += "};\n";
  }
  declaration += "using " + alias + " = vlt::Function<" + returnType + "(";
  bool first = true;
  for (const auto &p : child.inputs)
    if (p.type != "function") {
      if (!first)
        declaration += ",";
      first = false;
      declaration += p.cppType;
    }
  declaration += ")>;\n";
  auto &sm = ast->getSourceManager();
  const auto loc = entry->getTypeSourceInfo()
                       ->getTypeLoc()
                       .getAs<clang::FunctionProtoTypeLoc>();
  edits.push_back({sm.getFileOffset(sm.getExpansionLoc(loc.getRParenLoc())), 0,
                   (entry->getNumParams() ? ", " : "") +
                       std::string("VLT_PORT(\"") + portId + "\") " + alias +
                       " " + child.entry + " = {}"});
  edits.push_back({sm.getFileOffset(sm.getExpansionLoc(entry->getBeginLoc())),
                   0, declaration});
  original.source = applyEdits(original.source, std::move(edits));
  auto parent = analyze(original, tools, "node");
  if (!parent.error.empty())
    return {{"ok", false},
            {"error", parent.error},
            {"diagnostics", parent.diagnostics}};
  return {{"ok", true},
          {"parent", functionToJson(parent.function)},
          {"child", functionToJson(child)},
          {"port", portId}};
}
json bindFunction(const json &request, const std::filesystem::path &tools) {
  auto child =
      analyze(functionFromJson(request.at("child")), tools, "function");
  if (!child.error.empty())
    return {{"ok", false},
            {"error", child.error},
            {"diagnostics", child.diagnostics}};
  return connectFunction(functionFromJson(request.at("function")),
                         child.function, tools);
}
json extractFunction(const json &request, const std::filesystem::path &tools) {
  const auto original = functionFromJson(request.at("function"));
  auto parsed = analyze(original, tools, "node", false);
  if (!parsed.error.empty())
    return {{"ok", false},
            {"error", parsed.error},
            {"diagnostics", parsed.diagnostics}};
  const auto name = request.at("name").get<std::string>();
  if (name == original.entry || name == "prepare" || name == "reset")
    return {{"ok", false},
            {"error",
             "Choose a helper function, not the entry or lifecycle function"}};
  json chosen;
  for (const auto &f : parsed.details["functions"])
    if (f["name"] == name) {
      if (!chosen.is_null())
        return {{"ok", false},
                {"error", "Choose a helper with a unique name; overloaded "
                          "helpers cannot be extracted"}};
      chosen = f;
    }
  if (chosen.is_null())
    return {{"ok", false}, {"error", "Helper function not found"}};
  const unsigned begin = chosen.at("begin"), end = chosen.at("end");
  FunctionDefinition child;
  child.entry = name;
  child.source = "#include <vlt/creator.hpp>\n\n" +
                 original.source.substr(begin, end - begin) + "\n";
  auto childAnalysis = analyze(child, tools, "function");
  if (!childAnalysis.error.empty() || !childAnalysis.function.stateType.empty())
    return {{"ok", false},
            {"error",
             "This helper depends on local types, state or other declarations. "
             "Move those dependencies into the helper first."}};
  auto result = connectFunction(original, childAnalysis.function, tools,
                                {{begin, end - begin, ""}});
  if (!result.value("ok", false))
    result["error"] = "Cannot preserve this helper's dependencies: " +
                      result.value("error", std::string{});
  return result;
}
} // namespace creator
