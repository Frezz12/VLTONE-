# daw_pluginhost — hosting for third-party plugin formats.
#
# Everything that talks to a plugin SDK lives here and nowhere else, which is
# what keeps daw_engine free of dependencies and daw_core free of plugins. The
# graph never learns what a plugin is: PluginNode is just another engine::Node.
#
# Sources are listed explicitly rather than globbed, unlike every other target
# in this project, and deliberately so. The format backends are chosen per
# platform, and the VST3 SDK's macOS module loader is an ObjC++ `.mm` file that
# a `*.cpp` glob would silently drop — producing a link error that points
# nowhere near the cause.

option(DAW_ENABLE_CLAP "Host CLAP plugins" ON)
option(DAW_ENABLE_VST3 "Host VST3 plugins" ON)
option(DAW_ENABLE_VST "Host legacy VST 1.x/2.x plugins" ON)
# Audio Units exist only on macOS; there is nothing to switch on elsewhere.
if(APPLE)
    option(DAW_ENABLE_AU "Host Audio Unit plugins" ON)
else()
    set(DAW_ENABLE_AU OFF)
endif()

set(DAW_PLUGINHOST_SOURCES
    Host/Registry.cpp
    Host/PluginNode.cpp
    Internal/InternalFactory.cpp
    Internal/SamplerInstance.cpp
    Internal/SamplerParams.cpp
    Internal/SamplerPrecompute.cpp
    Internal/SamplerVoice.cpp
    Scan/ScanProtocol.cpp)

if(DAW_ENABLE_CLAP)
    list(APPEND DAW_PLUGINHOST_SOURCES
        Clap/ClapModule.cpp
        Clap/ClapInstance.cpp
        Clap/ClapFactory.cpp)
endif()

if(DAW_ENABLE_VST3)
    list(APPEND DAW_PLUGINHOST_SOURCES
        Vst3/Vst3Module.cpp
        Vst3/Vst3Instance.cpp
        Vst3/Vst3Factory.cpp)
endif()

if(DAW_ENABLE_VST)
    list(APPEND DAW_PLUGINHOST_SOURCES
        Vst/VstModule.cpp
        Vst/VstInstance.cpp
        Vst/VstFactory.cpp)
endif()

if(DAW_ENABLE_AU)
    # AuInstance is ObjC++: an AU's editor is an NSView the host has to
    # instantiate itself. This is the `.mm` the explicit source list above
    # exists for — a *.cpp glob would drop it and the link error would point
    # nowhere near the cause.
    list(APPEND DAW_PLUGINHOST_SOURCES
        Au/AuFactory.cpp
        Au/AuInstance.mm)
endif()

add_library(daw_pluginhost STATIC ${DAW_PLUGINHOST_SOURCES})

target_include_directories(daw_pluginhost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_pluginhost PUBLIC cxx_std_23)
target_link_libraries(daw_pluginhost PUBLIC daw_engine daw_platform_paths daw_json)

if(DAW_ENABLE_CLAP)
    target_link_libraries(daw_pluginhost PUBLIC daw_clap_headers)
    target_compile_definitions(daw_pluginhost PUBLIC DAW_ENABLE_CLAP=1)
endif()

if(DAW_ENABLE_VST3)
    target_link_libraries(daw_pluginhost PUBLIC daw_vst3_pluginterfaces)
    target_compile_definitions(daw_pluginhost PUBLIC DAW_ENABLE_VST3=1)
    # A macOS VST3 bundle is loaded through CFBundle so the plugin can reach its
    # own resources; nothing else here needs CoreFoundation.
    if(APPLE)
        target_link_libraries(daw_pluginhost PUBLIC "-framework CoreFoundation")
    endif()
endif()

if(DAW_ENABLE_VST)
    target_link_libraries(daw_pluginhost PUBLIC daw_fst_headers)
    target_compile_definitions(daw_pluginhost PUBLIC DAW_ENABLE_VST=1)
    if(APPLE)
        target_link_libraries(daw_pluginhost PUBLIC "-framework CoreFoundation")
    endif()
endif()

if(DAW_ENABLE_AU)
    target_compile_definitions(daw_pluginhost PUBLIC DAW_ENABLE_AU=1)
    target_link_libraries(daw_pluginhost PUBLIC
        "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio"
        "-framework AppKit" "-framework CoreFoundation")
    # ARC, for the ObjC half only: the editor juggles NSView and NSBundle
    # lifetimes and manual retain/release there is a bug farm.
    set_source_files_properties(Au/AuInstance.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
endif()

# dlopen/dlsym live in libdl on Linux; macOS and Windows have them in libc and
# kernel32 respectively.
if(UNIX AND NOT APPLE)
    target_link_libraries(daw_pluginhost PUBLIC ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(daw_pluginhost PRIVATE /W4)
else()
    target_compile_options(daw_pluginhost PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
