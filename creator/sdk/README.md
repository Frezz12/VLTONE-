# Creator SDK 1

Authoring kit for portable C++ mini-modules in VLTONE Creator.

- [Полное руководство на русском](CREATOR_SDK_RU.md): workflow, full API, state, typed connections, callable functions, modes, packaging and diagnostics. It embeds the SDK header and every example source for use as one AI context document.
- [Краткий контракт для нейросети](AI_GUIDE_RU.md): generation rules and a reusable prompt.
- [Public C++ header](vlt/creator.hpp): the actual header used by Creator.
- [Example sources](examples): 13 `.cpp` files, including callable helpers.
- [Connected projects and modules](examples/projects): 12 `.vltcreator` projects and matching `.vltmini` files.
- [Example verification](examples/projects/verification.json): recorded analysis, build and runtime results.

Open a `.vltcreator` in **View → Creator** to inspect and modify its complete graph. To install an already compiled example, copy its `.vltmini` to your system Documents folder under `VLTONE/MiniModules`. Importing a `.cpp` alone transfers only that node's source, not the other nodes or wires.

The app ships the compiler in `CreatorTools`; this documentation kit does not require a separate developer compiler. Files here target SDK/ABI 1, Creator format 2 and mini-module format 4. Runtime verification for these examples was performed on Windows at 48 kHz stereo. Audio quality and additional platforms require their own evaluation.

Repository maintainers can regenerate examples with Python's standard library:

```text
python creator/sdk/verify_examples.py --tools build-windows/stage/bin/CreatorTools --validator build-windows/bin/creator_graph_test.exe
```

The verifier writes example projects/modules and their report. It does not install them into the user's module library. Omit `--validator` for analysis/build only; use `--out` for a different output folder. The generated binaries are portable WebAssembly embedded in `.vltmini`; no external native builds are included.
