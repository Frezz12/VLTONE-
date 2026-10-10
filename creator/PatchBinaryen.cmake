# The pinned Binaryen declares Pass after PassRunner. libc++ instantiates
# unique_ptr<Pass> destruction while compiling inline constructors and add().
# Keep those definitions after Pass is complete, without changing behavior.
set(_pass_header "${creator_binaryen_SOURCE_DIR}/src/pass.h")
file(READ "${_pass_header}" _pass_text)
if(NOT _pass_text MATCHES "VLTONE: complete Pass before destroying its owners")
  if(NOT _pass_text MATCHES "PassRunner\\(Module\\* wasm\\) : wasm\\(wasm\\)")
    message(FATAL_ERROR "Pinned Binaryen PassRunner layout changed; review libc++ patch")
  endif()
  string(REPLACE
    "PassRunner(Module* wasm) : wasm(wasm), allocator(&wasm->allocator) {}"
    "PassRunner(Module* wasm);" _pass_text "${_pass_text}")
  string(REPLACE
    "PassRunner(Module* wasm, PassOptions options)\n    : wasm(wasm), allocator(&wasm->allocator), options(options) {}"
    "PassRunner(Module* wasm, PassOptions options);" _pass_text "${_pass_text}")
  string(REPLACE "virtual ~PassRunner() = default;" "virtual ~PassRunner();"
    _pass_text "${_pass_text}")
  string(REPLACE
    "explicit PassRunner(const PassRunner* runner)\n    : wasm(runner->wasm), allocator(runner->allocator),\n      options(runner->options), isNested(true) {}"
    "explicit PassRunner(const PassRunner* runner);" _pass_text "${_pass_text}")
  string(REPLACE "void add(std::unique_ptr<Pass> pass) { doAdd(std::move(pass)); }"
    "void add(std::unique_ptr<Pass> pass);" _pass_text "${_pass_text}")
  set(_pass_definitions [=[
// VLTONE: complete Pass before destroying its owners.
inline PassRunner::PassRunner(Module* wasm)
  : wasm(wasm), allocator(&wasm->allocator) {}
inline PassRunner::PassRunner(Module* wasm, PassOptions options)
  : wasm(wasm), allocator(&wasm->allocator), options(options) {}
inline PassRunner::PassRunner(const PassRunner* runner)
  : wasm(runner->wasm), allocator(runner->allocator),
    options(runner->options), isNested(true) {}
inline PassRunner::~PassRunner() = default;
inline void PassRunner::add(std::unique_ptr<Pass> pass) { doAdd(std::move(pass)); }

]=])
  string(REPLACE "} // namespace wasm"
    "${_pass_definitions}} // namespace wasm" _pass_text "${_pass_text}")
  file(WRITE "${_pass_header}" "${_pass_text}")
endif()
