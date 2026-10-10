# daw_controller — framework-agnostic application logic.
#
# Document projection, commands and Undo. The executable audio graph lives in
# daw_audio_runtime, which has no dependency on the controller or a UI framework.
file(GLOB_RECURSE DAW_CONTROLLER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntime(Graph|Content|Plugins|PluginUi|PluginLifecycle|PluginService|MiniModules|Audition|Control|State|Recording|Session)?\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioContentValidation\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/SampleLoader\\.cpp$")

add_library(daw_audio_runtime STATIC AudioRuntime.cpp AudioRuntimeGraph.cpp AudioRuntimeContent.cpp
    AudioRuntimePlugins.cpp AudioRuntimePluginUi.cpp AudioRuntimeControl.cpp AudioRuntimeState.cpp
    AudioRuntimeRecording.cpp AudioRuntimePluginLifecycle.cpp AudioRuntimePluginService.cpp AudioRuntimeMiniModules.cpp
    AudioRuntimeAudition.cpp AudioContentValidation.cpp AudioRuntimeSession.cpp SampleLoader.cpp)
target_include_directories(daw_audio_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_audio_runtime PUBLIC cxx_std_23)
target_link_libraries(daw_audio_runtime PUBLIC daw_engine daw_core daw_pluginhost)
target_link_libraries(daw_audio_runtime PRIVATE daw_json)
if(MSVC)
    target_compile_options(daw_audio_runtime PRIVATE /W3 /bigobj)
else()
    target_compile_options(daw_audio_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_library(daw_controller STATIC ${DAW_CONTROLLER_SOURCES})

target_include_directories(daw_controller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_controller PUBLIC cxx_std_23)
# Native hosting is shared by the live runtime and offline render clones.
target_link_libraries(daw_controller PUBLIC
    daw_audio_runtime daw_process daw_json)
include(${CMAKE_SOURCE_DIR}/cmake/AudioAnalysis.cmake)
if(DAW_ENFORCE_COLLABORATION_RELEASE_GATES)
    target_compile_definitions(daw_controller PRIVATE
        DAW_ENFORCE_COLLABORATION_RELEASE_GATES=1)
endif()
if(WIN32)
    target_link_libraries(daw_controller PRIVATE Dbghelp)
endif()

if(MSVC)
    # EngineController and the command-v2 reducer intentionally instantiate a
    # large std::variant visitor surface; release/PDB builds exceed COFF's
    # default section count without the extended object format.
    target_compile_options(daw_controller PRIVATE /W3 /bigobj)
else()
    target_compile_options(daw_controller PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
