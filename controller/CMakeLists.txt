# daw_controller — framework-agnostic application logic.
#
# Document projection, commands and Undo. The executable audio graph lives in
# daw_audio_runtime, which has no dependency on the controller or a UI framework.
file(GLOB_RECURSE DAW_CONTROLLER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntime(Graph|Content|Plugins|PluginUi|PluginRecovery|PluginService|MiniModules|Audition|Control|State|Recording|Session)?\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/ProcessAudioResources\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioSessionCodec\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntimeProcess\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntimeRpc\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntimeReadoutBatch\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntimeDurableEdits\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/AudioRuntimeEndpoint\\.cpp$")
list(FILTER DAW_CONTROLLER_SOURCES EXCLUDE REGEX "/SampleLoader\\.cpp$")

add_library(daw_audio_runtime STATIC AudioRuntime.cpp AudioRuntimeGraph.cpp AudioRuntimeContent.cpp
    AudioRuntimePlugins.cpp AudioRuntimePluginUi.cpp AudioRuntimeControl.cpp AudioRuntimeState.cpp
    AudioRuntimeRecording.cpp AudioRuntimePluginRecovery.cpp AudioRuntimePluginService.cpp AudioRuntimeMiniModules.cpp
    AudioRuntimeAudition.cpp ProcessAudioResources.cpp AudioSessionCodec.cpp AudioRuntimeSession.cpp SampleLoader.cpp)
target_include_directories(daw_audio_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_audio_runtime PUBLIC cxx_std_23)
target_link_libraries(daw_audio_runtime PUBLIC daw_engine daw_core daw_pluginprocess)
target_link_libraries(daw_audio_runtime PRIVATE daw_json)
if(MSVC)
    target_compile_options(daw_audio_runtime PRIVATE /W3 /bigobj)
else()
    target_compile_options(daw_audio_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_library(daw_audioprocess STATIC AudioRuntimeProcess.cpp AudioRuntimeRpc.cpp AudioRuntimeReadoutBatch.cpp AudioRuntimeDurableEdits.cpp)
target_include_directories(daw_audioprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_audioprocess PUBLIC cxx_std_23)
target_link_libraries(daw_audioprocess PUBLIC daw_audio_runtime PRIVATE daw_json)
if(MSVC)
    target_compile_options(daw_audioprocess PRIVATE /W3 /bigobj)
else()
    target_compile_options(daw_audioprocess PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_library(daw_audioendpoint STATIC AudioRuntimeEndpoint.cpp)
target_include_directories(daw_audioendpoint PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_audioendpoint PUBLIC cxx_std_23)
target_link_libraries(daw_audioendpoint PUBLIC daw_audioprocess)
if(MSVC)
    target_compile_options(daw_audioendpoint PRIVATE /W3 /bigobj)
endif()

add_library(daw_controller STATIC ${DAW_CONTROLLER_SOURCES})

target_include_directories(daw_controller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_controller PUBLIC cxx_std_23)
# daw_pluginprocess carries the process adapter and the local plugin formats;
# daw_json is discovered once in third_party/.
target_link_libraries(daw_controller PUBLIC
    daw_audioendpoint daw_pluginprocess daw_json)
add_dependencies(daw_controller daw_plugin_host)
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
