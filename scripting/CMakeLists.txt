# Standard Lua, built into the disposable helper; the GUI never executes Lua.
include(FetchContent)
FetchContent_Declare(vlt_lua
    URL "${PROJECT_SOURCE_DIR}/third_party/lua/lua-5.5.1.tar.gz"
    URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(vlt_lua)
file(GLOB lua_sources "${vlt_lua_SOURCE_DIR}/src/*.c")
list(FILTER lua_sources EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
add_library(vlt_lua STATIC ${lua_sources})
# Lua supports C++ exceptions internally. On Windows this also avoids crossing
# MSVC C++ exception frames with C longjmp when a broker call is rejected.
# Keep the public ABI as the standard Lua C API.
set_source_files_properties(${lua_sources} PROPERTIES LANGUAGE CXX)
if(MSVC)
    target_compile_options(vlt_lua PRIVATE /EHc- "/FI${CMAKE_CURRENT_SOURCE_DIR}/LuaCAbi.hpp")
else()
    target_compile_options(vlt_lua PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/LuaCAbi.hpp")
endif()
target_include_directories(vlt_lua PUBLIC "${vlt_lua_SOURCE_DIR}/src")
# Do not enable LUA_USE_DLOPEN or LUA_USE_WINDOWS: native modules are not an API.
if(UNIX)
    target_link_libraries(vlt_lua PUBLIC m)
endif()
add_library(vlt_script_api STATIC Api.cpp Protocol.cpp Sdk.cpp)
target_include_directories(vlt_script_api PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vlt_script_api PUBLIC daw_json)
add_executable(vlt_script_worker Worker.cpp)
if(MSVC)
    # The C API can propagate Lua's internal exception to lua_pcall. /EHsc's
    # implicit assumption that every extern-C function is noexcept is invalid.
    target_compile_options(vlt_script_worker PRIVATE /EHc-)
endif()
target_link_libraries(vlt_script_worker PRIVATE vlt_lua vlt_script_api)
daw_add_windows_metadata(vlt_script_worker "vlt_script_worker.exe" "vlt_script_worker"
    "VLTONE isolated Lua extension runtime")
if(NOT (DAW_PACKAGE AND APPLE))
    install(TARGETS vlt_script_worker RUNTIME DESTINATION bin)
endif()
add_custom_target(vlt_script_sdk ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/sdk" "${CMAKE_BINARY_DIR}/ScriptSDK"
    COMMAND $<TARGET_FILE:vlt_script_worker> --write-sdk "${CMAKE_BINARY_DIR}/ScriptSDK"
    DEPENDS vlt_script_worker VERBATIM)
install(DIRECTORY "${CMAKE_BINARY_DIR}/ScriptSDK/" DESTINATION share/VLTONE/ScriptSDK)
install(FILES "${PROJECT_SOURCE_DIR}/third_party/lua/LICENSE.txt" DESTINATION share/VLTONE/licenses RENAME Lua.txt)
