#pragma once

#include <cstdio>

// Minimal portable logging. Replaces the macOS os_log dependency. These write
// to stderr and are intended for control-thread diagnostics only — never call
// them from the realtime audio callback.
//
// __VA_OPT__ (C++20) lets the macros accept a bare format string with no
// trailing arguments, portably across Clang, GCC and MSVC.
#define DAW_LOG_INFO(fmt, ...) \
    std::fprintf(stderr, "[INFO]  " fmt "\n" __VA_OPT__(,) __VA_ARGS__)

#define DAW_LOG_ERROR(fmt, ...) \
    std::fprintf(stderr, "[ERROR] " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
