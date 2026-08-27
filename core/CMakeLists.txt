# daw_core — the portable platform layer.
#
# What is left here after the engine rewrite: device I/O, file decoding,
# recording and the shared buffer/result types. All DSP, mixing, routing and
# scheduling now live in daw_engine, which depends on nothing but the STL.
#
# Device I/O runs on PortAudio (CoreAudio / WASAPI / ASIO / ALSA) and file
# decoding on libsndfile. Both are discovered via their CMake config packages
# first (how vcpkg ships them on Windows) and fall back to pkg-config (how
# Homebrew / system packages ship them on macOS and Linux).
file(GLOB_RECURSE DAW_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM DAW_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/KnownFolders.cpp")

add_library(daw_core STATIC ${DAW_CORE_SOURCES})

# Kept separate so the plugin scanner can use Unicode-safe Known Folders
# without linking the audio I/O and decoder dependencies from daw_core.
add_library(daw_platform_paths STATIC
    platform/KnownFolders.cpp)
target_include_directories(daw_platform_paths PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_platform_paths PUBLIC cxx_std_23)
if(WIN32)
    target_link_libraries(daw_platform_paths PUBLIC shell32 ole32)
endif()

target_include_directories(daw_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_core PUBLIC cxx_std_23)

# ── libsndfile ──
find_package(SndFile CONFIG QUIET)
if(SndFile_FOUND)
    set(DAW_SNDFILE_TARGET SndFile::sndfile)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    set(DAW_SNDFILE_TARGET PkgConfig::SNDFILE)
endif()

# ── PortAudio ──
find_package(portaudio CONFIG QUIET)
if(portaudio_FOUND AND TARGET portaudio)
    set(DAW_PORTAUDIO_TARGET portaudio)
elseif(portaudio_FOUND AND TARGET portaudio_static)
    set(DAW_PORTAUDIO_TARGET portaudio_static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
    set(DAW_PORTAUDIO_TARGET PkgConfig::PORTAUDIO)
endif()

target_link_libraries(daw_core PUBLIC
    daw_platform_paths
    ${DAW_SNDFILE_TARGET}
    ${DAW_PORTAUDIO_TARGET})

if(MSVC)
    target_compile_options(daw_core PRIVATE /W3)
else()
    target_compile_options(daw_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
