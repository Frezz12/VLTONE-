# daw_controller — framework-agnostic application logic.
#
# Replaces the ObjC++ AudioEngineBridge and the Swift service layer: owns the
# realtime engine and the document model, exposes a clean C++ API to any UI.
# Depends only on daw_core and nlohmann/json — no UI framework.
file(GLOB_RECURSE DAW_CONTROLLER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(daw_controller STATIC ${DAW_CONTROLLER_SOURCES})

target_include_directories(daw_controller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(daw_controller PUBLIC cxx_std_23)
# daw_pluginhost carries the plugin formats and the scanner's wire protocol;
# daw_json is discovered once in third_party/.
target_link_libraries(daw_controller PUBLIC
    daw_engine daw_core daw_pluginhost daw_json)
if(DAW_ENFORCE_COLLABORATION_RELEASE_GATES)
    target_compile_definitions(daw_controller PRIVATE
        DAW_ENFORCE_COLLABORATION_RELEASE_GATES=1)
endif()
if(WIN32)
    target_link_libraries(daw_controller PRIVATE Dbghelp)
endif()

if(MSVC)
    target_compile_options(daw_controller PRIVATE /W3)
else()
    target_compile_options(daw_controller PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
