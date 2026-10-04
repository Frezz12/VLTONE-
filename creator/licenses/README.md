Creator tools and runtime use the following open-source components:

- Clang / LLVM 20.1.8, libc++ and libc++abi: Apache 2.0 with LLVM exceptions (`LLVM.txt`).
- WebAssembly Micro Runtime 2.4.5: Apache 2.0 with LLVM exceptions (`WAMR.txt`, copied from the pinned source during packaging).
- Binaryen 123: Apache 2.0 (`Binaryen.txt`, copied from the pinned source during packaging).
- WASI libc from WASI SDK 27, commit `3f7eb4c7d6ede4dde3c4bffa6ed14e8d656fe93f`: component notices in `WASI-libc.txt`.
- OpenSSL 3: Apache 2.0 (`OpenSSL.txt`).

The pinned source URLs and checksums are in `creator/CMakeLists.txt` and
`scripts/setup-creator-tools.{ps1,sh}`. These notices ship with CreatorTools.
