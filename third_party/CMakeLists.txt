# Vendored third-party SDKs.
#
# Each is exposed as an INTERFACE target so consumers get the include paths and
# nothing else. Only the plugin host may link these — keeping them out of
# daw_engine is what lets that library keep its "standard library only" promise.

# ── CLAP ──
# Header-only and MIT, so the headers are the whole SDK. See clap/README.md for
# the pinned version and how to move it.
add_library(daw_clap_headers INTERFACE)
target_include_directories(daw_clap_headers
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clap/include)

# ── Legacy VST 1.x/2.x (FST compatibility headers) ──
# FST is a clean-room, GPL-3.0-or-later description of the withdrawn AEffect
# ABI.  The two compatibility include names both pull in fst.h, so all three
# headers are the indivisible minimum.  See fst/README.md for the pinned source.
add_library(daw_fst_headers INTERFACE)
target_include_directories(daw_fst_headers SYSTEM
    INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/fst)
target_sources(daw_fst_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/fst/aeffect.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fst/aeffectx.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fst/fst.h
    ${CMAKE_CURRENT_SOURCE_DIR}/fst/README.upstream.md
    ${CMAKE_CURRENT_SOURCE_DIR}/fst/LICENSE.txt)

# ── VST3 (pluginterfaces only) ──
# Not header-only: the `FUnknown` helpers have to be compiled somewhere, which
# is what these three files are. The SDK's fourth, `coreiids.cpp`, is
# deliberately left out — the interface IIDs are all defined in one place, in
# vst3_iids.cpp, and having both is duplicate symbols at link. That file is
# ours, not Steinberg's, which is why it sits beside the vendored tree rather
# than inside it; it lives in this target because both the host and the VST3
# test fixture need the IIDs and neither should link the other. Everything else in the SDK — `base`,
# `public_sdk`, `vstgui` — is plugin-side and is deliberately not vendored.
# See vst3/README.md; the 3.8.0 floor is a licence requirement, not a whim.
add_library(daw_vst3_pluginterfaces STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vst3_iids.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vst3/pluginterfaces/base/conststringtable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vst3/pluginterfaces/base/funknown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vst3/pluginterfaces/base/ustring.cpp)
# The SDK includes itself as <pluginterfaces/...>, so the parent directory is
# what goes on the include path.
target_include_directories(daw_vst3_pluginterfaces PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vst3)
target_compile_features(daw_vst3_pluginterfaces PUBLIC cxx_std_17)
# Third-party code we do not maintain: its warnings are noise in our build log.
if(MSVC)
    target_compile_options(daw_vst3_pluginterfaces PRIVATE /w)
else()
    target_compile_options(daw_vst3_pluginterfaces PRIVATE -w)
endif()

# ── nlohmann/json ──
# Not vendored: it ships with vcpkg on Windows and Homebrew on macOS. Wrapped in
# one target because three separate places now need it — the project format, the
# plugin cache and the scanner's wire protocol — and the discovery dance below
# should exist exactly once.
add_library(daw_json INTERFACE)
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(daw_json INTERFACE nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE nlohmann/json.hpp
        HINTS /opt/homebrew/include /usr/local/include)
    if(NOT NLOHMANN_JSON_INCLUDE)
        message(FATAL_ERROR "nlohmann/json.hpp not found (install nlohmann-json)")
    endif()
    target_include_directories(daw_json INTERFACE ${NLOHMANN_JSON_INCLUDE})
endif()
