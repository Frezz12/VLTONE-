# VLT Studio Pro — cross-platform (Windows + macOS) build.
# Replaces the macOS-only Xcode/xcodegen project. The old Swift/ObjC++ app
# under DAW/ is not part of this build; it remains as a frozen reference until
# the Qt app reaches parity.
cmake_minimum_required(VERSION 3.24)

project(VLTStudioPro
    VERSION 0.1.4
    DESCRIPTION "VLT Studio Pro digital audio workstation"
    LANGUAGES CXX)

include(cmake/WindowsResources.cmake)

# vcpkg requires its own version field, but CMake remains the release version
# authority. Fail at configure time instead of publishing two differently
# numbered artifacts after one side was updated.
file(READ "${CMAKE_SOURCE_DIR}/vcpkg.json" DAW_VCPKG_MANIFEST)
string(JSON DAW_VCPKG_VERSION GET "${DAW_VCPKG_MANIFEST}" version)
if(NOT DAW_VCPKG_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Version mismatch: CMake=${PROJECT_VERSION}, vcpkg.json=${DAW_VCPKG_VERSION}")
endif()

add_compile_definitions(
    VLT_STUDIO_PRO_NAME="VLT Studio Pro"
    VLT_STUDIO_PRO_VERSION="${PROJECT_VERSION}")

# ── Global C++ settings ──
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(MSVC)
        # Source files and every std::string path in the application are UTF-8.
        # Make that contract explicit instead of inheriting the machine's ACP.
        add_compile_options(/utf-8 /Zc:preprocessor)
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Help find_package locate Homebrew-installed packages (Qt, nlohmann_json …)
# on Apple Silicon. On Windows the vcpkg toolchain file supplies these paths.
if(APPLE AND EXISTS /opt/homebrew)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
endif()

# Feature toggles — lets the core + tests build before the UI deps are wired in.
option(DAW_BUILD_APP   "Build the Qt front-end application" OFF)
option(DAW_BUILD_TESTS "Build the engine unit tests"        ON)
# Declared here rather than in app/: the scanner and the watchdog are installed
# *inside* the bundle when it is on, and they are configured before app/ is.
option(DAW_PACKAGE     "Build a deployable bundle (app bundle + Qt runtime)" OFF)
# Multiplayer foundations can be dogfooded independently of the production
# release gate. The second switch is intentionally stricter: it makes an
# incomplete command-migration ledger a compile error and must be enabled by
# release automation before collaboration is exposed to ordinary users.
option(DAW_ENABLE_COLLABORATION
       "Enable the experimental cloud collaboration UI and networking" OFF)
option(DAW_ENFORCE_COLLABORATION_RELEASE_GATES
       "Require complete shared-mutation command coverage" OFF)
if(DAW_ENFORCE_COLLABORATION_RELEASE_GATES AND
   NOT DAW_ENABLE_COLLABORATION)
    message(FATAL_ERROR
        "Collaboration release gates require DAW_ENABLE_COLLABORATION=ON")
endif()
if(DAW_ENFORCE_COLLABORATION_RELEASE_GATES)
    include(cmake/CheckCollaborationMutationCoverage.cmake)
endif()
# Development builds talk to a local API by default. Release automation sets
# this cache value to the HTTPS API endpoint; an explicit VLT_API_ORIGIN
# environment variable always takes precedence at runtime.
set(VLT_DEFAULT_API_ORIGIN "http://localhost:8080/v1" CACHE STRING
    "Default VLT account API origin compiled into the desktop application")

# The deployment target has to be the truth, not a wish: Homebrew builds its
# bottles for the machine you are on, so the libsndfile and PortAudio this links
# against already refuse to load on anything older. Claiming a lower minimum
# only turns "this app needs a newer macOS" into a crash on launch. Pass
# -DCMAKE_OSX_DEPLOYMENT_TARGET=... to override it — worth doing for a release
# build whose dependencies were themselves built for an older system.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    execute_process(COMMAND sw_vers -productVersion
                    OUTPUT_VARIABLE DAW_HOST_MACOS
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    string(REGEX MATCH "^[0-9]+" DAW_HOST_MACOS_MAJOR "${DAW_HOST_MACOS}")
    if(DAW_HOST_MACOS_MAJOR)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${DAW_HOST_MACOS_MAJOR}.0" CACHE STRING "" FORCE)
        message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET} (host)")
    endif()
endif()

enable_testing()

add_subdirectory(third_party)  # vendored SDKs, exposed as INTERFACE targets
add_subdirectory(engine)       # graph-based audio engine (C++23, no deps)
add_subdirectory(core)
add_subdirectory(plugins)      # VST3/CLAP/AU hosting; the only user of the SDKs
add_subdirectory(scanner)      # daw_scan: loads plugins out of process, so a crash is survivable
add_subdirectory(guard)        # daw_guard: watches the app from outside, so a hang is visible
add_subdirectory(controller)   # framework-agnostic; no Qt dependency

if(DAW_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(DAW_BUILD_APP)
    add_subdirectory(app)
endif()
