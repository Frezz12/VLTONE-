#include "Compiler.hpp"
#include <binaryen-c.h>
#include <ir/utils.h>
#include <memory>
#include <wasm-binary.h>
#include <wasm-builder.h>
#include <wasm-traversal.h>
#include <wasm-validator.h>
#include <wasm.h>

namespace creator {
namespace {
constexpr const char *fuelName = "__vlt_private_fuel";
constexpr const char *budgetExport = "__vlt_set_budget";
constexpr const char *prepareMemory = "__vlt_prepare_pages";
struct Meter final : wasm::PostWalker<Meter> {
  wasm::Builder builder;
  explicit Meter(wasm::Module &module) : builder(module) {}
  wasm::Expression *charge(unsigned count) {
    using namespace wasm;
    return builder.makeBlock(
        {builder.makeGlobalSet(
             fuelName, builder.makeBinary(
                           SubInt64, builder.makeGlobalGet(fuelName, Type::i64),
                           builder.makeConst(std::int64_t(count)))),
         builder.makeIf(
             builder.makeBinary(LtSInt64,
                                builder.makeGlobalGet(fuelName, Type::i64),
                                builder.makeConst(std::int64_t(0))),
             builder.makeUnreachable())});
  }
  void visitLoop(wasm::Loop *loop) {
    const auto cost = std::max(1u, wasm::Measurer::measure(loop->body));
    loop->body = builder.makeBlock({charge(cost), loop->body});
  }
  void visitFunction(wasm::Function *function) {
    if (function->imported())
      return;
    const auto cost = std::max(1u, wasm::Measurer::measure(function->body));
    function->body = builder.makeBlock({charge(cost), function->body});
  }
  void visitMemoryGrow(wasm::MemoryGrow *) {
    replaceCurrent(builder.makeUnreachable());
  }
  void visitTableGrow(wasm::TableGrow *) {
    replaceCurrent(builder.makeUnreachable());
  }
};
} // namespace
std::vector<std::uint8_t> instrument(std::vector<std::uint8_t> bytes) {
  using namespace wasm;
  if (bytes.size() < 8 || bytes.size() > kMaxCodeArtifactBytes)
    throw std::runtime_error("Invalid WebAssembly size");
  std::unique_ptr<Module> module(
      BinaryenModuleRead(reinterpret_cast<char *>(bytes.data()), bytes.size()));
  if (!module || !WasmValidator().validate(*module, WasmValidator::Globally |
                                                        WasmValidator::Quiet))
    throw std::runtime_error("Invalid WebAssembly module");
  if (module->start.is() || module->memories.size() != 1 ||
      module->tables.size() > 1 || !module->tags.empty() ||
      module->functions.size() > 4096)
    throw std::runtime_error("Unsupported WebAssembly resources");
  auto &memory = *module->memories.front();
  if (memory.imported() || memory.shared || memory.is64() || !memory.hasMax() ||
      memory.max > 256 || memory.initial > 256)
    throw std::runtime_error("C++ memory must be private and at most 16 MiB");
  for (const auto &global : module->globals)
    if (global->imported())
      throw std::runtime_error("Imported globals are not permitted");
  for (const auto &table : module->tables)
    if (table->imported() || table->initial > 4096 || table->max > 4096)
      throw std::runtime_error("Unsupported function table");
  for (const auto &function : module->functions)
    if (function->imported() &&
        (function->module != "vlt" || function->base != "reserve" ||
         function->getParams() != Type({Type::i32, Type::i32}) ||
         function->getResults() != Type::i32))
      throw std::runtime_error(
          "System calls and external imports are not permitted");
  const auto check = [&](const char *name, Type args, Type result) {
    auto *exported = module->getExportOrNull(name);
    auto *function =
        exported && exported->kind == ExternalKind::Function
            ? module->getFunctionOrNull(*exported->getInternalName())
            : nullptr;
    if (!function || function->getParams() != args ||
        function->getResults() != result)
      throw std::runtime_error(std::string("Invalid Creator ABI: ") + name);
  };
  check("vlt_abi", Type::none, Type::i32);
  check("vlt_heap_base", Type::none, Type::i32);
  check("vlt_table", Type::none, Type::i32);
  check("vlt_init", Type({Type::f64, Type::i32, Type::i32, Type::i64}),
        Type::none);
  check(
      "vlt_process",
      Type({Type::i32, Type::i32, Type::f64, Type::f64, Type::i64, Type::i32}),
      Type::none);
  check("vlt_reset", Type::none, Type::none);
  if (module->getGlobalOrNull(fuelName) ||
      module->getFunctionOrNull(budgetExport) ||
      module->getExportOrNull(budgetExport) ||
      module->getFunctionOrNull(prepareMemory) ||
      module->getExportOrNull(prepareMemory))
    throw std::runtime_error("Reserved Creator metering symbol");
  Builder builder(*module);
  module->addGlobal(Builder::makeGlobal(fuelName, Type::i64,
                                        builder.makeConst(std::int64_t(0)),
                                        Builder::Mutable));
  Meter meter(*module);
  for (auto &function : module->functions)
    if (!function->imported())
      meter.walkFunction(function.get());
  // Added after user code is linked. No user call or table entry can reference
  // this setter; only the host can refill the non-addressable Wasm global.
  module->addFunction(Builder::makeFunction(
      budgetExport, Signature(Type::i64, Type::none), {},
      builder.makeGlobalSet(fuelName, builder.makeLocalGet(0, Type::i64))));
  module->addExport(Builder::makeExport(budgetExport, Name(budgetExport),
                                        ExternalKind::Function));
  // WAMRC otherwise folds grow-free memory to a single fixed-size page. This
  // host-only export keeps preparation growth possible; all user grow sites
  // were replaced with traps above. No user table or call can reach this body.
  module->addFunction(Builder::makeFunction(
      prepareMemory, Signature(Type::i32, Type::i32), {},
      builder.makeMemoryGrow(builder.makeLocalGet(0, Type::i32), memory.name)));
  module->addExport(Builder::makeExport(prepareMemory, Name(prepareMemory),
                                        ExternalKind::Function));
  if (!WasmValidator().validate(*module,
                                WasmValidator::Globally | WasmValidator::Quiet))
    throw std::runtime_error("Invalid metered WebAssembly");
  BufferWithRandomAccess buffer;
  PassOptions options;
  WasmBinaryWriter writer(module.get(), buffer, options);
  writer.write();
  return {buffer.begin(), buffer.end()};
}
} // namespace creator
