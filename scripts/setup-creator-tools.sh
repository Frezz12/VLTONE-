#!/usr/bin/env bash
# Developer bootstrap for macOS (Intel / Apple Silicon) and Linux. End users
# receive CreatorTools inside the application and do not run this script.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
destination="${1:-$root/.cache/creator-tools}"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
mkdir -p "$destination"
destination="$(cd "$destination" && pwd)"

download() {
    local name="$1" url="$2" digest="$3"
    if [[ ! -f "$destination/$name" ]]; then
        curl -L --fail --retry 3 -o "$destination/$name" "$url"
    fi
    local actual
    actual="$(shasum -a 256 "$destination/$name")"
    [[ "${actual%% *}" == "$digest" ]] || { echo "Checksum mismatch: $name" >&2; exit 1; }
}

if [[ ! -f "$destination/llvm-20.1.8/lib/cmake/clang/ClangConfig.cmake" ]]; then
    download llvm-source.tar.xz \
      https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/llvm-project-20.1.8.src.tar.xz \
      6898f963c8e938981e6c4a302e83ec5beb4630147c7311183cf61069af16333d
    tar -xf "$destination/llvm-source.tar.xz" -C "$destination"
    cmake -S "$destination/llvm-project-20.1.8.src/llvm" -B "$destination/llvm-build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$destination/llvm-20.1.8" \
      '-DLLVM_ENABLE_PROJECTS=clang;lld' '-DLLVM_TARGETS_TO_BUILD=Native;WebAssembly' \
      -DLLVM_ENABLE_RTTI=ON -DLLVM_BUILD_LLVM_DYLIB=OFF -DLLVM_LINK_LLVM_DYLIB=OFF \
      -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_LIBEDIT=OFF \
      -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF
    cmake --build "$destination/llvm-build" --parallel "$jobs"
    cmake --install "$destination/llvm-build"
fi

if [[ ! -x "$destination/wamrc/wamrc" ]]; then
    download wamr-source.tar.gz \
      https://github.com/wasm-micro-runtime/wasm-micro-runtime/archive/25bd7eb63e828e4bd242cc9b38d260b4b31c6605.tar.gz \
      4ea552d9d979f3c3d5f9e298a1594150820d2b76e8328ad68779578edea7a873
    tar -xf "$destination/wamr-source.tar.gz" -C "$destination"
    cmake -S "$destination/wasm-micro-runtime-25bd7eb63e828e4bd242cc9b38d260b4b31c6605/wamr-compiler" \
      -B "$destination/wamrc-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_DIR="$destination/llvm-20.1.8/lib/cmake/llvm" -DWAMR_BUILD_SIMD=1
    cmake --build "$destination/wamrc-build" --parallel "$jobs"
    mkdir -p "$destination/wamrc"
    cp "$destination/wamrc-build/wamrc" "$destination/wamrc/wamrc"
fi

if [[ ! -f "$destination/wasi-sysroot-27.0/include/c++/v1/array" ]]; then
    download wasi-sysroot.tar.gz \
      https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sysroot-27.0.tar.gz \
      7110ac48f5d0b1f6ab67d57aecf52450540dddd790cafdc45f0fdfb429bdab84
    tar -xf "$destination/wasi-sysroot.tar.gz" -C "$destination"
fi
printf 'Creator tools ready: %s\n' "$destination"
