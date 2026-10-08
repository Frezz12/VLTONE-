# C++ Function nodes — SDK 1

Full authoring reference: [Creator SDK in Russian](sdk/CREATOR_SDK_RU.md).
The [SDK kit](sdk/README.md) includes an AI writing contract, the public header,
13 source files and 12 connected example projects with compiled modules.

Open **View → Creator**, create a project, then add **Code → C++ Function**.
The dialog generates an entry function and named inputs/outputs. The code panel
under the canvas supports C++ highlighting, line numbers, Ctrl+F, Ctrl+Space SDK
completion, `.cpp` import/export and the Creator Undo/Redo history. Drag its
splitter to change the editor height. Double-click a diagnostic to open its line.

**Update node** checks the current source and updates the ports. It preserves
connections by `VLT_PORT("stable-id")`, including when parameters are renamed or
reordered. Keep these annotations when editing existing inputs and return fields.
Removed or incompatible ports leave visible broken wires in the draft. A failed
check keeps the last valid ports. Text changes do not rebuild the canvas.

**Compile** saves the source project, builds the module off the audio thread and
installs one `.vltmini` in `Documents/VLTONE/MiniModules`. Existing instances on
tracks and Master update together, keeping their IDs, controls, automation,
bypass and position around Audio FX. Changed DSP invalidates affected Freeze.
Cancelled, invalid or stale builds preserve the installed file and live DSP.
The `.vltcreator` project retains unfinished source, cursor locations and layouts.

## Signals and callable functions

An entry function is a non-overloaded, global C++23 free function. Signal ports
are passed by value: `vlt::AudioFrame` (mono/stereo audio), `float` (Number),
`bool` (Gate). Return one of these types, or an aggregate struct of named fields
to produce several outputs in one invocation. Up to 16 ordinary/function inputs
and 16 signal outputs are supported. A mini-module still exposes at most two
Interface controls; internal nodes can have more parameters.

`vlt::Function<R(Args...)>` is a callable input. Connect another function's amber
hexagonal output to it; the dashed wire checks the complete signal signature.
The caller supplies arguments in C++, for example `saturate(input, drive)`.
The callee's ordinary input wires affect only its separate signal-graph use.
Each callable binding has private state, including nested dependencies. Calls
through the same binding share that binding's state. Signal cycles and recursive
dependencies between function nodes are rejected independently.

**Create function** declares a callable input and connects a new function node.
Place the call at the desired point in your algorithm. **Extract function** moves
a uniquely named local helper into a node and rewrites the entry in one Undo.
Helpers that depend on local types/state or other undeclared helpers are refused
with an explanation; the original source is preserved. Such dependencies can
first be moved into a self-contained helper manually.

## State, time and prepared memory

Add one trivial `State &` argument for persistent state and an optional
`const vlt::Context &` for sample rate, sample time, beat, tempo, playing, channels
and deterministic seed. They are hidden from the port list. AudioFrame has left
and right samples; mono inputs are mirrored and the host keeps only the mono
output. Processing is one frame per call within a generated block wrapper,
preserving sample-accurate modulation. The host enters DSP once per node/block
and refills its budget separately, rather than crossing into the runtime per frame.

Optional lifecycle functions are `void prepare(State &, vlt::PrepareContext &)`
and `void reset(State &)`. Allocate `vlt::Buffer<T>` storage only in `prepare`.
Its indexed access checks bounds. Set `PrepareContext::latency` and `tail` in
samples for the complete entry algorithm. If an entry invokes a delayed helper,
the entry declares its own total latency; arbitrary C++ call order cannot be
inferred from callable wires. GraphProcessor compensates signal branches using
that declaration. `reset` restores prepared state and invokes the reset callback.

Use explicit state instead of mutable global/static variables. Native DLLs,
system APIs, files, sockets, user threads, exceptions and RTTI are unavailable.
General heap allocation traps; prepared storage is bounded to 16 MiB per module.
The portable code runs with software memory/stack checks and a Binaryen-inserted
budget at loop and function entries. Exceeding it, invalid memory, stack overflow
or non-finite audio faults the module. Diagnostics are transferred outside the
audio callback; offline export reports failure instead of publishing substitute
audio. AI, external-editor watching and MIDI are outside SDK 1.

## Portability and developer builds

`.vltmini` carries source, SDK version, graph and portable WebAssembly. Copy that
single file to another machine's MiniModules folder. A received native build is
never accepted: WAMR AOT is generated locally in the user's `VLTONE/Creator/AOT-v1`
cache. Its key includes Wasm, ABI/SDK, metering revision, compiler binary hash and
platform; cached bytes are checked against their checksum before loading.

Creator format is 3, mini-module format 5, DAW project format 16 and strip preset
format 6. Previous supported formats remain readable. Unknown SDK definitions
remain embedded as unavailable data rather than losing their source.

For code-free DSP see [the node guide](sdk/NODE_DSP_RU.md) and
[the generated registry](sdk/NODE_REFERENCE.json). The prepared elementary
runtime supports sample history, bounded numeric collections, typed subgraphs
and 2×/4× oversampling. Reusable `.vltnode` v1 files live in
`Documents/VLTONE/CreatorNodes`; projects embed independent copies of library
definitions. The four [teaching projects](sdk/examples/nodes) demonstrate a
compressor, equalizer, saturator and feedback delay.

Developers run `scripts/setup-creator-tools.ps1` on Windows or
`bash scripts/setup-creator-tools.sh` on macOS/Linux before configuring CMake.
The Unix bootstrap builds a pinned static LLVM 20.1.8 SDK and WAMR 2.4.5 compiler
for the host architecture; it needs CMake, Ninja and the platform development
tools. Windows downloads the checksum-pinned official tools. CMake packages
CreatorTools, SDK/sysroot, licenses and required runtime libraries with the app.
End users do not need Visual Studio, Xcode or a separate C++ compiler.

The service process has cancellation, memory/time limits and restricted include
access to the bundled SDK/sysroot. Compilation, cache access, allocation and
destruction are outside audio processing. The audio path uses preallocated
buffers and the existing sequential GraphProcessor.

Examples in `examples/`: Waveshaper, OnePole (stateful filter), and Composed
(saturation with an extracted callable helper). The `.cpp` files are editable
starting points; the accompanying `.vltcreator` files open as connected projects.

Validation targets: `creator_cpp_test`, `creator_cpp_integration_test`,
`creator_graph_test`, `creator_integration_test` and `VLTONE --creator-check`.
The UI check accepts `--theme`, `--language`, `QT_SCALE_FACTOR` and
`DAW_CREATOR_CHECK_DIR`. Cross-platform CI runs the same portable code checks;
a Windows-only local run does not verify execution on macOS.
